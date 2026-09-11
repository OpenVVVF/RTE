#pragma once

#include "inverter_protocol/packet_parser.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace ivp {

/* Reassembles fragmented string telemetry (IVP_VT_STR_FRAG) into complete
 * messages, mirroring Source/NodeGUI IvpStreamDecoder semantics:
 *  - IVP_SF_START clears any partial buffer collected for the key,
 *  - fragments append in arrival order,
 *  - IVP_SF_END delivers the assembled message,
 *  - a plain IVP_VT_STR supersedes and discards any partial buffer,
 *  - partials with no new fragment for > kStaleUs of device time are dropped
 *    via expireStale().
 *
 * Fragments that arrive without a prior START still collect (an END then
 * delivers what was seen); a new START mid-message discards the old partial.
 */
class StringFragmentReassembler {
public:
    static constexpr uint32_t kStaleUs = 2000000u;

    /* Feed one STR/STR_FRAG DATA item. Returns true when `out` holds a
     * complete message to deliver (plain STR, or an END-terminated
     * fragment sequence). Other value types always return false. */
    bool handle(const std::string& key, const ivp_data_item_t& item,
                uint32_t time_us, std::string& out) {
        if (item.type == IVP_VT_STR) {
            partials_.erase(key);
            out.assign(item.v.str.data, item.v.str.len);
            return true;
        }
        if (item.type != IVP_VT_STR_FRAG) return false;

        auto& part = partials_[key];
        if (item.v.frag.frag & IVP_SF_START) part.buf.clear();
        part.buf.append(item.v.frag.data, item.v.frag.len);
        part.last_time_us = time_us;
        if (item.v.frag.frag & IVP_SF_END) {
            out = std::move(part.buf);
            partials_.erase(key);
            return true;
        }
        return false;
    }

    /* Drop partials idle for more than kStaleUs of device time. Unsigned
     * subtraction tolerates time_us wraparound. Call once per DATA frame. */
    void expireStale(uint32_t time_us) {
        for (auto it = partials_.begin(); it != partials_.end();) {
            if (time_us - it->second.last_time_us > kStaleUs) {
                it = partials_.erase(it);
            } else {
                ++it;
            }
        }
    }

    /* Number of keys with an in-flight fragment sequence (test hook). */
    size_t partialCount() const { return partials_.size(); }

private:
    struct Partial {
        std::string buf;
        uint32_t last_time_us = 0;
    };
    std::unordered_map<std::string, Partial> partials_;
};

} // namespace ivp
