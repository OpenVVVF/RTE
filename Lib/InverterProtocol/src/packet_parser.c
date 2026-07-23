#include "inverter_protocol/packet_parser.h"

#include <string.h>

ivp_result_t ivp_packet_parse(const uint8_t* packet, size_t packet_len,
                              ivp_header_t* header,
                              const uint8_t** payload, uint16_t* payload_len) {
    if (!packet || !header || !payload || !payload_len) return IVP_ERR_MALFORMED;
    if (packet_len < IVP_HEADER_SIZE + 2u) return IVP_ERR_BAD_LENGTH;

    ivp_result_t r = ivp_header_decode(packet, header);
    if (r != IVP_OK) return r;

    const size_t expected = ivp_packet_size(header->payload_len);
    if (packet_len != expected) return IVP_ERR_BAD_LENGTH;

    const uint16_t rx_crc = ivp_read_u16le(packet + IVP_HEADER_SIZE + header->payload_len);
    const uint16_t calc = ivp_crc16_ccitt(packet, IVP_HEADER_SIZE + header->payload_len);
    if (rx_crc != calc) return IVP_ERR_BAD_CRC;

    *payload_len = header->payload_len;
    *payload = (header->payload_len > 0) ? (packet + IVP_HEADER_SIZE) : NULL;
    return IVP_OK;
}

/* ========================================================================
 * DEFINE payload walker
 * ======================================================================== */

ivp_result_t ivp_telemetry_define_iter_init(const uint8_t* payload, uint16_t payload_len,
                                            ivp_define_iter_t* it) {
    if (!it) return IVP_ERR_MALFORMED;
    memset(it, 0, sizeof(*it));
    if (!payload || payload_len == 0) return IVP_ERR_BAD_LENGTH;

    it->payload = payload;
    it->payload_len = payload_len;
    it->total = payload[0];
    it->index = 0;
    it->pos = payload + 1;
    return IVP_OK;
}

bool ivp_telemetry_define_iter_next(ivp_define_iter_t* it,
                                    uint16_t* id, uint8_t* type,
                                    const char** key, uint8_t* key_len) {
    if (!it || it->index >= it->total) return false;

    const uint8_t* end = it->payload + it->payload_len;
    if (end - it->pos < 4) return false;

    *id = ivp_read_u16le(it->pos);
    it->pos += 2;
    *type = *it->pos++;
    *key_len = *it->pos++;

    if ((size_t)(end - it->pos) < *key_len) return false;
    *key = (const char*)it->pos;
    it->pos += *key_len;

    ++it->index;
    return true;
}

/* ========================================================================
 * DATA payload walker
 * ======================================================================== */

ivp_result_t ivp_telemetry_data_iter_init(const uint8_t* payload, uint16_t payload_len,
                                          ivp_data_iter_t* it) {
    if (!it) return IVP_ERR_MALFORMED;
    memset(it, 0, sizeof(*it));
    if (!payload || payload_len == 0) return IVP_ERR_BAD_LENGTH;

    it->payload = payload;
    it->payload_len = payload_len;
    it->total = payload[0];
    it->index = 0;
    it->pos = payload + 1;
    return IVP_OK;
}

static bool data_iter_advance_u8(const uint8_t** p, const uint8_t* end, uint8_t* out) {
    if (*p >= end) return false;
    *out = **p;
    ++(*p);
    return true;
}

static bool data_iter_advance_f32(const uint8_t** p, const uint8_t* end, float* out) {
    if ((size_t)(end - *p) < 4) return false;
    *out = ivp_read_f32le(*p);
    *p += 4;
    return true;
}

bool ivp_telemetry_data_iter_next(ivp_data_iter_t* it, ivp_data_item_t* item) {
    if (!it || it->index >= it->total) return false;

    const uint8_t* end = it->payload + it->payload_len;
    if (end - it->pos < 3) return false;

    item->id = ivp_read_u16le(it->pos);
    it->pos += 2;
    item->type = *it->pos++;

    if (item->type == IVP_VT_F32) {
        if (!data_iter_advance_f32(&it->pos, end, &item->v.f32)) return false;
    } else if (item->type == IVP_VT_STR) {
        uint8_t len;
        if (!data_iter_advance_u8(&it->pos, end, &len)) return false;
        if ((size_t)(end - it->pos) < len) return false;
        item->v.str.data = (const char*)it->pos;
        item->v.str.len = len;
        it->pos += len;
    } else if (item->type == IVP_VT_STR_FRAG) {
        uint8_t frag;
        if (!data_iter_advance_u8(&it->pos, end, &frag)) return false;
        uint8_t len;
        if (!data_iter_advance_u8(&it->pos, end, &len)) return false;
        if ((size_t)(end - it->pos) < len) return false;
        item->v.frag.frag = frag;
        item->v.frag.data = (const char*)it->pos;
        item->v.frag.len = len;
        it->pos += len;
    } else {
        return false;
    }

    ++it->index;
    return true;
}

/* ========================================================================
 * Command request/response walkers
 * ======================================================================== */

static ivp_result_t arg_iter_init(const uint8_t* payload, uint16_t payload_len,
                                  size_t header_bytes, ivp_arg_iter_t* it) {
    if (!it) return IVP_ERR_MALFORMED;
    memset(it, 0, sizeof(*it));
    if (!payload || payload_len < header_bytes) return IVP_ERR_BAD_LENGTH;

    it->payload = payload;
    it->payload_len = payload_len;
    it->pos = payload + header_bytes;
    it->total = payload[header_bytes - 1];
    it->index = 0;
    return IVP_OK;
}

static bool arg_iter_next_internal(ivp_arg_iter_t* it, ivp_arg_t* arg) {
    if (!it || it->index >= it->total) return false;

    const uint8_t* end = it->payload + it->payload_len;
    if (it->pos >= end) return false;

    arg->type = *it->pos++;
    switch (arg->type) {
        case IVP_ARG_U8:
            if (it->pos + 1 > end) return false;
            arg->v.u8 = *it->pos++;
            break;
        case IVP_ARG_U16:
            if (it->pos + 2 > end) return false;
            arg->v.u16 = ivp_read_u16le(it->pos);
            it->pos += 2;
            break;
        case IVP_ARG_U32:
            if (it->pos + 4 > end) return false;
            arg->v.u32 = ivp_read_u32le(it->pos);
            it->pos += 4;
            break;
        case IVP_ARG_F32:
            if (it->pos + 4 > end) return false;
            arg->v.f32 = ivp_read_f32le(it->pos);
            it->pos += 4;
            break;
        case IVP_ARG_STR: {
            if (it->pos + 1 > end) return false;
            uint8_t len = *it->pos++;
            if ((size_t)(end - it->pos) < len) return false;
            arg->v.str.data = (const char*)it->pos;
            arg->v.str.len = len;
            it->pos += len;
            break;
        }
        default:
            return false;
    }

    ++it->index;
    return true;
}

ivp_result_t ivp_command_req_parse(const uint8_t* payload, uint16_t payload_len,
                                   uint8_t* opcode, uint8_t* req_id,
                                   ivp_arg_iter_t* args) {
    if (!payload || payload_len < 3 || !opcode || !req_id || !args)
        return IVP_ERR_MALFORMED;

    *opcode = payload[0];
    *req_id = payload[1];
    return arg_iter_init(payload, payload_len, 3, args);
}

ivp_result_t ivp_command_rsp_parse(const uint8_t* payload, uint16_t payload_len,
                                   uint8_t* req_id, uint8_t* status,
                                   ivp_arg_iter_t* results) {
    if (!payload || payload_len < 3 || !req_id || !status || !results)
        return IVP_ERR_MALFORMED;

    *req_id = payload[0];
    *status = payload[1];
    return arg_iter_init(payload, payload_len, 3, results);
}

bool ivp_arg_iter_next(ivp_arg_iter_t* it, ivp_arg_t* arg) {
    return arg_iter_next_internal(it, arg);
}
