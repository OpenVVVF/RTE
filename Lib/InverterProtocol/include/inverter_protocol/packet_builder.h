#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "inverter_protocol/protocol.h"

#include <stddef.h>
#include <stdint.h>

/* ========================================================================
 * Payload builders for telemetry DEFINE/DATA frames and command frames.
 *
 * All builders write into caller-provided buffers and return IVP_OK on
 * success. They intentionally avoid dynamic allocation.
 * ======================================================================== */

/* ---------- Telemetry DEFINE payload ---------- */

typedef struct {
    uint8_t* buf;
    size_t   cap;
    size_t   len;
    uint8_t* count_ptr;
    uint8_t  count;
} ivp_define_builder_t;

ivp_result_t ivp_telemetry_define_begin(ivp_define_builder_t* b, uint8_t* buf, size_t cap);
ivp_result_t ivp_telemetry_define_add_f32(ivp_define_builder_t* b, uint16_t id,
                                          const char* key, uint8_t key_len);
ivp_result_t ivp_telemetry_define_add_str(ivp_define_builder_t* b, uint16_t id,
                                          const char* key, uint8_t key_len);

/* ---------- Telemetry DATA payload ---------- */

typedef struct {
    uint8_t* buf;
    size_t   cap;
    size_t   len;
    uint8_t* count_ptr;
    uint8_t  count;
} ivp_data_builder_t;

ivp_result_t ivp_telemetry_data_begin(ivp_data_builder_t* b, uint8_t* buf, size_t cap);
ivp_result_t ivp_telemetry_data_add_f32(ivp_data_builder_t* b, uint16_t id, float value);
ivp_result_t ivp_telemetry_data_add_str(ivp_data_builder_t* b, uint16_t id,
                                        const char* value, uint8_t len);
ivp_result_t ivp_telemetry_data_add_str_frag(ivp_data_builder_t* b, uint16_t id,
                                             uint8_t frag_flags,
                                             const char* value, uint8_t len);

/* ---------- Command request payload ---------- */

typedef struct {
    uint8_t* buf;
    size_t   cap;
    size_t   len;
    uint8_t* count_ptr;
    uint8_t  count;
} ivp_command_req_builder_t;

ivp_result_t ivp_command_req_begin(ivp_command_req_builder_t* b, uint8_t* buf, size_t cap,
                                   uint8_t opcode, uint8_t req_id);
ivp_result_t ivp_command_req_add_u8 (ivp_command_req_builder_t* b, uint8_t v);
ivp_result_t ivp_command_req_add_u16(ivp_command_req_builder_t* b, uint16_t v);
ivp_result_t ivp_command_req_add_u32(ivp_command_req_builder_t* b, uint32_t v);
ivp_result_t ivp_command_req_add_f32(ivp_command_req_builder_t* b, float v);
ivp_result_t ivp_command_req_add_str(ivp_command_req_builder_t* b,
                                     const char* s, uint8_t len);

/* ---------- Command response payload ---------- */

typedef struct {
    uint8_t* buf;
    size_t   cap;
    size_t   len;
    uint8_t* count_ptr;
    uint8_t  count;
} ivp_command_rsp_builder_t;

ivp_result_t ivp_command_rsp_begin(ivp_command_rsp_builder_t* b, uint8_t* buf, size_t cap,
                                   uint8_t req_id, uint8_t status);
ivp_result_t ivp_command_rsp_add_u8 (ivp_command_rsp_builder_t* b, uint8_t v);
ivp_result_t ivp_command_rsp_add_u16(ivp_command_rsp_builder_t* b, uint16_t v);
ivp_result_t ivp_command_rsp_add_u32(ivp_command_rsp_builder_t* b, uint32_t v);
ivp_result_t ivp_command_rsp_add_f32(ivp_command_rsp_builder_t* b, float v);
ivp_result_t ivp_command_rsp_add_str(ivp_command_rsp_builder_t* b,
                                     const char* s, uint8_t len);

/* ---------- Full packet encoding ---------- */

/* Encode a complete packet (header + payload + CRC) into `out`.
 * Returns IVP_OK or IVP_ERR_BUF_TOO_SMALL. */
ivp_result_t ivp_packet_encode(uint8_t msg_type, uint32_t seq, uint32_t time_us,
                               const uint8_t* payload, uint16_t payload_len,
                               uint8_t* out, size_t out_cap, size_t* out_len);

#ifdef __cplusplus
} /* extern "C" */
#endif
