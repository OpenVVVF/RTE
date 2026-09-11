#include "TelemetryStore.h"

#include <algorithm>
#include <cmath>

namespace NodeGUI::runtime {

void TelemetryStore::AddF32(const std::string& key, float value, float tsec) {
    std::lock_guard lock(mtx_);
    auto& hist = snap_.hist[key];
    hist.t.push_back(tsec);
    hist.y.push_back(value);
    TrimHistoryLocked(hist);
    snap_.latest[key] = value;

    if (!sessionTelemetryClockInitialized_) {
        sessionTelemetryClockInitialized_ = true;
        sessionTelemetrySourceOrigin_ = tsec;
        sessionTelemetryElapsedOrigin_ = SessionElapsedSeconds();
    }
    const float sessionTsec = static_cast<float>(
        sessionTelemetryElapsedOrigin_ +
        static_cast<double>(tsec - sessionTelemetrySourceOrigin_));
    AppendSessionF32Locked(sessionFloatSignals_[key], sessionTsec, value);
}

void TelemetryStore::AddString(const std::string& key, const std::string& value) {
    std::lock_guard lock(mtx_);
    snap_.latestStr[key] = value;
    auto& samples = sessionStringSignals_[key];
    samples.push_back(SessionStringSample{SessionElapsedSeconds(), value});
    if (samples.size() > kSessionStringSamples) {
        // Drop the oldest half in one batch so steady-state pushes stay
        // allocation-free; a sparse string signal older than half the cap
        // ago is of little export value.
        samples.erase(samples.begin(),
                      samples.begin() +
                          static_cast<std::ptrdiff_t>(kSessionStringSamples / 2));
    }
}

void TelemetryStore::AddConsoleLine(const std::string& text) {
    std::lock_guard lock(mtx_);
    const uint64_t seq = nextConsoleSeq_++;
    snap_.console.push_back(ConsoleLine{seq, text});
    sessionConsole_.push_back(
        SessionConsoleLine{seq, SessionElapsedSeconds(), text});
    if (sessionConsole_.size() > kSessionConsoleCapLines) {
        sessionConsole_.erase(
            sessionConsole_.begin(),
            sessionConsole_.begin() +
                static_cast<std::ptrdiff_t>(kSessionConsoleCapLines / 2));
    }
    while (snap_.console.size() > kConsoleCapLines) {
        snap_.console.pop_front();
    }
}

void TelemetryStore::AddCommand(const std::string& text,
                                const std::string& source,
                                bool sent) {
    std::lock_guard lock(mtx_);
    sessionCommands_.push_back(
        SessionCommand{SessionElapsedSeconds(),
                       std::numeric_limits<double>::quiet_NaN(),
                       source,
                       text,
                       sent});
    ++unmarkedCommands_;
    unmarkedIndex_ = sessionCommands_.size() - 1;
    if (sessionCommands_.size() > kSessionCommandCap) {
        const std::size_t removed = kSessionCommandCap / 2;
        for (std::size_t i = 0; i < removed; ++i) {
            if (std::isnan(sessionCommands_[i].receivedTsec)) {
                --unmarkedCommands_;
            }
        }
        sessionCommands_.erase(
            sessionCommands_.begin(),
            sessionCommands_.begin() + static_cast<std::ptrdiff_t>(removed));
        // Indices shifted by `removed`; if the cached scan start fell into
        // the erased range the true newest-unmarked position is unknown, so
        // restart from the back once (a rare path — caps are generous).
        unmarkedIndex_ = unmarkedIndex_ >= removed
                             ? unmarkedIndex_ - removed
                             : (sessionCommands_.empty()
                                    ? kNoUnmarkedCommand
                                    : sessionCommands_.size() - 1);
    }
}

void TelemetryStore::MarkLastCommandReceived() {
    std::lock_guard lock(mtx_);
    // Fast path: most calls happen when no command is pending (every console
    // line arrives here), so the count check avoids touching the vector.
    if (unmarkedCommands_ == 0 || sessionCommands_.empty()) {
        return;
    }
    std::size_t i = std::min(unmarkedIndex_, sessionCommands_.size() - 1);
    for (;;) {
        if (std::isnan(sessionCommands_[i].receivedTsec)) {
            sessionCommands_[i].receivedTsec = SessionElapsedSeconds();
            --unmarkedCommands_;
            break;
        }
        if (i == 0) {
            // Bookkeeping slipped; resync so the next call is cheap again.
            unmarkedCommands_ = 0;
            unmarkedIndex_ = kNoUnmarkedCommand;
            return;
        }
        --i;
    }
    unmarkedIndex_ = i;
}

void TelemetryStore::ClearConsole() {
    std::lock_guard lock(mtx_);
    // Clearing affects the visible rolling console only. The session archive
    // remains intact so a later export is complete.
    snap_.console.clear();
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
    unmarkedCommands_ = 0;
    unmarkedIndex_ = kNoUnmarkedCommand;
    sessionTelemetryClockInitialized_ = false;
    sessionTelemetrySourceOrigin_ = 0.0f;
    sessionTelemetryElapsedOrigin_ = 0.0;
    // nextConsoleSeq_ deliberately keeps counting: since-polling clients
    // would silently miss renumbered lines. sessionEpoch_ marks the reset.
    ++sessionEpoch_;
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

TelemetryStore::DeviceView TelemetryStore::GetDeviceView() const {
    std::lock_guard lock(mtx_);
    DeviceView view;
    view.stats.rxHz = snap_.rxHz;
    view.stats.rxBytesPerSec = snap_.rxBytesPerSec;
    view.stats.goodFrames = snap_.goodFrames;
    view.stats.badFrames = snap_.badFrames;
    view.stats.rejectCrc = snap_.rejectCrc;
    view.stats.rejectHdr = snap_.rejectHdr;
    view.stats.rejectLen = snap_.rejectLen;
    view.stats.rejectPayloadParse = snap_.rejectPayloadParse;
    view.stats.rejectUnknownId = snap_.rejectUnknownId;
    view.stats.lastSeq = snap_.lastSeq;
    view.stats.suspended = snap_.suspended;
    view.latest = snap_.latest;
    view.latestStr = snap_.latestStr;
    return view;
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
    // Export layout: decimated older samples first, then the full-rate
    // recent block — monotonic in time, matching the pre-cap format.
    for (const auto& [key, store] : sessionFloatSignals_) {
        SessionSignalHistory history;
        history.t.reserve(store.archiveT.size() + store.recentT.size());
        history.y.reserve(store.archiveY.size() + store.recentY.size());
        history.t.insert(history.t.end(),
                         store.archiveT.begin(), store.archiveT.end());
        history.t.insert(history.t.end(),
                         store.recentT.begin(), store.recentT.end());
        history.y.insert(history.y.end(),
                         store.archiveY.begin(), store.archiveY.end());
        history.y.insert(history.y.end(),
                         store.recentY.begin(), store.recentY.end());
        result.floatSignals.emplace(key, std::move(history));
    }
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

uint64_t TelemetryStore::SessionEpoch() const {
    std::lock_guard lock(mtx_);
    return sessionEpoch_;
}

void TelemetryStore::AppendSessionF32Locked(SessionSignalStore& store,
                                            float t,
                                            float y) {
    if (store.recentT.capacity() < kSessionRecentSamples + 1) {
        store.recentT.reserve(kSessionRecentSamples + 1);
        store.recentY.reserve(kSessionRecentSamples + 1);
    }
    store.recentT.push_back(t);
    store.recentY.push_back(y);
    if (store.recentT.size() <= kSessionRecentSamples) {
        return;
    }
    // Fold the oldest half of the recent block into the decimated archive.
    const std::size_t fold = kSessionRecentSamples / 2;
    for (std::size_t i = 0; i < fold; ++i) {
        AppendSessionArchiveLocked(store, store.recentT[i], store.recentY[i]);
    }
    store.recentT.erase(store.recentT.begin(),
                        store.recentT.begin() + static_cast<std::ptrdiff_t>(fold));
    store.recentY.erase(store.recentY.begin(),
                        store.recentY.begin() + static_cast<std::ptrdiff_t>(fold));
}

void TelemetryStore::AppendSessionArchiveLocked(SessionSignalStore& store,
                                                float t,
                                                float y) {
    // Counter-based decimation: keep one sample per `stride` arrivals.
    if (++store.phase < store.stride) {
        return;
    }
    store.phase = 0;
    if (store.archiveT.capacity() < kSessionArchiveSamples + 1) {
        store.archiveT.reserve(kSessionArchiveSamples + 1);
        store.archiveY.reserve(kSessionArchiveSamples + 1);
    }
    store.archiveT.push_back(t);
    store.archiveY.push_back(y);
    if (store.archiveT.size() > kSessionArchiveSamples) {
        // Progressive halving (oscilloscope-style): keep every second
        // sample and double the stride, so archive density decays with age
        // while recent samples retain the finest current resolution.
        std::size_t w = 0;
        for (std::size_t r = 1; r < store.archiveT.size(); r += 2) {
            store.archiveT[w] = store.archiveT[r];
            store.archiveY[w] = store.archiveY[r];
            ++w;
        }
        store.archiveT.resize(w);
        store.archiveY.resize(w);
        store.stride *= 2;
    }
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
