#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <tuple>
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

// Point-in-time copy of everything the runtime knows. Mirrors the old ImGui
// client's TelemetryState so the HTTP API can keep its exact JSON contract.
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
// HTTP server thread.
//
// Retention matches the old client: 30 seconds or 12000 samples per signal,
// 6000 console lines.
class TelemetryStore {
public:
    static constexpr float kRetainSeconds = 60.0f;
    static constexpr float kPwmRetainSeconds = 60.0f;
    static constexpr std::size_t kMaxSamples = 20000;
    static constexpr std::size_t kMaxPwmSamples = 20000;
    static constexpr std::size_t kConsoleCapLines = 6000;

    void AddF32(const std::string& key, float value, float tsec);
    void AddF32Batch(const std::vector<std::tuple<std::string, float, float>>& samples);
    void AddString(const std::string& key, const std::string& value);
    void AddConsoleLine(const std::string& text);
    void ClearConsole();

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

    // When frozen, latest values still update but history deques are not
    // appended (used while sim is paused so plot time does not advance).
    void SetHistoryFrozen(bool frozen);

    // Lightweight scalar stats (no histories) — cheap enough for ~30 Hz UI
    // header updates, unlike Snapshot().
    struct StatsLine {
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
    StatsLine GetStatsLine() const;

    TelemetrySnapshot Snapshot() const;

    // Copies one signal's history. Returns false if the signal is unknown.
    bool CopyHistory(const std::string& key,
                     std::deque<float>& t,
                     std::deque<float>& y) const;

    // Same as CopyHistory but fills reusable vectors (avoids the allocation
    // churn of deque copies in the ~30 Hz plot refresh path).
    // When maxSamples > 0, only the most recent maxSamples points are copied.
    bool CopyHistoryInto(const std::string& key,
                         std::vector<float>& t,
                         std::vector<float>& y,
                         std::size_t maxSamples = 0) const;

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
    void TrimHistoryLocked(const std::string& key, SignalHistory& hist) const;
    void InvalidateNameCacheLocked();

    mutable std::mutex mtx_;
    TelemetrySnapshot snap_;
    bool history_frozen_ = false;
    mutable bool names_dirty_ = true;
    mutable std::vector<std::string> cached_names_;
    // Starts at 1: the HTTP console API filters `seq > since` with a default
    // `since` of 0, so seq 0 would never be delivered.
    uint64_t nextConsoleSeq_ = 1;
};

}  // namespace NodeGUI::runtime
