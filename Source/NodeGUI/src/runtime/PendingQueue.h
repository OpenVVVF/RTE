#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace NodeGUI::runtime {

// Item types for the RuntimeController producer -> GUI handoff queue. They
// mirror the TelemetryStore mutation they cause once drained.
struct QueuedF32 {
    std::string key;
    float value;
    float tsec;
};
struct QueuedString {
    std::string key;
    std::string value;
};
struct QueuedConsole {
    std::string text;
};
struct QueuedStats {
    float rxHz;
    float rxBytesPerSec;
    uint64_t goodFrames;
    uint64_t badFrames;
    uint64_t rejectCrc;
    uint64_t rejectHdr;
    uint64_t rejectLen;
    uint64_t rejectPayloadParse;
    uint64_t rejectUnknownId;
    uint32_t seq;
};
using PendingItem =
    std::variant<QueuedF32, QueuedString, QueuedConsole, QueuedStats>;

// Bounded handoff queue between the telemetry client callbacks (producer
// threads) and the GUI thread's drain timer.
//
// Policy: past kCoalesceThreshold (the GUI is stalled, e.g. behind a modal
// dialog) F32 and string items coalesce per key — newest value wins, writing
// into the key's pending slot — and past kHardCap new F32/string items are
// dropped. QueuedConsole and QueuedStats items always append: they are the
// operator-visible record of what the device said and did, and their volume
// is small next to telemetry. Dropped and coalesced counts are returned by
// Drain() so the consumer can surface them (throttled) in the console.
//
// All members are guarded internally; Push()/Drain() may race freely.
class PendingQueue {
public:
    static constexpr std::size_t kCoalesceThreshold = 4096;
    static constexpr std::size_t kHardCap = 65536;

    void Push(PendingItem item) {
        std::lock_guard lock(mtx_);
        if (queue_.size() >= kCoalesceThreshold) {
            if (const auto* f32 = std::get_if<QueuedF32>(&item)) {
                auto [it, inserted] =
                    f32Index_.try_emplace(f32->key, queue_.size());
                if (!inserted) {
                    // Newest value wins the existing slot (position kept).
                    queue_[it->second] = std::move(item);
                    ++coalesced_;
                    return;
                }
                if (queue_.size() >= kHardCap) {
                    f32Index_.erase(it);
                    ++dropped_;
                    return;
                }
                queue_.push_back(std::move(item));
                return;
            }
            if (const auto* str = std::get_if<QueuedString>(&item)) {
                auto [it, inserted] =
                    stringIndex_.try_emplace(str->key, queue_.size());
                if (!inserted) {
                    queue_[it->second] = std::move(item);
                    ++coalesced_;
                    return;
                }
                if (queue_.size() >= kHardCap) {
                    stringIndex_.erase(it);
                    ++dropped_;
                    return;
                }
                queue_.push_back(std::move(item));
                return;
            }
            // QueuedConsole / QueuedStats always append (see class comment).
        }
        queue_.push_back(std::move(item));
    }

    // Moves everything out and resets the coalescing state. Positions in the
    // key maps become stale with the swap, so the maps never outlive a drain.
    std::vector<PendingItem> Drain(uint64_t& coalesced, uint64_t& dropped) {
        std::lock_guard lock(mtx_);
        coalesced = coalesced_;
        dropped = dropped_;
        coalesced_ = 0;
        dropped_ = 0;
        f32Index_.clear();
        stringIndex_.clear();
        std::vector<PendingItem> out;
        out.swap(queue_);
        return out;
    }

private:
    std::mutex mtx_;
    std::vector<PendingItem> queue_;
    // key → pending queue slot, valid only while the queue holds the item.
    std::unordered_map<std::string, std::size_t> f32Index_;
    std::unordered_map<std::string, std::size_t> stringIndex_;
    uint64_t dropped_ = 0;
    uint64_t coalesced_ = 0;
};

}  // namespace NodeGUI::runtime
