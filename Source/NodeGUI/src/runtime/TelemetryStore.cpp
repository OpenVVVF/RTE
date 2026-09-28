#include "TelemetryStore.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <mach/mach.h>
#endif

namespace NodeGUI::runtime {

namespace {

struct MachineMemory {
    uint64_t total = 0;
    uint64_t available = 0;
};

uint64_t ReadNumberFile(const char* path) {
    std::ifstream input(path);
    uint64_t value = 0;
    input >> value;
    return input ? value : 0;
}

MachineMemory ReadMachineMemory() {
    MachineMemory memory;
    bool haveAvailable = false;
#ifdef __linux__
    std::ifstream input("/proc/meminfo");
    std::string name, unit;
    uint64_t kib = 0;
    while (input >> name >> kib >> unit) {
        if (name == "MemTotal:") memory.total = kib * 1024U;
        else if (name == "MemAvailable:") {
            memory.available = kib * 1024U;
            haveAvailable = true;
        }
    }
    uint64_t cgroupLimit = ReadNumberFile("/sys/fs/cgroup/memory.max");
    uint64_t cgroupUsed = ReadNumberFile("/sys/fs/cgroup/memory.current");
    if (!cgroupLimit) {
        cgroupLimit = ReadNumberFile("/sys/fs/cgroup/memory/memory.limit_in_bytes");
        cgroupUsed = ReadNumberFile("/sys/fs/cgroup/memory/memory.usage_in_bytes");
    }
    if (cgroupLimit && cgroupLimit < memory.total) {
        memory.total = cgroupLimit;
        memory.available = std::min(memory.available,
            cgroupLimit > cgroupUsed ? cgroupLimit - cgroupUsed : uint64_t{0});
        haveAvailable = true;
    }
#elif defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        memory.total = status.ullTotalPhys;
        memory.available = status.ullAvailPhys;
        haveAvailable = true;
    }
#elif defined(__APPLE__)
    uint64_t total = 0;
    size_t length = sizeof(total);
    if (sysctlbyname("hw.memsize", &total, &length, nullptr, 0) == 0)
        memory.total = total;
    mach_port_t host = mach_host_self();
    vm_size_t pageSize = 0;
    vm_statistics64_data_t stats{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_page_size(host, &pageSize) == KERN_SUCCESS &&
        host_statistics64(host, HOST_VM_INFO64,
            reinterpret_cast<host_info64_t>(&stats), &count) == KERN_SUCCESS) {
        memory.available = uint64_t(stats.free_count + stats.inactive_count) * pageSize;
        haveAvailable = true;
    }
#endif
    if (!memory.total) memory.total = 512ULL * 1024ULL * 1024ULL;
    if (!haveAvailable) memory.available = memory.total / 2U;
    memory.available = std::min(memory.available, memory.total);
    return memory;
}

std::size_t MemoryBudget(std::size_t archiveBytes) {
    const MachineMemory memory = ReadMachineMemory();
    // Include our archive in the available-memory estimate so normal writes
    // do not lower their own budget. Other applications can still lower it.
    const uint64_t usable = std::min<uint64_t>(memory.total,
        memory.available + std::min<uint64_t>(archiveBytes,
            memory.total - memory.available));
    return static_cast<std::size_t>(std::min(memory.total, usable) / 3U * 2U);
}

}  // namespace

TelemetryStore::TelemetryStore(std::size_t sessionBudgetBytes)
    : sessionBudgetBytes_(sessionBudgetBytes ? sessionBudgetBytes : MemoryBudget(0)),
      sessionBudgetOverride_(sessionBudgetBytes != 0),
      lastBudgetCheck_(std::chrono::steady_clock::now()) {}

void TelemetryStore::AddF32(const std::string& key, float value, float tsec) {
    std::lock_guard lock(mtx_);
    lastSignalUpdate_[key] = std::chrono::steady_clock::now();
    auto& rate = signalRates_[key];
    if (!rate.initialized || tsec < rate.lastSourceSec) {
        rate = SignalRateState{};
        rate.windowStartSec = tsec;
        rate.lastSourceSec = tsec;
        rate.lastValue = value;
        rate.initialized = true;
    } else if (tsec > rate.lastSourceSec) {
        if (value != rate.lastValue &&
            !(std::isnan(value) && std::isnan(rate.lastValue))) {
            ++rate.changesInWindow;
        }
        rate.lastValue = value;
        rate.lastSourceSec = tsec;
        const double elapsed = static_cast<double>(tsec - rate.windowStartSec);
        if (elapsed >= 1.0) {
            rate.hz = static_cast<double>(rate.changesInWindow) / elapsed;
            rate.measured = true;
            rate.windowStartSec = tsec;
            rate.changesInWindow = 0;
        }
    }
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
    lastSignalUpdate_[key] = std::chrono::steady_clock::now();
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
    sessionSampleCount_ = 0;
    ++archiveGeneration_;
    lastSignalUpdate_.clear();
    signalRates_.clear();
    lastFrameAt_ = {};
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
    if (!sessionBudgetOverride_) {
        sessionBudgetBytes_ = MemoryBudget(0);
        lastBudgetCheck_ = std::chrono::steady_clock::now();
    }
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
    if (goodFrames != snap_.goodFrames && goodFrames != 0) {
        lastFrameAt_ = std::chrono::steady_clock::now();
    }
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
    if (lastFrameAt_.time_since_epoch().count() != 0)
        line.frameAgeSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - lastFrameAt_).count();
    return line;
}

TelemetryStore::DeviceView TelemetryStore::GetDeviceView() const {
    std::lock_guard lock(mtx_);
    DeviceView view;
    const auto now = std::chrono::steady_clock::now();
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
    if (lastFrameAt_.time_since_epoch().count() != 0)
        view.stats.frameAgeSeconds = std::chrono::duration<double>(now - lastFrameAt_).count();
    view.latest = snap_.latest;
    view.latestStr = snap_.latestStr;
    for (const auto& [key, time] : lastSignalUpdate_)
        view.ageSeconds[key] = std::chrono::duration<double>(now - time).count();
    return view;
}

std::unordered_map<std::string, SignalHistory> TelemetryStore::CopyHistories(
    const std::vector<std::string>& keys) const {
    std::lock_guard lock(mtx_);
    std::unordered_map<std::string, SignalHistory> result;
    for (const auto& key : keys) {
        const auto it = snap_.hist.find(key);
        if (it != snap_.hist.end()) result.emplace(key, it->second);
    }
    return result;
}

TelemetrySnapshot TelemetryStore::Snapshot() const {
    std::lock_guard lock(mtx_);
    return snap_;
}

RuntimeSessionSnapshot TelemetryStore::SessionSnapshot() const {
    std::lock_guard lock(mtx_);
    return SessionSnapshotLocked(true);
}

RuntimeSessionSnapshot TelemetryStore::SessionMetadataSnapshot() const {
    std::lock_guard lock(mtx_);
    return SessionSnapshotLocked(false);
}

RuntimeSessionSnapshot TelemetryStore::SessionSnapshotLocked(bool includeFloats) const {
    RuntimeSessionSnapshot result;
    result.startedAtUnixMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            sessionStartWall_.time_since_epoch())
            .count();
    result.durationSeconds = SessionElapsedSeconds();
    if (includeFloats) {
        for (const auto& [key, store] : sessionFloatSignals_) {
            SessionSignalHistory history;
            const std::size_t count = SessionSignalSize(store);
            history.t.reserve(count);
            history.y.reserve(count);
            for (auto level = store.levels.rbegin(); level != store.levels.rend(); ++level) {
                for (const auto& sample : *level) {
                    history.t.push_back(sample.t);
                    history.y.push_back(sample.y);
                }
            }
            result.floatSignals.emplace(key, std::move(history));
        }
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

TelemetryStore::SessionRetentionStats TelemetryStore::GetSessionRetentionStats() const {
    std::lock_guard lock(mtx_);
    return {sessionBudgetBytes_, sessionSampleCount_, archiveGeneration_, sessionEpoch_};
}

std::vector<std::string> TelemetryStore::SessionFloatNames() const {
    std::lock_guard lock(mtx_);
    std::vector<std::string> names;
    names.reserve(sessionFloatSignals_.size());
    for (const auto& [name, store] : sessionFloatSignals_) {
        (void)store;
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::size_t TelemetryStore::SessionSignalSize(const SessionSignalStore& store) {
    std::size_t total = 0;
    for (const auto& level : store.levels) total += level.size();
    return total;
}

bool TelemetryStore::CopySessionHistoryPage(
    const std::string& key, std::size_t offset, std::size_t limit,
    std::vector<SessionFloatSample>& samples, std::size_t& total,
    uint64_t& generation) const {
    std::lock_guard lock(mtx_);
    const auto it = sessionFloatSignals_.find(key);
    if (it == sessionFloatSignals_.end()) return false;
    const auto& store = it->second;
    total = SessionSignalSize(store);
    generation = archiveGeneration_;
    samples.clear();
    if (offset >= total || limit == 0) return true;
    samples.reserve(std::min(limit, total - offset));
    for (auto level = store.levels.rbegin(); level != store.levels.rend(); ++level) {
        if (offset >= level->size()) {
            offset -= level->size();
            continue;
        }
        const std::size_t take = std::min({limit - samples.size(),
                                           level->size() - offset,
                                           total - samples.size()});
        for (std::size_t i = 0; i < take; ++i)
            samples.push_back((*level)[offset + i]);
        offset = 0;
        if (samples.size() == limit) break;
    }
    return true;
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

bool TelemetryStore::CopyPlotHistoryInto(
    const std::string& key, float viewSeconds, std::size_t maxPoints,
    std::vector<float>& t, std::vector<float>& y) const {
    std::lock_guard lock(mtx_);
    const auto it = snap_.hist.find(key);
    if (it == snap_.hist.end()) return false;
    const auto& history = it->second;
    t.clear();
    y.clear();
    if (history.t.empty()) return true;
    const float cutoff = history.t.back() - viewSeconds;
    const auto firstIt = std::lower_bound(history.t.begin(), history.t.end(), cutoff);
    const std::size_t first = static_cast<std::size_t>(firstIt - history.t.begin());
    const std::size_t count = history.t.size() - first;
    if (count <= maxPoints) {
        t.assign(firstIt, history.t.end());
        y.assign(history.y.begin() + static_cast<std::ptrdiff_t>(first), history.y.end());
        return true;
    }
    const std::size_t buckets = std::max<std::size_t>(1, maxPoints / 2);
    const std::size_t bucketSize = (count + buckets - 1) / buckets;
    t.reserve(maxPoints + 2);
    y.reserve(maxPoints + 2);
    auto append = [&](std::size_t index) {
        if (!t.empty() && t.back() == history.t[index]) return;
        t.push_back(history.t[index]);
        y.push_back(history.y[index]);
    };
    append(first);
    for (std::size_t begin = first; begin < history.t.size(); begin += bucketSize) {
        const std::size_t end = std::min(history.t.size(), begin + bucketSize);
        std::size_t minIndex = begin, maxIndex = begin;
        bool finiteFound = false;
        for (std::size_t i = begin; i < end; ++i) {
            if (!std::isfinite(history.y[i])) continue;
            if (!finiteFound) {
                minIndex = maxIndex = i;
                finiteFound = true;
            } else {
                if (history.y[i] < history.y[minIndex]) minIndex = i;
                if (history.y[i] > history.y[maxIndex]) maxIndex = i;
            }
        }
        if (minIndex < maxIndex) {
            append(minIndex);
            append(maxIndex);
        } else {
            append(maxIndex);
            append(minIndex);
        }
    }
    append(history.t.size() - 1);
    return true;
}

bool TelemetryStore::CopyStringHistory(const std::string& key, std::size_t limit,
                                       std::vector<SessionStringSample>& samples) const {
    std::lock_guard lock(mtx_);
    const auto it = sessionStringSignals_.find(key);
    if (it == sessionStringSignals_.end()) return false;
    const auto& source = it->second;
    const auto first = source.size() > limit ? source.size() - limit : 0;
    samples.assign(source.begin() + static_cast<std::ptrdiff_t>(first), source.end());
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

std::unordered_map<std::string, TelemetryStore::SignalDisplay>
TelemetryStore::SignalDisplays() const {
    std::lock_guard lock(mtx_);
    std::unordered_map<std::string, SignalDisplay> displays;
    displays.reserve(snap_.latest.size());
    const auto now = std::chrono::steady_clock::now();
    for (const auto& [key, value] : snap_.latest) {
        SignalDisplay display;
        display.value = value;
        const auto rateIt = signalRates_.find(key);
        const auto timeIt = lastSignalUpdate_.find(key);
        if (rateIt != signalRates_.end() && timeIt != lastSignalUpdate_.end() &&
            rateIt->second.measured) {
            const double hz = rateIt->second.hz;
            const double age = std::chrono::duration<double>(now - timeIt->second).count();
            display.updateHz = (snap_.suspended || age > 2.0) ? 0.0 : hz;
        }
        displays.emplace(key, display);
    }
    return displays;
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
    if (store.levels.empty()) store.levels.emplace_back();
    store.levels[0].push_back({t, y});
    ++sessionSampleCount_;
    const auto now = std::chrono::steady_clock::now();
    if (!sessionBudgetOverride_ &&
        now - lastBudgetCheck_ >= std::chrono::seconds(1)) {
        sessionBudgetBytes_ = MemoryBudget(sessionSampleCount_ * sizeof(SessionFloatSample));
        lastBudgetCheck_ = now;
    }
    // A sudden drop in available RAM may require many compactions. Spread
    // that work across incoming samples so the GUI drain stays responsive.
    std::size_t compactedBlocks = 0;
    while (sessionSampleCount_ * sizeof(SessionFloatSample) > sessionBudgetBytes_
           && compactedBlocks++ < 4) {
        if (!CompactOldestSessionBlockLocked()) break;
    }
}

bool TelemetryStore::CompactOldestSessionBlockLocked() {
    SessionSignalStore* selected = nullptr;
    std::size_t selectedLevel = 0;
    float oldest = std::numeric_limits<float>::infinity();
    for (auto& [name, store] : sessionFloatSignals_) {
        (void)name;
        for (std::size_t level = 0; level < store.levels.size(); ++level) {
            if (store.levels[level].size() < 2) continue;
            const float t = store.levels[level].front().t;
            if (t < oldest) {
                oldest = t;
                selected = &store;
                selectedLevel = level;
            }
        }
    }
    if (!selected) return false;
    if (selected->levels.size() <= selectedLevel + 1)
        selected->levels.resize(selectedLevel + 2);
    auto& source = selected->levels[selectedLevel];
    auto& target = selected->levels[selectedLevel + 1];
    const std::size_t pairs = std::min(kSessionCompactionBlock / 2,
                                       source.size() / 2);
    for (std::size_t i = 0; i < pairs; ++i) {
        source.pop_front();
        target.push_back(source.front());
        source.pop_front();
    }
    sessionSampleCount_ -= pairs;
    ++archiveGeneration_;
    return true;
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
