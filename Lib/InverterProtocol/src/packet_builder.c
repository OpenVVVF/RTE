#include "inverter_protocol/packet_builder.h"

#include <string.h>

static inline bool check_space(size_t used, size_t cap, size_t need) {
    return (used + need <= cap);
}

/* ========================================================================
 * Telemetry DEFINE payload builder
 * Format: count(1), [id(2) type(1) key_len(1) key(key_len)]*
 * ======================================================================== */

ivp_result_t ivp_telemetry_define_begin(ivp_define_builder_t* b, uint8_t* buf, size_t cap) {
    if (!b || !buf || cap < 1) return IVP_ERR_BUF_TOO_SMALL;
    b->buf = buf;
    b->cap = cap;
    b->len = 1;
    b->count_ptr = buf;
    b->count = 0;
    buf[0] = 0;
    return IVP_OK;
}

static ivp_result_t ivp_define_add_common(ivp_define_builder_t* b, uint16_t id,
                                          uint8_t type, const char* key, uint8_t key_len) {
    if (!b) return IVP_ERR_MALFORMED;
    if (key_len > IVP_KEY_MAX_LEN) key_len = IVP_KEY_MAX_LEN;

    const size_t need = 2u + 1u + 1u + key_len;
    if (!check_space(b->len, b->cap, need)) return IVP_ERR_BUF_TOO_SMALL;

    uint8_t* w = b->buf + b->len;
    ivp_write_u16le(w, id); w += 2;
    *w++ = type;
    *w++ = key_len;
    if (key_len) {
        memcpy(w, key, key_len);
        w += key_len;
    }

    b->len += need;
    b->count_ptr[0] = ++b->count;
    return IVP_OK;
}

ivp_result_t ivp_telemetry_define_add_f32(ivp_define_builder_t* b, uint16_t id,
                                          const char* key, uint8_t key_len) {
    return ivp_define_add_common(b, id, IVP_VT_F32, key, key_len);
}

ivp_result_t ivp_telemetry_define_add_str(ivp_define_builder_t* b, uint16_t id,
                                          const char* key, uint8_t key_len) {
    return ivp_define_add_common(b, id, IVP_VT_STR, key, key_len);
}

/* ========================================================================
 * Telemetry DATA payload builder
 * Format: count(1), [id(2) type(1) value]*
 * ======================================================================== */

ivp_result_t ivp_telemetry_data_begin(ivp_data_builder_t* b, uint8_t* buf, size_t cap) {
    if (!b || !buf || cap < 1) return IVP_ERR_BUF_TOO_SMALL;
    b->buf = buf;
    b->cap = cap;
    b->len = 1;
    b->count_ptr = buf;
    b->count = 0;
    buf[0] = 0;
    return IVP_OK;
}

ivp_result_t ivp_telemetry_data_add_f32(ivp_data_builder_t* b, uint16_t id, float value) {
    if (!b) return IVP_ERR_MALFORMED;
    const size_t need = 2u + 1u + 4u;
    if (!check_space(b->len, b->cap, need)) return IVP_ERR_BUF_TOO_SMALL;

    uint8_t* w = b->buf + b->len;
    ivp_write_u16le(w, id); w += 2;
    *w++ = IVP_VT_F32;
    ivp_write_f32le(w, value); w += 4;

    b->len += need;
    b->count_ptr[0] = ++b->count;
    return IVP_OK;
}

static ivp_result_t ivp_data_add_str_common(ivp_data_builder_t* b, uint16_t id,
                                            uint8_t type, uint8_t frag_flags,
                                            const char* value, uint8_t len) {
    if (!b) return IVP_ERR_MALFORMED;
    if (len > IVP_STR_MAX_LEN) len = IVP_STR_MAX_LEN;

    const size_t need = 2u + 1u + (type == IVP_VT_STR_FRAG ? 1u : 0u) + 1u + len;
    if (!check_space(b->len, b->cap, need)) return IVP_ERR_BUF_TOO_SMALL;

    uint8_t* w = b->buf + b->len;
    ivp_write_u16le(w, id); w += 2;
    *w++ = type;
    if (type == IVP_VT_STR_FRAG) *w++ = frag_flags;
    *w++ = len;
    if (len) {
        memcpy(w, value, len);
        w += len;
    }

    b->len += need;
    b->count_ptr[0] = ++b->count;
    return IVP_OK;
}

ivp_result_t ivp_telemetry_data_add_str(ivp_data_builder_t* b, uint16_t id,
                                        const char* value, uint8_t len) {
    return ivp_data_add_str_common(b, id, IVP_VT_STR, 0, value, len);
}

ivp_result_t ivp_telemetry_data_add_str_frag(ivp_data_builder_t* b, uint16_t id,
                                             uint8_t frag_flags,
                                             const char* value, uint8_t len) {
    return ivp_data_add_str_common(b, id, IVP_VT_STR_FRAG, frag_flags, value, len);
}

/* ========================================================================
 * Command request payload builder
 * Format: opcode(1) req_id(1) arg_count(1) [arg_type(1) arg_value]*
 * ======================================================================== */

ivp_result_t ivp_command_req_begin(ivp_command_req_builder_t* b, uint8_t* buf, size_t cap,
                                   uint8_t opcode, uint8_t req_id) {
    if (!b || !buf || cap < 3) return IVP_ERR_BUF_TOO_SMALL;
    b->buf = buf;
    b->cap = cap;
    buf[0] = opcode;
    buf[1] = req_id;
    buf[2] = 0;
    b->len = 3;
    b->count_ptr = buf + 2;
    b->count = 0;
    return IVP_OK;
}

static ivp_result_t ivp_command_req_add_arg(ivp_command_req_builder_t* b,
                                            uint8_t type, const uint8_t* data, uint8_t len) {
    if (!b) return IVP_ERR_MALFORMED;
    const size_t need = 1u + len;
    if (!check_space(b->len, b->cap, need)) return IVP_ERR_BUF_TOO_SMALL;

    uint8_t* w = b->buf + b->len;
    *w++ = type;
    memcpy(w, data, len);
    b->len += need;
    b->count_ptr[0] = ++b->count;
    return IVP_OK;
}

ivp_result_t ivp_command_req_add_u8(ivp_command_req_builder_t* b, uint8_t v) {
    return ivp_command_req_add_arg(b, IVP_ARG_U8, &v, sizeof(v));
}

ivp_result_t ivp_command_req_add_u16(ivp_command_req_builder_t* b, uint16_t v) {
    uint8_t tmp[2];
    ivp_write_u16le(tmp, v);
    return ivp_command_req_add_arg(b, IVP_ARG_U16, tmp, sizeof(tmp));
}

ivp_result_t ivp_command_req_add_u32(ivp_command_req_builder_t* b, uint32_t v) {
    uint8_t tmp[4];
    ivp_write_u32le(tmp, v);
    return ivp_command_req_add_arg(b, IVP_ARG_U32, tmp, sizeof(tmp));
}

ivp_result_t ivp_command_req_add_f32(ivp_command_req_builder_t* b, float v) {
    uint8_t tmp[4];
    ivp_write_f32le(tmp, v);
    return ivp_command_req_add_arg(b, IVP_ARG_F32, tmp, sizeof(tmp));
}

ivp_result_t ivp_command_req_add_str(ivp_command_req_builder_t* b,
                                     const char* s, uint8_t len) {
    if (!b) return IVP_ERR_MALFORMED;
    if (len > IVP_STR_MAX_LEN) len = IVP_STR_MAX_LEN;

    const size_t need = 1u + 1u + len;
    if (!check_space(b->len, b->cap, need)) return IVP_ERR_BUF_TOO_SMALL;

    uint8_t* w = b->buf + b->len;
    *w++ = IVP_ARG_STR;
    *w++ = len;
    if (len) memcpy(w, s, len);
    b->len += need;
    b->count_ptr[0] = ++b->count;
    return IVP_OK;
}

/* ========================================================================
 * Command response payload builder
 * Format: req_id(1) status(1) result_count(1) [arg_type(1) value]*
 * ======================================================================== */

ivp_result_t ivp_command_rsp_begin(ivp_command_rsp_builder_t* b, uint8_t* buf, size_t cap,
                                   uint8_t req_id, uint8_t status) {
    if (!b || !buf || cap < 3) return IVP_ERR_BUF_TOO_SMALL;
    b->buf = buf;
    b->cap = cap;
    buf[0] = req_id;
    buf[1] = status;
    buf[2] = 0;
    b->len = 3;
    b->count_ptr = buf + 2;
    b->count = 0;
    return IVP_OK;
}

static ivp_result_t ivp_command_rsp_add_result(ivp_command_rsp_builder_t* b,
                                               uint8_t type, const uint8_t* data, uint8_t len) {
    if (!b) return IVP_ERR_MALFORMED;
    const size_t need = 1u + len;
    if (!check_space(b->len, b->cap, need)) return IVP_ERR_BUF_TOO_SMALL;

    uint8_t* w = b->buf + b->len;
    *w++ = type;
    memcpy(w, data, len);
    b->len += need;
    b->count_ptr[0] = ++b->count;
    return IVP_OK;
}

ivp_result_t ivp_command_rsp_add_u8(ivp_command_rsp_builder_t* b, uint8_t v) {
    return ivp_command_rsp_add_result(b, IVP_ARG_U8, &v, sizeof(v));
}

ivp_result_t ivp_command_rsp_add_u16(ivp_command_rsp_builder_t* b, uint16_t v) {
    uint8_t tmp[2];
    ivp_write_u16le(tmp, v);
    return ivp_command_rsp_add_result(b, IVP_ARG_U16, tmp, sizeof(tmp));
}

ivp_result_t ivp_command_rsp_add_u32(ivp_command_rsp_builder_t* b, uint32_t v) {
    uint8_t tmp[4];
    ivp_write_u32le(tmp, v);
    return ivp_command_rsp_add_result(b, IVP_ARG_U32, tmp, sizeof(tmp));
}

ivp_result_t ivp_command_rsp_add_f32(ivp_command_rsp_builder_t* b, float v) {
    uint8_t tmp[4];
    ivp_write_f32le(tmp, v);
    return ivp_command_rsp_add_result(b, IVP_ARG_F32, tmp, sizeof(tmp));
}

ivp_result_t ivp_command_rsp_add_str(ivp_command_rsp_builder_t* b,
                                     const char* s, uint8_t len) {
    if (!b) return IVP_ERR_MALFORMED;
    if (len > IVP_STR_MAX_LEN) len = IVP_STR_MAX_LEN;

    const size_t need = 1u + 1u + len;
    if (!check_space(b->len, b->cap, need)) return IVP_ERR_BUF_TOO_SMALL;

    uint8_t* w = b->buf + b->len;
    *w++ = IVP_ARG_STR;
    *w++ = len;
    if (len) memcpy(w, s, len);
    b->len += need;
    b->count_ptr[0] = ++b->count;
    return IVP_OK;
}

/* ========================================================================
 * Full packet encoding
 * ======================================================================== */

ivp_result_t ivp_packet_encode(uint8_t msg_type, uint32_t seq, uint32_t time_us,
                               const uint8_t* payload, uint16_t payload_len,
                               uint8_t* out, size_t out_cap, size_t* out_len) {
    if (!out) return IVP_ERR_MALFORMED;

    const size_t need = ivp_packet_size(payload_len);
    if (out_cap < need) return IVP_ERR_BUF_TOO_SMALL;

    ivp_header_t h;
    h.magic = IVP_MAGIC;
    h.version = IVP_VERSION;
    h.msg_type = msg_type;
    h.payload_len = payload_len;
    h.seq = seq;
    h.time_us = time_us;

    uint8_t* w = out;
    memcpy(w, &h, IVP_HEADER_SIZE);
    w += IVP_HEADER_SIZE;

    if (payload_len) {
        memcpy(w, payload, payload_len);
        w += payload_len;
    }

    const uint16_t crc = ivp_crc16_ccitt(out, IVP_HEADER_SIZE + payload_len);
    ivp_write_u16le(w, crc);
    w += 2;

    if (out_len) *out_len = need;
    return IVP_OK;
}
