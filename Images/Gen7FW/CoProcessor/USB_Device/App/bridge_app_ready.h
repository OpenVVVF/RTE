#ifndef BRIDGE_APP_READY_H
#define BRIDGE_APP_READY_H

#include "inverter_protocol/protocol.h"

/* Telemetry starts in the H7 application loop, after shell RX and hardware
 * initialization. A delimiter alone (including boot noise) is not readiness.
 * Keep this parser bounded and run it only while the bridge is waiting. */
enum {
    BRIDGE_APP_RAW_MAX = IVP_HEADER_SIZE + IVP_DATA_PAYLOAD_MAX + 2u,
    BRIDGE_APP_ENCODED_MAX = BRIDGE_APP_RAW_MAX + BRIDGE_APP_RAW_MAX / 254u + 1u,
    BRIDGE_APP_QUIET_MS = 20u
};

typedef struct {
    uint8_t encoded[BRIDGE_APP_ENCODED_MAX];
    uint16_t length;
    bool overflow;
    bool quiet;
    uint32_t last_rx_ms;
} BridgeAppReady;

static inline void BridgeAppReady_Begin(BridgeAppReady *ready,
                                        uint32_t now_ms, bool wait_for_quiet)
{
    ready->length = 0u;
    ready->overflow = false;
    ready->quiet = wait_for_quiet;
    ready->last_rx_ms = now_ms;
}

static inline bool BridgeAppReady_Feed(BridgeAppReady *ready, uint8_t byte,
                                       uint32_t now_ms)
{
    if (ready->quiet) {
        if ((uint32_t)(now_ms - ready->last_rx_ms) < BRIDGE_APP_QUIET_MS) {
            ready->last_rx_ms = now_ms;
            return false;
        }
        ready->quiet = false;
    }
    ready->last_rx_ms = now_ms;
    if (byte != 0u) {
        if (ready->length < sizeof(ready->encoded))
            ready->encoded[ready->length++] = byte;
        else
            ready->overflow = true;
        return false;
    }

    bool valid = false;
    if (ready->length != 0u && !ready->overflow) {
        uint8_t raw[BRIDGE_APP_RAW_MAX];
        const size_t length = ivp_cobs_decode(ready->encoded, ready->length,
                                             raw, sizeof(raw));
        ivp_header_t header;
        valid = length >= IVP_HEADER_SIZE + 2u &&
                ivp_header_decode(raw, &header) == IVP_OK &&
                (header.msg_type == IVP_MSG_TELEMETRY_DATA ||
                 header.msg_type == IVP_MSG_TELEMETRY_DEFINE) &&
                length == ivp_packet_size(header.payload_len) &&
                ivp_read_u16le(raw + length - 2u) ==
                    ivp_crc16_ccitt(raw, length - 2u);
    }
    ready->length = 0u;
    ready->overflow = false;
    return valid;
}

#endif
