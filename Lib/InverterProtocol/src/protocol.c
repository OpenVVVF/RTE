#include "inverter_protocol/protocol.h"

#include <string.h>

/* CRC16-CCITT, polynomial 0x1021, initial value 0xFFFF. */
uint16_t ivp_crc16_ccitt(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* Consistent Overhead Byte Stuffing (COBS) encoder. */
size_t ivp_cobs_encode(const uint8_t* in, size_t len, uint8_t* out, size_t out_cap) {
    if (out_cap == 0) return 0;

    size_t read_i = 0;
    size_t write_i = 1;
    size_t code_i = 0;
    uint8_t code = 1;

    while (read_i < len) {
        if (in[read_i] == 0) {
            if (write_i >= out_cap) return 0;
            out[code_i] = code;
            code = 1;
            code_i = write_i++;
            ++read_i;
            continue;
        }

        if (write_i >= out_cap) return 0;
        out[write_i++] = in[read_i++];
        if (++code == 0xFF) {
            if (write_i >= out_cap) return 0;
            out[code_i] = code;
            code = 1;
            code_i = write_i++;
        }
    }

    if (code_i >= out_cap) return 0;
    out[code_i] = code;
    return write_i;
}

/* COBS decoder. Returns decoded length, or 0 on error. */
size_t ivp_cobs_decode(const uint8_t* in, size_t len, uint8_t* out, size_t out_cap) {
    size_t r = 0;
    size_t w = 0;

    while (r < len) {
        uint8_t code = in[r++];
        if (code == 0) return 0; /* invalid code byte */

        for (uint8_t i = 1; i < code; ++i) {
            if (r >= len) return 0;
            if (w >= out_cap) return 0;
            out[w++] = in[r++];
        }

        if (code != 0xFF && r < len) {
            if (w >= out_cap) return 0;
            out[w++] = 0;
        }
    }

    return w;
}

ivp_result_t ivp_header_decode(const uint8_t* buf, ivp_header_t* out) {
    if (!buf || !out) return IVP_ERR_MALFORMED;

    ivp_header_t h;
    h.magic       = ivp_read_u32le(buf + 0);
    h.version     = buf[4];
    h.msg_type    = buf[5];
    h.payload_len = ivp_read_u16le(buf + 6);
    h.seq         = ivp_read_u32le(buf + 8);
    h.time_us     = ivp_read_u32le(buf + 12);

    if (h.magic != IVP_MAGIC)   return IVP_ERR_BAD_MAGIC;
    if (h.version != IVP_VERSION) return IVP_ERR_BAD_VERSION;
    if (h.msg_type != IVP_MSG_TELEMETRY_DATA &&
        h.msg_type != IVP_MSG_TELEMETRY_DEFINE &&
        h.msg_type != IVP_MSG_COMMAND_REQ &&
        h.msg_type != IVP_MSG_COMMAND_RSP &&
        h.msg_type != IVP_MSG_ACK &&
        h.msg_type != IVP_MSG_NACK) {
        return IVP_ERR_BAD_MSG_TYPE;
    }

    *out = h;
    return IVP_OK;
}

uint16_t ivp_packet_crc(const uint8_t* packet, size_t packet_len) {
    return ivp_crc16_ccitt(packet, packet_len);
}
