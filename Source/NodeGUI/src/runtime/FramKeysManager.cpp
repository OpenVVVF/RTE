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

// How long to wait after the last new console line before deciding the
// response is complete.
constexpr int kQuietTimeoutMs = 250;
// Hard ceiling for a single command response.
constexpr int kMaxWaitMs = 3000;
// Poll interval while waiting.
constexpr int kPollIntervalMs = 25;
// How many times to retry `config list` if the device does not respond.
constexpr int kListRetries = 3;

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
        std::vector<std::string> lines;
        if (!SendCommandAndCollect(QStringLiteral("config list"), lines, error)) {
            return false;
        }

        bool gotHeader = false;
        QJsonObject obj;
        for (const auto& line : lines) {
            if (std::regex_match(line, kListHeaderRegex)) {
                gotHeader = true;
                continue;
            }
            std::smatch match;
            if (std::regex_match(line, match, kKeyValueRegex)) {
                const std::string key = match[1].str();
                const double value = std::stod(match[2].str());
                obj[QString::fromStdString(key)] = value;
            }
        }

        if (gotHeader) {
            QJsonDocument doc(obj);
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
        std::vector<std::string> discard;
        if (!SendCommandAndCollect(QStringLiteral("config deleteall"), discard, error)) {
            return false;
        }
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

        std::vector<std::string> discard;
        if (!SendCommandAndCollect(setCmd, discard, error)) {
            return false;
        }
        if (!SendCommandAndCollect(saveCmd, discard, error)) {
            return false;
        }
        ++written;

        // Keep the GUI responsive while writing many keys.
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    error = QStringLiteral("Loaded %1 key(s) from %2.").arg(written).arg(path);
    return true;
}

bool FramKeysManager::SendCommandAndCollect(const QString& line,
                                            std::vector<std::string>& responseLines,
                                            QString& error) {
    responseLines.clear();

    if (!controller_->SendCommand(line)) {
        error = QStringLiteral("Failed to send command: %1").arg(line);
        return false;
    }

    // Record the last console sequence number before the response arrives.
    uint64_t before = 0;
    {
        const auto snap = controller_->Store().Snapshot();
        if (!snap.console.empty()) {
            before = snap.console.back().seq;
        }
    }

    int elapsedMs = 0;
    int quietMs = 0;
    uint64_t maxSeen = before;

    while (elapsedMs < kMaxWaitMs) {
        QThread::msleep(kPollIntervalMs);
        elapsedMs += kPollIntervalMs;

        const auto snap = controller_->Store().Snapshot();
        bool gotNew = false;
        for (const auto& item : snap.console) {
            if (item.seq > maxSeen) {
                responseLines.push_back(item.text);
                maxSeen = item.seq;
                gotNew = true;
            }
        }

        if (gotNew) {
            quietMs = 0;
        } else if (maxSeen > before) {
            quietMs += kPollIntervalMs;
            if (quietMs >= kQuietTimeoutMs) {
                return true;
            }
        }
    }

    // Timed out, but we may still have partial data; don't treat it as fatal.
    return true;
}

}  // namespace NodeGUI::runtime
