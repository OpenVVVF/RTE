#include "RuntimeSessionExporter.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTextStream>
#include <QTimeZone>

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <set>
#include <string>
#include <vector>

namespace NodeGUI::runtime {

namespace {

QString JsonString(const QString& value) {
    QJsonArray array;
    array.append(value);
    const QByteArray json =
        QJsonDocument(array).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}

QString JsonString(const std::string& value) {
    return JsonString(QString::fromUtf8(
        value.data(), static_cast<qsizetype>(value.size())));
}

template <typename Number>
void WriteNumber(QTextStream& stream, Number value) {
    if (!std::isfinite(static_cast<double>(value))) {
        stream << QStringLiteral("null");
        return;
    }
    stream << QString::number(
        value, 'g', std::numeric_limits<Number>::max_digits10);
}

void WriteTime(QTextStream& stream, double tsec) {
    if (!std::isfinite(tsec)) {
        stream << QStringLiteral("null");
        return;
    }
    QString value = QString::number(tsec, 'f', 6);
    while (value.contains('.') && value.endsWith('0')) {
        value.chop(1);
    }
    if (value.endsWith('.')) {
        value.chop(1);
    }
    stream << value;
}

template <typename Map>
std::vector<std::string> SortedKeys(const Map& values) {
    std::vector<std::string> keys;
    keys.reserve(values.size());
    for (const auto& [key, value] : values) {
        (void)value;
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

void WriteStatsObject(QTextStream& stream, const TelemetryStats& stats) {
    stream << "{\"rx_hz\":";
    WriteNumber(stream, stats.rxHz);
    stream << ",\"rx_bytes_per_second\":";
    WriteNumber(stream, stats.rxBytesPerSec);
    stream << ",\"good_frames\":" << stats.goodFrames
           << ",\"bad_frames\":" << stats.badFrames
           << ",\"reject_crc\":" << stats.rejectCrc
           << ",\"reject_header\":" << stats.rejectHdr
           << ",\"reject_length\":" << stats.rejectLen
           << ",\"reject_payload_parse\":" << stats.rejectPayloadParse
           << ",\"reject_unknown_id\":" << stats.rejectUnknownId
           << ",\"last_sequence\":" << stats.lastSeq
           << ",\"suspended\":" << (stats.suspended ? "true" : "false")
           << '}';
}

void WriteSignalSummaries(
    QTextStream& stream,
    const std::unordered_map<std::string, SessionSignalHistory>&
        signalHistories) {
    stream << '{';
    const auto keys = SortedKeys(signalHistories);
    for (std::size_t keyIndex = 0; keyIndex < keys.size(); ++keyIndex) {
        if (keyIndex != 0) {
            stream << ',';
        }
        const auto& key = keys[keyIndex];
        const auto& values = signalHistories.at(key).y;
        std::size_t finiteSamples = 0;
        double sum = 0.0;
        float minimum = 0.0f;
        float maximum = 0.0f;
        for (const float value : values) {
            if (!std::isfinite(value)) {
                continue;
            }
            if (finiteSamples == 0) {
                minimum = value;
                maximum = value;
            } else {
                minimum = std::min(minimum, value);
                maximum = std::max(maximum, value);
            }
            ++finiteSamples;
            sum += value;
        }

        stream << JsonString(key)
               << ":{\"samples\":" << values.size()
               << ",\"finite_samples\":" << finiteSamples
               << ",\"min\":";
        if (finiteSamples == 0) {
            stream << "null,\"max\":null,\"mean\":null}";
            continue;
        }
        WriteNumber(stream, minimum);
        stream << ",\"max\":";
        WriteNumber(stream, maximum);
        stream << ",\"mean\":";
        WriteNumber(stream, sum / static_cast<double>(finiteSamples));
        stream << '}';
    }
    stream << '}';
}

enum class EventKind {
    FloatTelemetry,
    StringTelemetry,
    Command,
    Console,
};

struct EventCursor {
    EventKind kind = EventKind::FloatTelemetry;
    std::string key;
    std::size_t index = 0;
    double tsec = 0.0;
};

double SortableTime(double tsec) {
    return std::isfinite(tsec)
               ? tsec
               : std::numeric_limits<double>::infinity();
}

struct LaterEvent {
    bool operator()(const EventCursor& left,
                    const EventCursor& right) const {
        const double leftTime = SortableTime(left.tsec);
        const double rightTime = SortableTime(right.tsec);
        if (leftTime != rightTime) {
            return leftTime > rightTime;
        }
        if (left.kind != right.kind) {
            return left.kind > right.kind;
        }
        return left.key > right.key;
    }
};

using EventQueue =
    std::priority_queue<EventCursor,
                        std::vector<EventCursor>,
                        LaterEvent>;

void PushNext(EventQueue& events,
              const RuntimeSessionSnapshot& session,
              EventCursor cursor) {
    ++cursor.index;
    switch (cursor.kind) {
    case EventKind::FloatTelemetry: {
        const auto& history = session.floatSignals.at(cursor.key);
        if (cursor.index < std::min(history.t.size(), history.y.size())) {
            cursor.tsec = history.t[cursor.index];
            events.push(std::move(cursor));
        }
        break;
    }
    case EventKind::StringTelemetry: {
        const auto& samples = session.stringSignals.at(cursor.key);
        if (cursor.index < samples.size()) {
            cursor.tsec = samples[cursor.index].tsec;
            events.push(std::move(cursor));
        }
        break;
    }
    case EventKind::Command:
        if (cursor.index < session.commands.size()) {
            cursor.tsec = session.commands[cursor.index].tsec;
            events.push(std::move(cursor));
        }
        break;
    case EventKind::Console:
        if (cursor.index < session.console.size()) {
            cursor.tsec = session.console[cursor.index].tsec;
            events.push(std::move(cursor));
        }
        break;
    }
}

void PopulateEvents(EventQueue& events,
                    const RuntimeSessionSnapshot& session) {
    for (const auto& key : SortedKeys(session.floatSignals)) {
        const auto& history = session.floatSignals.at(key);
        if (!history.t.empty() && !history.y.empty()) {
            events.push(
                EventCursor{EventKind::FloatTelemetry, key, 0, history.t[0]});
        }
    }
    for (const auto& key : SortedKeys(session.stringSignals)) {
        const auto& samples = session.stringSignals.at(key);
        if (!samples.empty()) {
            events.push(EventCursor{
                EventKind::StringTelemetry, key, 0, samples[0].tsec});
        }
    }
    if (!session.commands.empty()) {
        events.push(EventCursor{
            EventKind::Command, {}, 0, session.commands[0].tsec});
    }
    if (!session.console.empty()) {
        events.push(EventCursor{
            EventKind::Console, {}, 0, session.console[0].tsec});
    }
}

double WriteEvents(QTextStream& stream,
                   const RuntimeSessionSnapshot& session) {
    EventQueue events;
    PopulateEvents(events, session);
    double lastEventTime = 0.0;

    while (!events.empty()) {
        EventCursor cursor = events.top();
        events.pop();
        if (std::isfinite(cursor.tsec)) {
            lastEventTime = std::max(lastEventTime, cursor.tsec);
        }

        if (cursor.kind == EventKind::FloatTelemetry) {
            const double eventTime = cursor.tsec;
            std::vector<std::pair<std::string, float>> values;
            std::set<std::string> includedSignals;
            while (true) {
                const auto& history =
                    session.floatSignals.at(cursor.key);
                values.emplace_back(cursor.key, history.y[cursor.index]);
                includedSignals.insert(cursor.key);
                PushNext(events, session, cursor);

                if (events.empty() ||
                    events.top().kind != EventKind::FloatTelemetry ||
                    events.top().tsec != eventTime ||
                    includedSignals.contains(events.top().key)) {
                    break;
                }
                cursor = events.top();
                events.pop();
            }

            stream << "{\"type\":\"telemetry\",\"t\":";
            WriteTime(stream, eventTime);
            stream << ",\"values\":{";
            for (std::size_t i = 0; i < values.size(); ++i) {
                if (i != 0) {
                    stream << ',';
                }
                stream << JsonString(values[i].first) << ':';
                WriteNumber(stream, values[i].second);
            }
            stream << "}}\n";
            continue;
        }

        if (cursor.kind == EventKind::StringTelemetry) {
            const auto& sample =
                session.stringSignals.at(cursor.key)[cursor.index];
            stream << "{\"type\":\"state\",\"t\":";
            WriteTime(stream, sample.tsec);
            stream << ",\"signal\":" << JsonString(cursor.key)
                   << ",\"value\":" << JsonString(sample.value)
                   << "}\n";
        } else if (cursor.kind == EventKind::Command) {
            const auto& command = session.commands[cursor.index];
            stream << "{\"type\":\"command\",\"t\":";
            WriteTime(stream, command.tsec);
            stream << ",\"received_t\":";
            WriteTime(stream, command.receivedTsec);
            stream << ",\"source\":" << JsonString(command.source)
                   << ",\"text\":" << JsonString(command.text)
                   << ",\"sent\":"
                   << (command.sent ? "true" : "false")
                   << "}\n";
        } else {
            const auto& line = session.console[cursor.index];
            stream << "{\"type\":\"console\",\"t\":";
            WriteTime(stream, line.tsec);
            stream << ",\"sequence\":" << line.seq
                   << ",\"text\":" << JsonString(line.text)
                   << "}\n";
        }
        PushNext(events, session, cursor);
    }
    return lastEventTime;
}

struct SignalSummary {
    std::size_t samples = 0;
    std::size_t finiteSamples = 0;
    double sum = 0.0;
    float minimum = 0.0f;
    float maximum = 0.0f;

    void Add(float value) {
        ++samples;
        if (!std::isfinite(value)) return;
        if (finiteSamples == 0) {
            minimum = maximum = value;
        } else {
            minimum = std::min(minimum, value);
            maximum = std::max(maximum, value);
        }
        ++finiteSamples;
        sum += value;
    }
};

void WriteSignalSummaries(QTextStream& stream,
                          const std::unordered_map<std::string, SignalSummary>& summaries) {
    stream << '{';
    const auto keys = SortedKeys(summaries);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (i) stream << ',';
        const auto& summary = summaries.at(keys[i]);
        stream << JsonString(keys[i]) << ":{\"samples\":" << summary.samples
               << ",\"finite_samples\":" << summary.finiteSamples
               << ",\"min\":";
        if (!summary.finiteSamples) {
            stream << "null,\"max\":null,\"mean\":null}";
        } else {
            WriteNumber(stream, summary.minimum);
            stream << ",\"max\":";
            WriteNumber(stream, summary.maximum);
            stream << ",\"mean\":";
            WriteNumber(stream, summary.sum / summary.finiteSamples);
            stream << '}';
        }
    }
    stream << '}';
}

bool WriteEventsPaged(QTextStream& stream, const TelemetryStore& store,
                      const RuntimeSessionSnapshot& metadata,
                      const TelemetryStore::SessionRetentionStats& retention,
                      double& lastEventTime,
                      std::unordered_map<std::string, SignalSummary>& summaries,
                      QString& error) {
    constexpr std::size_t kPageSamples = 2048;
    struct Page {
        std::size_t offset = 0;
        std::size_t end = 0;
        std::vector<SessionFloatSample> samples;
    };
    std::unordered_map<std::string, Page> pages;
    EventQueue events;
    PopulateEvents(events, metadata);  // strings, commands, and console
    for (const auto& key : store.SessionFloatNames()) {
        Page page;
        uint64_t generation = 0;
        if (!store.CopySessionHistoryPage(key, 0, kPageSamples,
                                           page.samples, page.end, generation)) continue;
        if (generation != retention.archiveGeneration) {
            error = QStringLiteral("Session archive changed during export; retry");
            return false;
        }
        if (!page.samples.empty()) {
            events.push(EventCursor{EventKind::FloatTelemetry, key, 0,
                                    page.samples.front().t});
            summaries.emplace(key, SignalSummary{});
        }
        pages.emplace(key, std::move(page));
    }
    auto loadSample = [&](const std::string& key, std::size_t index,
                          SessionFloatSample& sample) -> bool {
        auto& page = pages.at(key);
        if (index >= page.end) return false;
        if (index < page.offset || index >= page.offset + page.samples.size()) {
            uint64_t generation = 0;
            std::size_t total = 0;
            if (!store.CopySessionHistoryPage(key, index, kPageSamples,
                                               page.samples, total, generation)
                || generation != retention.archiveGeneration
                || store.SessionEpoch() != retention.sessionEpoch || total < page.end) {
                error = QStringLiteral("Session archive changed during export; retry");
                return false;
            }
            page.offset = index;
            if (page.samples.empty()) {
                error = QStringLiteral("Missing session history page during export");
                return false;
            }
        }
        sample = page.samples[index - page.offset];
        return true;
    };
    auto pushNext = [&](EventCursor cursor) -> bool {
        ++cursor.index;
        if (cursor.kind == EventKind::FloatTelemetry) {
            if (cursor.index >= pages.at(cursor.key).end) return true;
            SessionFloatSample sample;
            if (!loadSample(cursor.key, cursor.index, sample)) return false;
            cursor.tsec = sample.t;
            events.push(std::move(cursor));
        } else {
            PushNext(events, metadata, std::move(cursor));
        }
        return true;
    };
    lastEventTime = 0.0;
    while (!events.empty()) {
        EventCursor cursor = events.top();
        events.pop();
        if (std::isfinite(cursor.tsec))
            lastEventTime = std::max(lastEventTime, cursor.tsec);
        if (cursor.kind == EventKind::FloatTelemetry) {
            const double eventTime = cursor.tsec;
            std::vector<std::pair<std::string, float>> values;
            std::set<std::string> includedSignals;
            while (true) {
                SessionFloatSample sample;
                if (!loadSample(cursor.key, cursor.index, sample)) return false;
                values.emplace_back(cursor.key, sample.y);
                summaries.at(cursor.key).Add(sample.y);
                includedSignals.insert(cursor.key);
                if (!pushNext(cursor)) return false;
                if (events.empty() || events.top().kind != EventKind::FloatTelemetry
                    || events.top().tsec != eventTime
                    || includedSignals.contains(events.top().key)) break;
                cursor = events.top();
                events.pop();
            }
            stream << "{\"type\":\"telemetry\",\"t\":";
            WriteTime(stream, eventTime);
            stream << ",\"values\":{";
            for (std::size_t i = 0; i < values.size(); ++i) {
                if (i) stream << ',';
                stream << JsonString(values[i].first) << ':';
                WriteNumber(stream, values[i].second);
            }
            stream << "}}\n";
        } else if (cursor.kind == EventKind::StringTelemetry) {
            const auto& sample = metadata.stringSignals.at(cursor.key)[cursor.index];
            stream << "{\"type\":\"state\",\"t\":";
            WriteTime(stream, sample.tsec);
            stream << ",\"signal\":" << JsonString(cursor.key)
                   << ",\"value\":" << JsonString(sample.value) << "}\n";
            if (!pushNext(cursor)) return false;
        } else if (cursor.kind == EventKind::Command) {
            const auto& command = metadata.commands[cursor.index];
            stream << "{\"type\":\"command\",\"t\":";
            WriteTime(stream, command.tsec);
            stream << ",\"received_t\":";
            WriteTime(stream, command.receivedTsec);
            stream << ",\"source\":" << JsonString(command.source)
                   << ",\"text\":" << JsonString(command.text)
                   << ",\"sent\":" << (command.sent ? "true" : "false") << "}\n";
            if (!pushNext(cursor)) return false;
        } else {
            const auto& line = metadata.console[cursor.index];
            stream << "{\"type\":\"console\",\"t\":";
            WriteTime(stream, line.tsec);
            stream << ",\"sequence\":" << line.seq
                   << ",\"text\":" << JsonString(line.text) << "}\n";
            if (!pushNext(cursor)) return false;
        }
    }
    return true;
}

}  // namespace

bool ExportRuntimeSession(const QString& path,
                          const RuntimeSessionSnapshot& session,
                          const RuntimeSessionMetadata& metadata,
                          QString& error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        error = file.errorString();
        return false;
    }

    QTextStream stream(&file);
    const QDateTime startedAt =
        QDateTime::fromMSecsSinceEpoch(session.startedAtUnixMs,
                                       QTimeZone::UTC);
    const QDateTime exportedAt = QDateTime::currentDateTimeUtc();

    stream << "{\"type\":\"session_start\""
           << ",\"format\":\"rte-runtime-session-jsonl\""
           << ",\"version\":3"
           << ",\"t\":0"
           << ",\"started_at_utc\":"
           << JsonString(startedAt.toString(Qt::ISODateWithMs))
           << ",\"exported_at_utc\":"
           << JsonString(exportedAt.toString(Qt::ISODateWithMs))
           << ",\"port\":" << JsonString(metadata.port)
           << ",\"mode\":" << JsonString(metadata.mode)
           << ",\"protocol\":" << JsonString(metadata.protocol)
           << "}\n";

    const double lastEventTime = WriteEvents(stream, session);
    const double endTime =
        std::max(session.durationSeconds, lastEventTime);
    stream << "{\"type\":\"session_end\",\"t\":";
    WriteTime(stream, endTime);
    stream << ",\"final_stats\":";
    WriteStatsObject(stream, session.stats);
    stream << ",\"signal_summaries\":";
    WriteSignalSummaries(stream, session.floatSignals);
    stream << "}\n";

    stream.flush();
    if (stream.status() != QTextStream::Ok) {
        error = QStringLiteral("Failed while writing session data");
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        error = file.errorString();
        return false;
    }
    error.clear();
    return true;
}

bool ExportRuntimeSession(const QString& path,
                          const TelemetryStore& store,
                          const RuntimeSessionMetadata& metadata,
                          QString& error) {
    const auto retention = store.GetSessionRetentionStats();
    const RuntimeSessionSnapshot session = store.SessionMetadataSnapshot();
    const auto afterMetadata = store.GetSessionRetentionStats();
    if (afterMetadata.archiveGeneration != retention.archiveGeneration ||
        afterMetadata.sessionEpoch != retention.sessionEpoch) {
        error = QStringLiteral("Session archive changed during export; retry");
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        error = file.errorString();
        return false;
    }
    QTextStream stream(&file);
    const QDateTime startedAt = QDateTime::fromMSecsSinceEpoch(
        session.startedAtUnixMs, QTimeZone::UTC);
    const QDateTime exportedAt = QDateTime::currentDateTimeUtc();
    stream << "{\"type\":\"session_start\""
           << ",\"format\":\"rte-runtime-session-jsonl\""
           << ",\"version\":3,\"t\":0,\"started_at_utc\":"
           << JsonString(startedAt.toString(Qt::ISODateWithMs))
           << ",\"exported_at_utc\":"
           << JsonString(exportedAt.toString(Qt::ISODateWithMs))
           << ",\"port\":" << JsonString(metadata.port)
           << ",\"mode\":" << JsonString(metadata.mode)
           << ",\"protocol\":" << JsonString(metadata.protocol) << "}\n";

    double lastEventTime = 0.0;
    std::unordered_map<std::string, SignalSummary> summaries;
    if (!WriteEventsPaged(stream, store, session, retention,
                          lastEventTime, summaries, error)) {
        file.cancelWriting();
        return false;
    }
    stream << "{\"type\":\"session_end\",\"t\":";
    WriteTime(stream, std::max(session.durationSeconds, lastEventTime));
    stream << ",\"final_stats\":";
    WriteStatsObject(stream, session.stats);
    stream << ",\"signal_summaries\":";
    WriteSignalSummaries(stream, summaries);
    stream << "}\n";
    stream.flush();
    if (stream.status() != QTextStream::Ok) {
        error = QStringLiteral("Failed while writing session data");
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        error = file.errorString();
        return false;
    }
    error.clear();
    return true;
}

}  // namespace NodeGUI::runtime
