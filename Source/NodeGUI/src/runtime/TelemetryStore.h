#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
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
};

struct SessionSignalHistory {
    std::vector<float> t;
    std::vector<float> y;
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

// Full, non-rolling capture used by "Export Session". Plot histories below
// remain bounded for rendering performance, while this archive lasts for the
// lifetime of the RuntimeController.
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
// high-frequency polling.
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
// Retention matches the old client: 30 seconds or 12000 samples per signal,
// 6000 console lines.
class TelemetryStore {
public:
    static constexpr float kRetainSeconds = 30.0f;
    static constexpr std::size_t kMaxSamples = 12000;
    static constexpr std::size_t kConsoleCapLines = 6000;

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
    // header updates, unlike Snapshot().
    using StatsLine = TelemetryStats;
    StatsLine GetStatsLine() const;

    TelemetrySnapshot Snapshot() const;
    RuntimeSessionSnapshot SessionSnapshot() const;

    // Copies one signal's history. Returns false if the signal is unknown.
    bool CopyHistory(const std::string& key,
                     std::deque<float>& t,
                     std::deque<float>& y) const;

    // Same as CopyHistory but fills reusable vectors (avoids the allocation
    // churn of deque copies in the ~30 Hz plot refresh path).
    bool CopyHistoryInto(const std::string& key,
                         std::vector<float>& t,
                         std::vector<float>& y) const;

    // Latest float value of one signal. Returns false if unknown.
    bool LatestValue(const std::string& key, float& value) const;

    // Sorted list of known float signal names (for the signal table).
    std::vector<std::string> SignalNames() const;

    // Console lines with seq > sinceSeq (for incremental console drains).
    std::vector<ConsoleLine> ConsoleSince(uint64_t sinceSeq) const;

    // Seq of the most recent console line, 0 when the console is empty. Used
    // by console views to detect a ClearConsole() (seq goes backwards).
    uint64_t LatestConsoleSeq() const;

private:
    void TrimHistoryLocked(SignalHistory& hist) const;
    double SessionElapsedSeconds() const;

    mutable std::mutex mtx_;
    TelemetrySnapshot snap_;
    std::unordered_map<std::string, SessionSignalHistory> sessionFloatSignals_;
    std::unordered_map<std::string, std::vector<SessionStringSample>>
        sessionStringSignals_;
    std::vector<SessionConsoleLine> sessionConsole_;
    std::vector<SessionCommand> sessionCommands_;
    bool sessionTelemetryClockInitialized_ = false;
    float sessionTelemetrySourceOrigin_ = 0.0f;
    double sessionTelemetryElapsedOrigin_ = 0.0;
    std::chrono::steady_clock::time_point sessionStartSteady_ =
        std::chrono::steady_clock::now();
    std::chrono::system_clock::time_point sessionStartWall_ =
        std::chrono::system_clock::now();
    // Starts at 1: the HTTP console API filters `seq > since` with a default
    // `since` of 0, so seq 0 would never be delivered.
    uint64_t nextConsoleSeq_ = 1;
};

}  // namespace NodeGUI::runtime
