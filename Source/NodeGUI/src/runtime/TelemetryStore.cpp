#include "TelemetryStore.h"

#include <algorithm>
#include <tuple>

namespace NodeGUI::runtime {

void TelemetryStore::SetHistoryFrozen(bool frozen) {
    std::lock_guard lock(mtx_);
    history_frozen_ = frozen;
}

void TelemetryStore::AddF32(const std::string& key, float value, float tsec) {
    std::lock_guard lock(mtx_);
    const bool new_key = snap_.latest.find(key) == snap_.latest.end();
    snap_.latest[key] = value;
    if (new_key) {
        InvalidateNameCacheLocked();
    }
    if (history_frozen_) {
        return;
    }
    auto& hist = snap_.hist[key];
    hist.t.push_back(tsec);
    hist.y.push_back(value);
    TrimHistoryLocked(key, hist);
}

void TelemetryStore::AddF32Batch(
    const std::vector<std::tuple<std::string, float, float>>& samples) {
    if (samples.empty()) {
        return;
    }
    std::lock_guard lock(mtx_);
    for (const auto& [key, value, tsec] : samples) {
        const bool new_key = snap_.latest.find(key) == snap_.latest.end();
        snap_.latest[key] = value;
        if (new_key) {
            InvalidateNameCacheLocked();
        }
        if (history_frozen_) {
            continue;
        }
        auto& hist = snap_.hist[key];
        hist.t.push_back(tsec);
        hist.y.push_back(value);
        TrimHistoryLocked(key, hist);
    }
}

void TelemetryStore::AddString(const std::string& key, const std::string& value) {
    std::lock_guard lock(mtx_);
    snap_.latestStr[key] = value;
}

void TelemetryStore::AddConsoleLine(const std::string& text) {
    std::lock_guard lock(mtx_);
    snap_.console.push_back(ConsoleLine{nextConsoleSeq_++, text});
    while (snap_.console.size() > kConsoleCapLines) {
        snap_.console.pop_front();
    }
}

void TelemetryStore::ClearConsole() {
    std::lock_guard lock(mtx_);
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
                                     std::vector<float>& y,
                                     std::size_t maxSamples) const {
    std::lock_guard lock(mtx_);
    const auto it = snap_.hist.find(key);
    if (it == snap_.hist.end()) {
        return false;
    }
    const auto& hist = it->second;
    if (maxSamples == 0 || hist.t.size() <= maxSamples) {
        t.assign(hist.t.begin(), hist.t.end());
        y.assign(hist.y.begin(), hist.y.end());
    } else {
        const auto start = hist.t.end() - static_cast<std::ptrdiff_t>(maxSamples);
        t.assign(start, hist.t.end());
        y.assign(hist.y.end() - static_cast<std::ptrdiff_t>(maxSamples), hist.y.end());
    }
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
    if (names_dirty_) {
        cached_names_.clear();
        cached_names_.reserve(snap_.latest.size());
        for (const auto& [name, value] : snap_.latest) {
            (void)value;
            cached_names_.push_back(name);
        }
        std::sort(cached_names_.begin(), cached_names_.end());
        names_dirty_ = false;
    }
    return cached_names_;
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

void TelemetryStore::TrimHistoryLocked(const std::string& key,
                                       SignalHistory& hist) const {
    const bool pwm = key.rfind("pwm_", 0) == 0;
    const std::size_t cap = pwm ? kMaxPwmSamples : kMaxSamples;
    const float retain_s = pwm ? kPwmRetainSeconds : kRetainSeconds;

    if (hist.t.size() > cap) {
        const auto drop = hist.t.size() - cap;
        hist.t.erase(hist.t.begin(), hist.t.begin() + static_cast<std::ptrdiff_t>(drop));
        hist.y.erase(hist.y.begin(), hist.y.begin() + static_cast<std::ptrdiff_t>(drop));
    }
    if (!hist.t.empty()) {
        const float cutoff = hist.t.back() - retain_s;
        const auto it = std::lower_bound(hist.t.begin(), hist.t.end(), cutoff);
        if (it != hist.t.begin()) {
            const auto drop = static_cast<std::size_t>(it - hist.t.begin());
            hist.t.erase(hist.t.begin(), it);
            hist.y.erase(hist.y.begin(), hist.y.begin() + static_cast<std::ptrdiff_t>(drop));
        }
    }
}

void TelemetryStore::InvalidateNameCacheLocked() {
    names_dirty_ = true;
}

}  // namespace NodeGUI::runtime
