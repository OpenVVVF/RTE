#include "SimSession.h"

#include "runtime/TelemetryStore.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QRegularExpression>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <limits>

namespace NodeGUI::sim {

namespace {

// Naive (QJsonDocument-free) scanners for SelfCheck: same byte stream, a
// completely different code path, so the two reads cross-validate.
int NaiveFrameCount(const QString& contents) {
    static const QRegularExpression pattern(
        QStringLiteral("\"type\"\\s*:\\s*\"telemetry\""));
    int count = 0;
    auto it = pattern.globalMatch(contents);
    while (it.hasNext()) {
        it.next();
        ++count;
    }
    return count;
}

// Every numeric occurrence of "<name>": <number> in the file (json null is
// skipped by construction). Finite samples only.
QVector<double> NaiveChannelValues(const QString& contents, const QString& name) {
    const QRegularExpression pattern(
        QStringLiteral("\"") + QRegularExpression::escape(name)
        + QStringLiteral("\"\\s*:\\s*(-?(?:\\d+(?:\\.\\d+)?|\\.\\d+)(?:[eE][+-]?\\d+)?)"));
    QVector<double> values;
    auto it = pattern.globalMatch(contents);
    while (it.hasNext()) {
        const auto match = it.next();
        bool ok = false;
        const double value = match.captured(1).toDouble(&ok);
        if (ok) {
            values.push_back(value);
        }
    }
    return values;
}

QString ReadWholeFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

}  // namespace

bool SimSession::Load(const QString& path, QString& error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        error = QStringLiteral("could not open: %1").arg(path);
        return false;
    }

    meta_ = SimSessionMeta{};
    channels_.clear();
    console_.clear();

    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        QJsonParseError parseError;
        const QJsonDocument document =
            QJsonDocument::fromJson(line.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            ++meta_.badLines;
            continue;
        }
        const QJsonObject record = document.object();
        const QJsonValue timeValue = record.value(QStringLiteral("t"));
        if (timeValue.isDouble() && std::isfinite(timeValue.toDouble())) {
            meta_.durationS = std::max(meta_.durationS, timeValue.toDouble());
        }
        const QString type = record.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("session_start")) {
            meta_.version = record.value(QStringLiteral("version")).toInt(0);
            meta_.scenario =
                record.value(QStringLiteral("scenario")).toString();
            meta_.plant = record.value(QStringLiteral("plant")).toString();
            meta_.mode = record.value(QStringLiteral("mode")).toString();
        } else if (type == QLatin1String("telemetry")) {
            if (!timeValue.isDouble()) {
                ++meta_.badLines;
                continue;
            }
            const double tsec = timeValue.toDouble();
            const QJsonObject values =
                record.value(QStringLiteral("values")).toObject();
            for (auto it = values.begin(); it != values.end(); ++it) {
                const std::string name = it.key().toStdString();
                Channel& channel = channels_[name];
                const float value =
                    it.value().isDouble()
                        ? static_cast<float>(it.value().toDouble())
                        : std::numeric_limits<float>::quiet_NaN();
                channel.t.push_back(static_cast<float>(tsec));
                channel.y.push_back(value);

                SimChannelStats& stats = channel.stats;
                ++stats.samples;
                if (std::isfinite(value)) {
                    const double dv = static_cast<double>(value);
                    if (stats.finiteSamples == 0) {
                        stats.minimum = dv;
                        stats.maximum = dv;
                    } else {
                        stats.minimum = std::min(stats.minimum, dv);
                        stats.maximum = std::max(stats.maximum, dv);
                    }
                    ++stats.finiteSamples;
                    stats.lastFinite = dv;
                    stats.haveLastFinite = true;
                }
            }
            ++meta_.telemetryFrames;
        } else if (type == QLatin1String("console")) {
            const QString text = record.value(QStringLiteral("text")).toString();
            console_.push_back(QStringLiteral("[%1] %2")
                                   .arg(timeValue.isDouble()
                                            ? QString::number(timeValue.toDouble(), 'f', 3)
                                            : QStringLiteral("?"),
                                        text));
            ++meta_.consoleLines;
        }
        // state / command / session_end and anything newer: tolerated and
        // ignored (their "t" still fed durationS above).
    }

    if (channels_.empty()) {
        error = QStringLiteral("no telemetry channels found in: %1").arg(path);
        return false;
    }
    error.clear();
    return true;
}

QStringList SimSession::ChannelNames() const {
    QStringList names;
    names.reserve(static_cast<qsizetype>(channels_.size()));
    for (const auto& [name, channel] : channels_) {
        (void)channel;
        names.push_back(QString::fromStdString(name));
    }
    names.sort();
    return names;
}

SimChannelStats SimSession::ChannelStats(const QString& name) const {
    const auto it = channels_.find(name.toStdString());
    if (it == channels_.end()) {
        return {};
    }
    return it->second.stats;
}

bool SimSession::ChannelHistory(const QString& name,
                                std::vector<float>& t,
                                std::vector<float>& y) const {
    const auto it = channels_.find(name.toStdString());
    if (it == channels_.end()) {
        return false;
    }
    t = it->second.t;
    y = it->second.y;
    return true;
}

void SimSession::PopulateStore(NodeGUI::runtime::TelemetryStore& store) const {
    for (const auto& [name, channel] : channels_) {
        const std::size_t count =
            std::min(channel.t.size(), channel.y.size());
        for (std::size_t i = 0; i < count; ++i) {
            if (std::isfinite(channel.y[i])) {
                store.AddF32(name, channel.y[i], channel.t[i]);
            }
        }
    }
}

bool SimSession::SelfCheck(const QString& path, QString& report) {
    bool ok = true;
    auto check = [&report, &ok](bool pass, const QString& text) {
        report += QStringLiteral("[%1] %2\n")
                      .arg(pass ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
                           text);
        ok = ok && pass;
    };

    const QString contents = ReadWholeFile(path);
    check(!contents.isEmpty(), QStringLiteral("file readable: %1").arg(path));
    if (contents.isEmpty()) {
        return false;
    }

    SimSession session;
    QString error;
    check(session.Load(path, error),
          QStringLiteral("loader accepts the file (%1)").arg(error));

    // 1. Telemetry frame count matches the naive line scan.
    const int naiveFrames = NaiveFrameCount(contents);
    check(session.Meta().telemetryFrames == naiveFrames,
          QStringLiteral("telemetry frames: loader %1 == naive scan %2")
              .arg(session.Meta().telemetryFrames)
              .arg(naiveFrames));

    // 2. Per-channel values match the naive scan (two reference channels
    //    when the file has them, else the first two channels overall).
    QStringList reference;
    for (const QString& preferred : {QStringLiteral("cg_iq_a"),
                                     QStringLiteral("vdc_v")}) {
        if (reference.size() < 2 && session.ChannelStats(preferred).samples > 0) {
            reference.push_back(preferred);
        }
    }
    const QStringList all = session.ChannelNames();
    for (const QString& name : all) {
        if (reference.size() >= 2) {
            break;
        }
        if (!reference.contains(name)) {
            reference.push_back(name);
        }
    }
    for (const QString& name : reference) {
        const QVector<double> naive = NaiveChannelValues(contents, name);
        std::vector<float> t;
        std::vector<float> y;
        session.ChannelHistory(name, t, y);
        int finite = 0;
        float first = 0.0f;
        float last = 0.0f;
        for (const float value : y) {
            if (std::isfinite(value)) {
                if (finite == 0) {
                    first = value;
                }
                last = value;
                ++finite;
            }
        }
        check(finite == naive.size(),
              QStringLiteral("%1 finite samples: loader %2 == naive scan %3")
                  .arg(name)
                  .arg(finite)
                  .arg(naive.size()));
        if (finite > 0 && naive.size() == finite) {
            auto closeEnough = [](float a, double b) {
                return std::abs(static_cast<double>(a) - b)
                       <= 1e-6 * std::max(1.0, std::abs(b));
            };
            check(closeEnough(first, naive.first()),
                  QStringLiteral("%1 first sample: loader %2 == naive %3")
                      .arg(name)
                      .arg(static_cast<double>(first))
                      .arg(naive.first()));
            check(closeEnough(last, naive.last()),
                  QStringLiteral("%1 last sample: loader %2 == naive %3")
                      .arg(name)
                      .arg(static_cast<double>(last))
                      .arg(naive.last()));
        }
    }

    // 3. The TelemetryStore bridge carries the same tails.
    NodeGUI::runtime::TelemetryStore store;
    session.PopulateStore(store);
    const std::vector<std::string> storeNames = store.SignalNames();
    int bridgedFiniteChannels = 0;
    for (const QString& name : all) {
        if (session.ChannelStats(name).finiteSamples > 0) {
            ++bridgedFiniteChannels;
        }
    }
    check(static_cast<int>(storeNames.size()) == bridgedFiniteChannels,
          QStringLiteral("store channels: %1 == finite channels %2")
              .arg(storeNames.size())
              .arg(bridgedFiniteChannels));
    for (const QString& name : reference.mid(0, 1)) {
        std::vector<float> t;
        std::vector<float> y;
        session.ChannelHistory(name, t, y);
        float lastFinite = 0.0f;
        int finite = 0;
        for (const float value : y) {
            if (std::isfinite(value)) {
                lastFinite = value;
                ++finite;
            }
        }
        std::vector<float> storeT;
        std::vector<float> storeY;
        const bool copied =
            store.CopyHistoryInto(name.toStdString(), storeT, storeY);
        check(copied && static_cast<int>(storeY.size()) == finite,
              QStringLiteral("store history of %1: %2 samples == %3 finite")
                  .arg(name)
                  .arg(storeY.size())
                  .arg(finite));
        check(!storeY.empty() && storeY.back() == lastFinite,
              QStringLiteral("store tail of %1: %2 == loader %3")
                  .arg(name)
                  .arg(storeY.empty() ? 0.0f : storeY.back())
                  .arg(lastFinite));
    }

    report += QStringLiteral("self-check %1: %2\n")
                  .arg(ok ? QStringLiteral("OK") : QStringLiteral("FAILED"),
                       path);
    return ok;
}

}  // namespace NodeGUI::sim
