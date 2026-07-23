#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "inverter_protocol/protocol.h"

#include <stddef.h>
#include <stdint.h>

/* ========================================================================
 * Packet parser and payload walkers.
 *
 * The transport layer delivers already-delimited packets (COBS-decoded for
 * UART, reassembled for CAN, etc.). This module validates the header and
 * CRC and walks the payloads.
 * ======================================================================== */

/* Parse a complete packet buffer (header + payload + CRC) and return the
 * decoded header and a pointer to the payload. `payload` is set to NULL and
 * `payload_len` to 0 if there is no payload. */
ivp_result_t ivp_packet_parse(const uint8_t* packet, size_t packet_len,
                              ivp_header_t* header,
                              const uint8_t** payload, uint16_t* payload_len);

/* ========================================================================
 * Telemetry DEFINE payload walker
 * Format: count(1), [id(2) type(1) key_len(1) key(key_len)]*
 * ======================================================================== */

typedef struct {
    const uint8_t* payload;
    uint16_t       payload_len;
    uint8_t        total;
    uint8_t        index;
    const uint8_t* pos;
} ivp_define_iter_t;

ivp_result_t ivp_telemetry_define_iter_init(const uint8_t* payload, uint16_t payload_len,
                                            ivp_define_iter_t* it);
bool         ivp_telemetry_define_iter_next(ivp_define_iter_t* it,
                                            uint16_t* id, uint8_t* type,
                                            const char** key, uint8_t* key_len);

/* ========================================================================
 * Telemetry DATA payload walker
 * Format: count(1), [id(2) wire_type(1) value]*
 * ======================================================================== */

typedef struct {
    const uint8_t* payload;
    uint16_t       payload_len;
    uint8_t        total;
    uint8_t        index;
    const uint8_t* pos;
} ivp_data_iter_t;

typedef struct {
    uint16_t id;
    uint8_t  type; /* ivp_value_type_t */
    union {
        float    f32;
        struct { const char* data; uint8_t len; } str;
        struct { uint8_t frag; const char* data; uint8_t len; } frag;
    } v;
} ivp_data_item_t;

ivp_result_t ivp_telemetry_data_iter_init(const uint8_t* payload, uint16_t payload_len,
                                          ivp_data_iter_t* it);
bool         ivp_telemetry_data_iter_next(ivp_data_iter_t* it, ivp_data_item_t* item);

/* ========================================================================
 * Command request/response payload walkers
 * ======================================================================== */

typedef struct {
    const uint8_t* payload;
    uint16_t       payload_len;
    const uint8_t* pos;
    uint8_t        total;
    uint8_t        index;
} ivp_arg_iter_t;

typedef struct {
    uint8_t type; /* ivp_arg_type_t */
    union {
        uint8_t  u8;
        uint16_t u16;
        uint32_t u32;
        float    f32;
        struct { const char* data; uint8_t len; } str;
    } v;
} ivp_arg_t;

/* Request: opcode(1) req_id(1) arg_count(1) [arg_type(1) value]* */
ivp_result_t ivp_command_req_parse(const uint8_t* payload, uint16_t payload_len,
                                   uint8_t* opcode, uint8_t* req_id,
                                   ivp_arg_iter_t* args);

/* Response: req_id(1) status(1) result_count(1) [arg_type(1) value]* */
ivp_result_t ivp_command_rsp_parse(const uint8_t* payload, uint16_t payload_len,
                                   uint8_t* req_id, uint8_t* status,
                                   ivp_arg_iter_t* results);

bool ivp_arg_iter_next(ivp_arg_iter_t* it, ivp_arg_t* arg);

#ifdef __cplusplus
} /* extern "C" */
#endif
