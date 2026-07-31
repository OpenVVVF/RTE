#include "TelemetryStore.h"

#include <algorithm>

namespace NodeGUI::runtime {

void TelemetryStore::AddF32(const std::string& key, float value, float tsec) {
    std::lock_guard lock(mtx_);
    auto& hist = snap_.hist[key];
    hist.t.push_back(tsec);
    hist.y.push_back(value);
    TrimHistoryLocked(hist);
    snap_.latest[key] = value;

    auto& sessionHistory = sessionFloatSignals_[key];
    sessionHistory.t.push_back(tsec);
    sessionHistory.y.push_back(value);
}

void TelemetryStore::AddString(const std::string& key, const std::string& value) {
    std::lock_guard lock(mtx_);
    snap_.latestStr[key] = value;
    sessionStringSignals_[key].push_back(
        SessionStringSample{SessionElapsedSeconds(), value});
}

void TelemetryStore::AddConsoleLine(const std::string& text) {
    std::lock_guard lock(mtx_);
    const uint64_t seq = nextConsoleSeq_++;
    snap_.console.push_back(ConsoleLine{seq, text});
    sessionConsole_.push_back(
        SessionConsoleLine{seq, SessionElapsedSeconds(), text});
    while (snap_.console.size() > kConsoleCapLines) {
        snap_.console.pop_front();
    }
}

void TelemetryStore::AddCommand(const std::string& text,
                                const std::string& source,
                                bool sent) {
    std::lock_guard lock(mtx_);
    sessionCommands_.push_back(
        SessionCommand{SessionElapsedSeconds(), source, text, sent});
}

void TelemetryStore::ClearConsole() {
    std::lock_guard lock(mtx_);
    // Clearing affects the visible rolling console only. The session archive
    // remains intact so a later export is complete.
    snap_.console.clear();
}

void TelemetryStore::SetStats(float rxHz,
                              float rxBytesPerSec,
                              uint64_t goodFrames,
                              uint64_t badFrames,
                              uint64_t rejectCrc,
                              uint64_t rejectHdr,
                              uint64_t rejectLen,
                              uint64_t rejectPayloadParse,
                              uint64_t rejectUnknownId,
                              uint32_t lastSeq) {
    std::lock_guard lock(mtx_);
    snap_.rxHz = rxHz;
    snap_.rxBytesPerSec = rxBytesPerSec;
    snap_.goodFrames = goodFrames;
    snap_.badFrames = badFrames;
    snap_.rejectCrc = rejectCrc;
    snap_.rejectHdr = rejectHdr;
    snap_.rejectLen = rejectLen;
    snap_.rejectPayloadParse = rejectPayloadParse;
    snap_.rejectUnknownId = rejectUnknownId;
    snap_.lastSeq = lastSeq;
}

void TelemetryStore::SetSuspended(bool suspended) {
    std::lock_guard lock(mtx_);
    snap_.suspended = suspended;
}

TelemetryStore::StatsLine TelemetryStore::GetStatsLine() const {
    std::lock_guard lock(mtx_);
    StatsLine line;
    line.rxHz = snap_.rxHz;
    line.rxBytesPerSec = snap_.rxBytesPerSec;
    line.goodFrames = snap_.goodFrames;
    line.badFrames = snap_.badFrames;
    line.rejectCrc = snap_.rejectCrc;
    line.rejectHdr = snap_.rejectHdr;
    line.rejectLen = snap_.rejectLen;
    line.rejectPayloadParse = snap_.rejectPayloadParse;
    line.rejectUnknownId = snap_.rejectUnknownId;
    line.lastSeq = snap_.lastSeq;
    line.suspended = snap_.suspended;
    return line;
}

TelemetrySnapshot TelemetryStore::Snapshot() const {
    std::lock_guard lock(mtx_);
    return snap_;
}

RuntimeSessionSnapshot TelemetryStore::SessionSnapshot() const {
    std::lock_guard lock(mtx_);
    RuntimeSessionSnapshot result;
    result.startedAtUnixMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            sessionStartWall_.time_since_epoch())
            .count();
    result.durationSeconds = SessionElapsedSeconds();
    result.floatSignals = sessionFloatSignals_;
    result.stringSignals = sessionStringSignals_;
    result.console = sessionConsole_;
    result.commands = sessionCommands_;
    result.stats.rxHz = snap_.rxHz;
    result.stats.rxBytesPerSec = snap_.rxBytesPerSec;
    result.stats.goodFrames = snap_.goodFrames;
    result.stats.badFrames = snap_.badFrames;
    result.stats.rejectCrc = snap_.rejectCrc;
    result.stats.rejectHdr = snap_.rejectHdr;
    result.stats.rejectLen = snap_.rejectLen;
    result.stats.rejectPayloadParse = snap_.rejectPayloadParse;
    result.stats.rejectUnknownId = snap_.rejectUnknownId;
    result.stats.lastSeq = snap_.lastSeq;
    result.stats.suspended = snap_.suspended;
    return result;
}

bool TelemetryStore::CopyHistory(const std::string& key,
                                 std::deque<float>& t,
                                 std::deque<float>& y) const {
    std::lock_guard lock(mtx_);
    const auto it = snap_.hist.find(key);
    if (it == snap_.hist.end()) {
        return false;
    }
    t = it->second.t;
    y = it->second.y;
    return true;
}

bool TelemetryStore::CopyHistoryInto(const std::string& key,
                                     std::vector<float>& t,
                                     std::vector<float>& y) const {
    std::lock_guard lock(mtx_);
    const auto it = snap_.hist.find(key);
    if (it == snap_.hist.end()) {
        return false;
    }
    t.assign(it->second.t.begin(), it->second.t.end());
    y.assign(it->second.y.begin(), it->second.y.end());
    return true;
}

bool TelemetryStore::LatestValue(const std::string& key, float& value) const {
    std::lock_guard lock(mtx_);
    const auto it = snap_.latest.find(key);
    if (it == snap_.latest.end()) {
        return false;
    }
    value = it->second;
    return true;
}

std::vector<std::string> TelemetryStore::SignalNames() const {
    std::lock_guard lock(mtx_);
    std::vector<std::string> names;
    names.reserve(snap_.latest.size());
    for (const auto& [name, value] : snap_.latest) {
        (void)value;
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<ConsoleLine> TelemetryStore::ConsoleSince(uint64_t sinceSeq) const {
    std::lock_guard lock(mtx_);
    std::vector<ConsoleLine> lines;
    for (const auto& line : snap_.console) {
        if (line.seq > sinceSeq) {
            lines.push_back(line);
        }
    }
    return lines;
}

uint64_t TelemetryStore::LatestConsoleSeq() const {
    std::lock_guard lock(mtx_);
    return snap_.console.empty() ? 0 : snap_.console.back().seq;
}

void TelemetryStore::TrimHistoryLocked(SignalHistory& hist) const {
    while (hist.t.size() > kMaxSamples) {
        hist.t.pop_front();
        hist.y.pop_front();
    }
    if (!hist.t.empty()) {
        const float cutoff = hist.t.back() - kRetainSeconds;
        while (!hist.t.empty() && hist.t.front() < cutoff) {
            hist.t.pop_front();
            hist.y.pop_front();
        }
    }
}

double TelemetryStore::SessionElapsedSeconds() const {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now() - sessionStartSteady_)
        .count();
}

}  // namespace NodeGUI::runtime
