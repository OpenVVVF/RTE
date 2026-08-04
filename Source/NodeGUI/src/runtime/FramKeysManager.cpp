#include "FramKeysManager.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

#include <regex>

namespace NodeGUI::runtime {

namespace {

// Matches the `config list` header line:
//   [SHELL] stored config keys (N):
const std::regex kListHeaderRegex(
    R"(^\s*\[SHELL\]\s+stored config keys\s*\(\s*\d+\s*\)\s*:\s*$)");

// Matches lines produced by `config list`:
//   [SHELL]   Some.Key = 1.2340
const std::regex kKeyValueRegex(
    R"(^\s*(?:\[SHELL\]\s+)?(\S+)\s*=\s*([-+]?\d*\.?\d+(?:[eE][-+]?\d+)?)\s*$)");

// How long to wait after the console stops growing before reading it.
constexpr int kQuietTimeoutMs = 250;
// Hard ceiling for a single command response.
constexpr int kMaxWaitMs = 3000;
// Poll interval while waiting.
constexpr int kPollIntervalMs = 25;
// How many times to retry `config list` if the device does not respond.
constexpr int kListRetries = 3;

// Wait until no new console lines have arrived for kQuietTimeoutMs, or until
// kMaxWaitMs has elapsed.  Returns false only if waiting was aborted early.
bool WaitForConsoleQuiet(RuntimeController* controller) {
    int elapsedMs = 0;
    int stableMs = 0;
    std::size_t lastSize = 0;

    while (elapsedMs < kMaxWaitMs) {
        QThread::msleep(kPollIntervalMs);
        elapsedMs += kPollIntervalMs;

        const auto snap = controller->Store().Snapshot();
        if (snap.console.size() == lastSize) {
            stableMs += kPollIntervalMs;
            if (stableMs >= kQuietTimeoutMs) {
                return true;
            }
        } else {
            stableMs = 0;
            lastSize = snap.console.size();
        }
    }
    return true;
}

// Scans the entire console for the most recent `config list` output and
// extracts its keys.  This avoids the need to pair responses to commands by
// sequence number, which breaks when a slow response to an earlier command
// arrives while a later command is in flight.
bool ExtractLatestConfigList(const std::deque<ConsoleLine>& console,
                             QJsonObject& outKeys) {
    outKeys = QJsonObject();

    // Find the last header line.
    int headerIndex = -1;
    for (int i = static_cast<int>(console.size()) - 1; i >= 0; --i) {
        if (std::regex_match(console[i].text, kListHeaderRegex)) {
            headerIndex = i;
            break;
        }
    }
    if (headerIndex < 0) {
        return false;
    }

    // Collect key/value lines that follow this header, stopping at the next
    // header (if any) or at the end of the console.
    for (std::size_t i = static_cast<std::size_t>(headerIndex) + 1;
         i < console.size();
         ++i) {
        const auto& text = console[i].text;
        if (std::regex_match(text, kListHeaderRegex)) {
            break;
        }
        std::smatch match;
        if (std::regex_match(text, match, kKeyValueRegex)) {
            const std::string key = match[1].str();
            const double value = std::stod(match[2].str());
            outKeys[QString::fromStdString(key)] = value;
        }
    }
    return true;
}

}  // namespace

FramKeysManager::FramKeysManager(RuntimeController* controller, QObject* parent)
    : QObject(parent)
    , controller_(controller) {}

bool FramKeysManager::SaveToFile(const QString& path, QString& error) {
    if (!controller_ || controller_->IsSimulating()) {
        error = QStringLiteral("FRAM access requires a connected device.");
        return false;
    }

    for (int attempt = 0; attempt < kListRetries; ++attempt) {
        if (!controller_->SendCommand(QStringLiteral("config list"))) {
            error = QStringLiteral("Failed to send 'config list'.");
            return false;
        }

        WaitForConsoleQuiet(controller_);

        QJsonObject keys;
        const auto snap = controller_->Store().Snapshot();
        if (ExtractLatestConfigList(snap.console, keys)) {
            QJsonDocument doc(keys);
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
                error = QStringLiteral("Cannot write %1: %2")
                            .arg(path, file.errorString());
                return false;
            }
            file.write(doc.toJson(QJsonDocument::Indented));
            return true;
        }

        if (attempt + 1 < kListRetries) {
            QThread::msleep(100);
        }
    }

    error = QStringLiteral(
        "Device did not return a valid config list after %1 attempts. "
        "Check that the device console is responding.").arg(kListRetries);
    return false;
}

bool FramKeysManager::LoadFromFile(const QString& path,
                                   bool clearFirst,
                                   QString& error) {
    if (!controller_ || controller_->IsSimulating()) {
        error = QStringLiteral("FRAM access requires a connected device.");
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        error = QStringLiteral("Cannot read %1: %2")
                    .arg(path, file.errorString());
        return false;
    }

    const QByteArray data = file.readAll();
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        error = QStringLiteral("JSON parse error: %1").arg(parseError.errorString());
        return false;
    }
    if (!doc.isObject()) {
        error = QStringLiteral("File must contain a JSON object {\"key\": value, ...}.");
        return false;
    }

    const QJsonObject obj = doc.object();
    if (obj.isEmpty()) {
        error = QStringLiteral("No keys found in %1.").arg(path);
        return false;
    }

    // Build an ordered map so the write order is deterministic.
    std::map<QString, double> keys;
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (!it.value().isDouble()) {
            error = QStringLiteral("Value for '%1' is not a number.").arg(it.key());
            return false;
        }
        keys[it.key()] = it.value().toDouble();
    }

    if (clearFirst) {
        if (!controller_->SendCommand(QStringLiteral("config deleteall"))) {
            error = QStringLiteral("Failed to send 'config deleteall'.");
            return false;
        }
        WaitForConsoleQuiet(controller_);
    }

    int written = 0;
    for (const auto& [key, value] : keys) {
        // `config set` updates live graph config nodes but does not persist
        // them; follow with `config save` to flush to FRAM.  For raw KV-store
        // keys the save fails harmlessly while the set already flushed.
        const QString setCmd = QStringLiteral("config set %1 %2")
                                   .arg(key)
                                   .arg(value, 0, 'g', -1);
        const QString saveCmd = QStringLiteral("config save %1").arg(key);

        if (!controller_->SendCommand(setCmd)) {
            error = QStringLiteral("Failed to send '%1'.").arg(setCmd);
            return false;
        }
        WaitForConsoleQuiet(controller_);

        if (!controller_->SendCommand(saveCmd)) {
            error = QStringLiteral("Failed to send '%1'.").arg(saveCmd);
            return false;
        }
        WaitForConsoleQuiet(controller_);

        ++written;

        // Keep the GUI responsive while writing many keys.
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    error = QStringLiteral("Loaded %1 key(s) from %2.").arg(written).arg(path);
    return true;
}

}  // namespace NodeGUI::runtime
