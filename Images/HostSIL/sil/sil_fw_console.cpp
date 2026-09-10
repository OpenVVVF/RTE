/*
 * sil_fw_console.cpp — see sil_fw_console.h.  Mirrors firmware Telemetry
 * "print" strings (USART3 COBS/InverterProtocol byte stream) onto stdout.
 *
 * Wire format recap (Lib/InverterProtocol):
 *   frame   = ivp_cobs_encode(packet) followed by a 0x00 delimiter
 *   packet  = 16-byte header (magic/version/type/payload_len/seq/time_us)
 *             + payload + CRC16-CCITT
 *   DEFINE payloads map dynamic ids -> keys; the firmware's printf strings
 *   travel as "print" key values inside DATA frames (VT_STR, or VT_STR_FRAG
 *   sequences for lines longer than STR_MAXLEN).
 */
#include "sil_fw_console.h"

#include <inverter_protocol/protocol.h>
#include <inverter_protocol/packet_parser.h>

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr size_t kAccCap = 32 * 1024;   /* accumulator bound (resync cap) */

std::vector<uint8_t>                  s_acc;
std::unordered_map<uint16_t, std::string>  s_key_name;
std::string                           s_frag;
bool                                  s_enabled = true;

void emitLine(uint32_t time_us, const std::string& text) {
    std::printf("[FW t=%8.3f] %s\n", static_cast<double>(time_us) / 1e6,
                text.c_str());
    std::fflush(stdout);
}

void onDataItem(uint32_t time_us, const ivp_data_item_t& item) {
    if (item.type != IVP_VT_STR && item.type != IVP_VT_STR_FRAG) return;
    const auto it = s_key_name.find(item.id);
    if (it == s_key_name.end() || it->second != "print") return;

    if (item.type == IVP_VT_STR) {
        emitLine(time_us, std::string(item.v.str.data, item.v.str.len));
        return;
    }
    /* Fragmented string: START begins a fresh buffer, END flushes. */
    const uint8_t frag = item.v.frag.frag;
    if ((frag & IVP_SF_START) != 0U) s_frag.clear();
    s_frag.append(item.v.frag.data, item.v.frag.len);
    if ((frag & IVP_SF_END) != 0U) {
        emitLine(time_us, s_frag);
        s_frag.clear();
    }
}

void onPacket(const uint8_t* frame, size_t frame_len) {
    if (frame_len == 0) return;
    std::vector<uint8_t> decoded(frame_len);
    const size_t n = ivp_cobs_decode(frame, frame_len, decoded.data(),
                                     decoded.size());
    if (n == 0) return;   /* not a valid encoder frame: resync by dropping */

    ivp_header_t hdr{};
    const uint8_t* payload = nullptr;
    uint16_t payload_len = 0;
    if (ivp_packet_parse(decoded.data(), n, &hdr, &payload, &payload_len) !=
        IVP_OK) {
        return;
    }

    if (hdr.msg_type == IVP_MSG_TELEMETRY_DEFINE) {
        ivp_define_iter_t it;
        if (ivp_telemetry_define_iter_init(payload, payload_len, &it) != IVP_OK)
            return;
        uint16_t id = 0;
        uint8_t type = 0;
        const char* key = nullptr;
        uint8_t key_len = 0;
        while (ivp_telemetry_define_iter_next(&it, &id, &type, &key, &key_len)) {
            (void)type;   /* only "print"-keyed strings are mirrored */
            s_key_name[id] = std::string(key, key_len);
        }
    } else if (hdr.msg_type == IVP_MSG_TELEMETRY_DATA) {
        ivp_data_iter_t it;
        if (ivp_telemetry_data_iter_init(payload, payload_len, &it) != IVP_OK)
            return;
        ivp_data_item_t item;
        while (ivp_telemetry_data_iter_next(&it, &item)) {
            onDataItem(hdr.time_us, item);
        }
    }
}

} // namespace

void silFwConsoleReset() {
    s_acc.clear();
    s_key_name.clear();
    s_frag.clear();
}

void silFwConsoleSetEnabled(bool enabled) { s_enabled = enabled; }

void silFwConsoleFeed(const uint8_t* data, size_t len) {
    if (!s_enabled || data == nullptr || len == 0) return;
    s_acc.insert(s_acc.end(), data, data + len);
    if (s_acc.size() > kAccCap) {
        /* Garbage on the wire (should not happen on the modeled UART): keep
         * the newest half so decoding can resync on the next delimiter. */
        s_acc.erase(s_acc.begin(), s_acc.begin() + s_acc.size() / 2);
    }

    for (;;) {
        auto it = std::find(s_acc.begin(), s_acc.end(), uint8_t{0});
        if (it == s_acc.end()) return;
        onPacket(s_acc.data(), static_cast<size_t>(it - s_acc.begin()));
        s_acc.erase(s_acc.begin(), it + 1);
    }
}
