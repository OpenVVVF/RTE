#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace NodeGUI::runtime {

// One float signal's rolling history (parallel deques of time/value).
struct SignalHistory {
    std::deque<float> t;
    std::deque<float> y;
};

struct ConsoleLine {
    uint64_t seq = 0;
    std::string text;
};

struct TelemetryStats {
    float rxHz = 0.0f;
    float rxBytesPerSec = 0.0f;
    uint64_t goodFrames = 0;
    uint64_t badFrames = 0;
    uint64_t rejectCrc = 0;
    uint64_t rejectHdr = 0;
    uint64_t rejectLen = 0;
    uint64_t rejectPayloadParse = 0;
    uint64_t rejectUnknownId = 0;
    uint32_t lastSeq = 0;
    bool suspended = false;
    // Negative until the first valid telemetry frame is observed.
    double frameAgeSeconds = -1.0;
};

struct SessionSignalHistory {
    std::vector<float> t;
    std::vector<float> y;
};

struct SessionFloatSample {
    float t = 0.0f;
    float y = 0.0f;
};

struct SessionStringSample {
    double tsec = 0.0;
    std::string value;
};

struct SessionConsoleLine {
    uint64_t seq = 0;
    double tsec = 0.0;
    std::string text;
};

struct SessionCommand {
    double tsec = 0.0;
    double receivedTsec = std::numeric_limits<double>::quiet_NaN();
    std::string source;
    std::string text;
    bool sent = false;
};

// Session metadata and optional numeric histories used by Export Session.
// The normal export path reads numeric histories in pages to avoid copying a
// large in-memory archive. Strings, console, and commands remain capped.
struct RuntimeSessionSnapshot {
    int64_t startedAtUnixMs = 0;
    double durationSeconds = 0.0;
    std::unordered_map<std::string, SessionSignalHistory> floatSignals;
    std::unordered_map<std::string, std::vector<SessionStringSample>> stringSignals;
    std::vector<SessionConsoleLine> console;
    std::vector<SessionCommand> commands;
    TelemetryStats stats;
};

// Point-in-time copy of everything the runtime knows. Mirrors the old ImGui
// client's TelemetryState so the local automation session can expose it.
// NOTE: expensive to produce (full history copies) — use GetStatsLine() for
// scalar polling and GetDeviceView() / ConsoleSince() for the HTTP endpoints.
struct TelemetrySnapshot {
    std::deque<ConsoleLine> console;
    std::unordered_map<std::string, float> latest;
    std::unordered_map<std::string, SignalHistory> hist;
    std::unordered_map<std::string, std::string> latestStr;

    uint32_t lastSeq = 0;
    float rxHz = 0.0f;
    float rxBytesPerSec = 0.0f;
    uint64_t goodFrames = 0;
    uint64_t badFrames = 0;
    uint64_t rejectCrc = 0;
    uint64_t rejectHdr = 0;
    uint64_t rejectLen = 0;
    uint64_t rejectPayloadParse = 0;
    uint64_t rejectUnknownId = 0;

    bool suspended = false;
};

// Thread-safe store for live telemetry: float signal histories, latest values
// (float + string), and the device console scrollback. Written by the GUI
// thread (from RuntimeController's drain timer) and read by the GUI and the
// local session endpoint.
//
// The live plot path keeps a small rolling window. The session archive keeps
// every received numeric sample until its host-RAM budget is reached, then
// incrementally decimates its oldest samples in bounded blocks. Numeric
// export and MCP reads use paginated copies, never a whole-archive clone.
class TelemetryStore {
public:
    // Zero selects two thirds of the host's available physical RAM. Supplying
    // a small budget is useful for deterministic compaction tests.
    explicit TelemetryStore(std::size_t sessionBudgetBytes = 0);
    struct SignalDisplay {
        float value = 0.0f;
        std::optional<double> updateHz;
    };

    static constexpr float kRetainSeconds = 60.0f;
    static constexpr std::size_t kMaxSamples = 16000;
    static constexpr std::size_t kConsoleCapLines = 6000;
    static constexpr std::size_t kSessionCompactionBlock = 4096;
    static constexpr std::size_t kSessionStringSamples = 12000;
    static constexpr std::size_t kSessionConsoleCapLines = 50000;
    static constexpr std::size_t kSessionCommandCap = 10000;

    void AddF32(const std::string& key, float value, float tsec);
    void AddString(const std::string& key, const std::string& value);
    void AddConsoleLine(const std::string& text);
    void AddCommand(const std::string& text,
                    const std::string& source,
                    bool sent);
    // Marks the most recent command that has not yet been marked as received
    // with the current session elapsed time. Call when a device console line
    // arrives so the exported command event can record when the response came
    // back. Safe to call when no pending command exists (no-op).
    void MarkLastCommandReceived();
    void ClearConsole();
    void ClearSession();

    void SetStats(float rxHz,
                  float rxBytesPerSec,
                  uint64_t goodFrames,
                  uint64_t badFrames,
                  uint64_t rejectCrc,
                  uint64_t rejectHdr,
                  uint64_t rejectLen,
                  uint64_t rejectPayloadParse,
                  uint64_t rejectUnknownId,
                  uint32_t lastSeq);
    void SetSuspended(bool suspended);

    // Lightweight scalar stats (no histories) — cheap enough for ~30 Hz UI
    // header updates and HTTP status polling, unlike Snapshot().
    using StatsLine = TelemetryStats;
    StatsLine GetStatsLine() const;

    // Cheap view for the device.telemetry HTTP endpoint: scalar stats plus
    // the latest-value maps (one entry per signal). Unlike Snapshot() this
    // does not copy any history.
    struct DeviceView {
        TelemetryStats stats;
        std::unordered_map<std::string, float> latest;
        std::unordered_map<std::string, std::string> latestStr;
        std::unordered_map<std::string, double> ageSeconds;
    };
    DeviceView GetDeviceView() const;

    // Full deep copy including every history — expensive; only for rare,
    // user-triggered consumers (FRAM key export).
    TelemetrySnapshot Snapshot() const;
    RuntimeSessionSnapshot SessionSnapshot() const;
    RuntimeSessionSnapshot SessionMetadataSnapshot() const;

    struct SessionRetentionStats {
        std::size_t budgetBytes = 0;
        std::size_t storedSamples = 0;
        uint64_t archiveGeneration = 0;
        uint64_t sessionEpoch = 0;
    };
    SessionRetentionStats GetSessionRetentionStats() const;
    std::vector<std::string> SessionFloatNames() const;
    bool CopySessionHistoryPage(const std::string& key, std::size_t offset,
                                std::size_t limit,
                                std::vector<SessionFloatSample>& samples,
                                std::size_t& total,
                                uint64_t& generation) const;

    // Copies one signal's history. Returns false if the signal is unknown.
    bool CopyHistory(const std::string& key,
                     std::deque<float>& t,
                     std::deque<float>& y) const;

    // Copies several histories under one lock so their window is consistent.
    std::unordered_map<std::string, SignalHistory> CopyHistories(
        const std::vector<std::string>& keys) const;

    // Same as CopyHistory but fills reusable vectors (avoids the allocation
    // churn of deque copies in the ~30 Hz plot refresh path).
    bool CopyHistoryInto(const std::string& key,
                         std::vector<float>& t,
                         std::vector<float>& y) const;

    // Visible plot window only, with min/max pairs per pixel bucket so spikes
    // survive culling and GPU work stays proportional to widget width.
    bool CopyPlotHistoryInto(const std::string& key, float viewSeconds,
                             std::size_t maxPoints,
                             std::vector<float>& t,
                             std::vector<float>& y) const;

    // Copies the newest string telemetry events for one key from the session.
    bool CopyStringHistory(const std::string& key, std::size_t limit,
                           std::vector<SessionStringSample>& samples) const;

    // Latest float value of one signal. Returns false if unknown.
    bool LatestValue(const std::string& key, float& value) const;

    // Latest numeric values and measured per-signal value-change rates.
    // Repeated telemetry values do not count as changes.
    std::unordered_map<std::string, SignalDisplay> SignalDisplays() const;

    // Sorted list of known float signal names (for the signal table).
    std::vector<std::string> SignalNames() const;

    // Console lines with seq > sinceSeq (for incremental console drains).
    std::vector<ConsoleLine> ConsoleSince(uint64_t sinceSeq) const;

    // Seq of the most recent console line, 0 when the console is empty. Used
    // by console views to detect a ClearConsole() (seq goes backwards). Note
    // that seq values themselves are never reused within a run: ClearSession()
    // does not restart numbering (see SessionEpoch).
    uint64_t LatestConsoleSeq() const;

    // Generation counter, incremented by every ClearSession() call. Console
    // seq numbers are never reset within a GUI run, so the HTTP response
    // carries this counter to let long-lived pollers detect that the archive
    // they track was discarded.
    uint64_t SessionEpoch() const;

private:
    struct SignalRateState {
        float windowStartSec = 0.0f;
        float lastSourceSec = 0.0f;
        float lastValue = 0.0f;
        uint32_t changesInWindow = 0;
        double hz = 0.0;
        bool measured = false;
        bool initialized = false;
    };

    // Level 0 has full-rate newest data. Higher levels contain older data
    // progressively reduced by factors of two. Each deque grows in small
    // blocks so retention near the RAM budget never reallocates a huge array.
    struct SessionSignalStore {
        std::vector<std::deque<SessionFloatSample>> levels;
    };

    void TrimHistoryLocked(SignalHistory& hist) const;
    void AppendSessionF32Locked(SessionSignalStore& store, float t, float y);
    bool CompactOldestSessionBlockLocked();
    static std::size_t SessionSignalSize(const SessionSignalStore& store);
    RuntimeSessionSnapshot SessionSnapshotLocked(bool includeFloats) const;
    double SessionElapsedSeconds() const;

    static constexpr std::size_t kNoUnmarkedCommand =
        std::numeric_limits<std::size_t>::max();

    mutable std::mutex mtx_;
    TelemetrySnapshot snap_;
    std::unordered_map<std::string, SessionSignalStore> sessionFloatSignals_;
    std::size_t sessionBudgetBytes_ = 0;
    bool sessionBudgetOverride_ = false;
    std::size_t sessionSampleCount_ = 0;
    uint64_t archiveGeneration_ = 0;
    std::chrono::steady_clock::time_point lastBudgetCheck_{};
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastSignalUpdate_;
    std::unordered_map<std::string, SignalRateState> signalRates_;
    std::chrono::steady_clock::time_point lastFrameAt_{};
    std::unordered_map<std::string, std::vector<SessionStringSample>>
        sessionStringSignals_;
    std::vector<SessionConsoleLine> sessionConsole_;
    std::vector<SessionCommand> sessionCommands_;
    // MarkLastCommandReceived() bookkeeping: the rolling console calls it on
    // every line, so the interesting case is the cheap early-out; while a
    // command is pending, unmarkedIndex_ bounds the backward scan start.
    std::size_t unmarkedCommands_ = 0;
    std::size_t unmarkedIndex_ = kNoUnmarkedCommand;
    bool sessionTelemetryClockInitialized_ = false;
    float sessionTelemetrySourceOrigin_ = 0.0f;
    double sessionTelemetryElapsedOrigin_ = 0.0;
    std::chrono::steady_clock::time_point sessionStartSteady_ =
        std::chrono::steady_clock::now();
    std::chrono::system_clock::time_point sessionStartWall_ =
        std::chrono::system_clock::now();
    // Starts at 1: the HTTP console API filters `seq > since` with a default
    // `since` of 0, so seq 0 would never be delivered. Never reset within a
    // run so `since`-polling clients do not silently lose lines across a
    // ClearSession(); sessionEpoch_ marks those resets instead.
    uint64_t nextConsoleSeq_ = 1;
    uint64_t sessionEpoch_ = 1;
};

}  // namespace NodeGUI::runtime
