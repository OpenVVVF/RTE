#pragma once

#include <QString>
#include <QStringList>

#include <unordered_map>
#include <vector>

namespace NodeGUI::runtime {
class TelemetryStore;
}

namespace NodeGUI::sim {

// Header meta + counters of one loaded v3 rte-runtime-session-jsonl file
// (the same schema the twin emits, so sim and recorded sessions align
// key-for-key).
struct SimSessionMeta {
    int version = 0;          // 0 => no session_start record was seen
    QString scenario;         // session_start.scenario ("" when absent)
    QString plant;            // "generated" | "fallback" | "" when no stamp
    QString mode;             // "sim", "device", ...
    double durationS = 0.0;   // last finite record time
    int telemetryFrames = 0;
    int consoleLines = 0;
    int badLines = 0;         // unparseable records dropped by the loader
};

// Per-float-channel roll-up; drives the grayed "all-null" rows in the list.
struct SimChannelStats {
    int samples = 0;          // present (null or numeric) in this many frames
    int finiteSamples = 0;    // actually numeric; 0 => channel never carried data
    double minimum = 0.0;
    double maximum = 0.0;
    double lastFinite = 0.0;
    bool haveLastFinite = false;
};

// Loaded rte sim / twin v3 session: float channel histories plus the console
// scrollback, with a bridge into the runtime TelemetryStore the plot leaf
// widgets already read.
//
// The loader is deliberately tolerant: it parses line by line and skips
// anything it does not understand (future record kinds, truncated tails,
// json null samples) instead of failing the whole file, so a half-written
// session still renders.
class SimSession {
public:
    // Returns false only when nothing usable came out of the file (cannot
    // open, or zero float channels across all telemetry records).
    bool Load(const QString& path, QString& error);

    const SimSessionMeta& Meta() const { return meta_; }
    QStringList ChannelNames() const;  // sorted float channel names
    SimChannelStats ChannelStats(const QString& name) const;
    // Full history of one channel; json null samples come back as NaN.
    bool ChannelHistory(const QString& name,
                        std::vector<float>& t,
                        std::vector<float>& y) const;
    QStringList ConsoleLines() const { return console_; }

    // Pushes every channel into the store the SignalPlotWidget leaf reads.
    // NaN samples are skipped so the GL line strips never see them (and the
    // store's 30 s / 12000-sample rolling window holds the whole card for
    // the sessions this screen produces).
    void PopulateStore(NodeGUI::runtime::TelemetryStore& store) const;

    // Acceptance self-check: re-scans the file with a naive regex scanner
    // (independent of the QJsonDocument code path) and asserts the frame
    // count plus first/last samples of up to two channels match what Load()
    // produced, then cross-checks the TelemetryStore bridge tail values.
    // Human-readable result lines are appended to report.
    static bool SelfCheck(const QString& path, QString& report);

private:
    struct Channel {
        std::vector<float> t;
        std::vector<float> y;
        SimChannelStats stats;
    };

    SimSessionMeta meta_;
    std::unordered_map<std::string, Channel> channels_;
    QStringList console_;
};

}  // namespace NodeGUI::sim
