#include "TelemetryStore.h"

#include <algorithm>
#include <cmath>

namespace rte::runtime {

TelemetryStore::TelemetryStore(float retainSeconds,
                               std::size_t maxSamples,
                               std::size_t consoleCapLines,
                               std::size_t eventReplayCap)
    : retainSeconds_(std::max(retainSeconds, 1.0f)),
      maxSamples_(std::max<std::size_t>(maxSamples, 1)),
      consoleCapLines_(std::max<std::size_t>(consoleCapLines, 1)),
      eventReplayCap_(std::max<std::size_t>(eventReplayCap, 1)) {}

void TelemetryStore::AddF32(const std::string& key, float value, float tsec) {
    std::lock_guard lock(mtx_);
    auto& hist = snap_.hist[key];
    if (!hist.t.empty() && std::isfinite(tsec)
        && tsec < hist.t.back()) {
        // A source/gateway restart begins a new time epoch. Keeping the old
        // point would make GL_LINE_STRIP draw across the reset boundary.
        hist.t.clear();
        hist.y.clear();
    }
    hist.t.push_back(tsec);
    hist.y.push_back(value);
    TrimHistoryLocked(hist);
    snap_.latest[key] = value;
    AddEventLocked(TelemetryEvent{.kind = TelemetryEventKind::Float,
                                  .key = key,
                                  .value = value,
                                  .tsec = tsec});

    if (!sessionTelemetryClockInitialized_) {
        sessionTelemetryClockInitialized_ = true;
        sessionTelemetrySourceOrigin_ = tsec;
        sessionTelemetryLastSourceTsec_ = tsec;
        sessionTelemetryElapsedOrigin_ = SessionElapsedSeconds();
        sessionTelemetryLastMappedTsec_ = sessionTelemetryElapsedOrigin_;
    } else if (std::isfinite(tsec) && std::isfinite(sessionTelemetryLastSourceTsec_)
               && tsec < sessionTelemetryLastSourceTsec_) {
        sessionTelemetrySourceOrigin_ = tsec;
        sessionTelemetryElapsedOrigin_ = std::max(
            SessionElapsedSeconds(), sessionTelemetryLastMappedTsec_);
    }
    const double sessionTsec =
        sessionTelemetryElapsedOrigin_ +
        static_cast<double>(tsec - sessionTelemetrySourceOrigin_);
    sessionTelemetryLastSourceTsec_ = tsec;
    sessionTelemetryLastMappedTsec_ = std::max(
        sessionTelemetryLastMappedTsec_, sessionTsec);
    auto& sessionHistory = sessionFloatSignals_[key];
    sessionHistory.t.push_back(static_cast<float>(sessionTsec));
    sessionHistory.y.push_back(value);
}

void TelemetryStore::AddString(const std::string& key, const std::string& value) {
    std::lock_guard lock(mtx_);
    snap_.latestStr[key] = value;
    AddEventLocked(TelemetryEvent{.kind = TelemetryEventKind::String,
                                  .key = key,
                                  .text = value});
    sessionStringSignals_[key].push_back(
        SessionStringSample{SessionElapsedSeconds(), value});
}

void TelemetryStore::AddConsoleLine(const std::string& text) {
    std::lock_guard lock(mtx_);
    const uint64_t seq = nextConsoleSeq_++;
    snap_.console.push_back(ConsoleLine{seq, text});
    AddEventLocked(TelemetryEvent{.kind = TelemetryEventKind::Console,
                                  .key = std::to_string(seq),
                                  .text = text});
    sessionConsole_.push_back(
        SessionConsoleLine{seq, SessionElapsedSeconds(), text});
    while (snap_.console.size() > consoleCapLines_) {
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

void TelemetryStore::ResetLiveTelemetry() {
    std::lock_guard lock(mtx_);
    snap_.latest.clear();
    snap_.hist.clear();
    snap_.latestStr.clear();
    snap_.rxHz = 0.0f;
    snap_.rxBytesPerSec = 0.0f;
    snap_.goodFrames = 0;
    snap_.badFrames = 0;
    snap_.rejectCrc = 0;
    snap_.rejectHdr = 0;
    snap_.rejectLen = 0;
    snap_.rejectPayloadParse = 0;
    snap_.rejectUnknownId = 0;
    snap_.lastSeq = 0;
}

void TelemetryStore::ClearSession() {
    std::lock_guard lock(mtx_);
    const bool suspended = snap_.suspended;
    snap_ = TelemetrySnapshot{};
    snap_.suspended = suspended;
    sessionFloatSignals_.clear();
    sessionStringSignals_.clear();
    sessionConsole_.clear();
    sessionCommands_.clear();
    sessionTelemetryClockInitialized_ = false;
    sessionTelemetrySourceOrigin_ = 0.0f;
    sessionTelemetryLastSourceTsec_ = 0.0f;
    sessionTelemetryElapsedOrigin_ = 0.0;
    sessionTelemetryLastMappedTsec_ = 0.0;
    nextConsoleSeq_ = 1;
    events_.clear();
    AddEventLocked(TelemetryEvent{.kind = TelemetryEventKind::Reset});
    sessionStartSteady_ = std::chrono::steady_clock::now();
    sessionStartWall_ = std::chrono::system_clock::now();
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
    TelemetryStats stats;
    stats.rxHz = rxHz;
    stats.rxBytesPerSec = rxBytesPerSec;
    stats.goodFrames = goodFrames;
    stats.badFrames = badFrames;
    stats.rejectCrc = rejectCrc;
    stats.rejectHdr = rejectHdr;
    stats.rejectLen = rejectLen;
    stats.rejectPayloadParse = rejectPayloadParse;
    stats.rejectUnknownId = rejectUnknownId;
    stats.lastSeq = lastSeq;
    stats.suspended = snap_.suspended;
    AddEventLocked(TelemetryEvent{.kind = TelemetryEventKind::Stats,
                                  .stats = stats});
}

void TelemetryStore::SetSuspended(bool suspended) {
    std::lock_guard lock(mtx_);
    snap_.suspended = suspended;
    AddEventLocked(TelemetryEvent{.kind = TelemetryEventKind::Suspended,
                                  .flag = suspended});
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

std::vector<TelemetryEvent> TelemetryStore::EventsSince(uint64_t sinceSeq,
                                                        std::size_t limit,
                                                        bool& reset) const {
    std::lock_guard lock(mtx_);
    reset = !events_.empty() && sinceSeq != 0
            && sinceSeq + 1 < events_.front().seq;
    std::vector<TelemetryEvent> result;
    result.reserve(std::min(limit, events_.size()));
    for (const auto& event : events_) {
        if (event.seq > sinceSeq) {
            result.push_back(event);
            if (result.size() >= limit) break;
        }
    }
    return result;
}

uint64_t TelemetryStore::LatestEventSeq() const {
    std::lock_guard lock(mtx_);
    return nextEventSeq_ - 1;
}

bool TelemetryStore::WaitForEvents(uint64_t sinceSeq,
                                   std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mtx_);
    return eventCv_.wait_for(lock, timeout,
                             [&] { return nextEventSeq_ - 1 > sinceSeq; });
}

void TelemetryStore::AddEventLocked(TelemetryEvent event) {
    event.seq = nextEventSeq_++;
    events_.push_back(std::move(event));
    while (events_.size() > eventReplayCap_) events_.pop_front();
    eventCv_.notify_all();
}

void TelemetryStore::TrimHistoryLocked(SignalHistory& hist) const {
    while (hist.t.size() > maxSamples_) {
        hist.t.pop_front();
        hist.y.pop_front();
    }
    if (!hist.t.empty()) {
        const float cutoff = hist.t.back() - retainSeconds_;
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

}  // namespace rte::runtime
