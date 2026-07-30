#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ========================================================================
 * Inverter Protocol (IVP) — shared host/device packet format.
 *
 * This header intentionally uses only C99 stdint/stddef/stdbool so it can
 * be compiled into host applications and bare-metal STM32 firmware.
 * ======================================================================== */

#define IVP_MAGIC   0x544C4D31u /* "TLM1" */
#define IVP_VERSION 1u

/* Maximum payload sizes used by the current Gen5 firmware. */
#define IVP_DEFINE_PAYLOAD_MAX 240u
#define IVP_DATA_PAYLOAD_MAX   600u

/* Key/string limits used by the device. */
#define IVP_KEY_MAX_LEN 32u
#define IVP_STR_MAX_LEN 48u

/* Message types. Values 1-2 are the existing TLM1 telemetry frames.
 * Values 3-6 reserve room for a structured host->device command protocol.
 * Values 7-14 are the CAN session/capability negotiation. */
typedef enum {
    IVP_MSG_TELEMETRY_DATA   = 1,
    IVP_MSG_TELEMETRY_DEFINE = 2,
    IVP_MSG_COMMAND_REQ      = 3,
    IVP_MSG_COMMAND_RSP      = 4,
    IVP_MSG_ACK              = 5,
    IVP_MSG_NACK             = 6,
    /* Session/capability negotiation (CAN, reserved for other transports). */
    IVP_MSG_HELLO            = 7,  /* host->dev: attach request        */
    IVP_MSG_ATTACH_RSP       = 8,  /* dev->host: device info + allow mask */
    IVP_MSG_CAP_REQ          = 9,  /* host->dev: requested cap mask (u8) */
    IVP_MSG_CAP_RSP          = 10, /* dev->host: granted cap mask (u8)   */
    IVP_MSG_DETACH           = 11, /* either: session end                */
    IVP_MSG_HEARTBEAT        = 12, /* host->dev: keepalive (empty)       */
    IVP_MSG_AUTH_REQ         = 13, /* reserved: future password gate     */
    IVP_MSG_AUTH_RSP         = 14, /* reserved: future password gate     */
} ivp_msg_type_t;

/* Capability bits for CAP_REQ/CAP_RSP and the ATTACH_RSP allow mask. */
typedef enum {
    IVP_CAP_TELEMETRY = 0x01,  /* device streams telemetry to the session */
    IVP_CAP_COMMANDS  = 0x02,  /* device accepts shell commands from it   */
    IVP_CAP_FLASH     = 0x04,  /* reserved: in-app firmware update        */
} ivp_capability_t;

/* Value types carried inside telemetry DATA frames. */
typedef enum {
    IVP_VT_F32      = 1,
    IVP_VT_STR      = 2,
    IVP_VT_STR_FRAG = 3,
} ivp_value_type_t;

/* Fragment flags for IVP_VT_STR_FRAG. */
typedef enum {
    IVP_SF_START    = 0x01,
    IVP_SF_END      = 0x02,
    IVP_SF_COMPLETE = 0x03, /* START | END */
} ivp_str_frag_t;

/* Argument/value types for command request/response payloads. */
typedef enum {
    IVP_ARG_U8  = 1,
    IVP_ARG_U16 = 2,
    IVP_ARG_U32 = 3,
    IVP_ARG_F32 = 4,
    IVP_ARG_STR = 5,
} ivp_arg_type_t;

/* 16-byte packed header. The device and host must agree on this layout. */
#ifdef _MSC_VER
#pragma pack(push, 1)
#endif
typedef struct
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((packed))
#endif
{
    uint32_t magic;       /* IVP_MAGIC */
    uint8_t  version;     /* IVP_VERSION */
    uint8_t  msg_type;    /* ivp_msg_type_t */
    uint16_t payload_len; /* bytes */
    uint32_t seq;
    uint32_t time_us;
} ivp_header_t;
#ifdef _MSC_VER
#pragma pack(pop)
#endif

/* Compile-time size check (usable from C and C++). */
#define IVP_HEADER_SIZE 16u

/* Result codes from parser and encoder functions. */
typedef enum {
    IVP_OK = 0,
    IVP_ERR_BUF_TOO_SMALL,
    IVP_ERR_BAD_MAGIC,
    IVP_ERR_BAD_VERSION,
    IVP_ERR_BAD_MSG_TYPE,
    IVP_ERR_BAD_LENGTH,
    IVP_ERR_BAD_CRC,
    IVP_ERR_BAD_VALUE_TYPE,
    IVP_ERR_BAD_ARG_TYPE,
    IVP_ERR_OVERSIZE,
    IVP_ERR_MALFORMED,
} ivp_result_t;

/* ========================================================================
 * Byte-order helpers (little-endian, matches the existing wire format).
 * ======================================================================== */

static inline uint16_t ivp_read_u16le(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t ivp_read_u32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void ivp_write_u16le(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}
static inline void ivp_write_u32le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static inline float ivp_read_f32le(const uint8_t* p) {
    float f;
    memcpy(&f, p, sizeof(f));
    return f;
}
static inline void ivp_write_f32le(uint8_t* p, float v) {
    memcpy(p, &v, sizeof(v));
}

/* ========================================================================
 * CRC16-CCITT (polynomial 0x1021, init 0xFFFF).
 * ======================================================================== */
uint16_t ivp_crc16_ccitt(const uint8_t* data, size_t len);

/* ========================================================================
 * COBS encoding/decoding (used by the UART transport).
 *
 * Encoded length worst case: ceil(len / 254) + len + 1.
 * ivp_cobs_encode returns 0 on output overflow.
 * ivp_cobs_decode returns number of decoded bytes, or 0 on error.
 * ======================================================================== */
size_t ivp_cobs_encode(const uint8_t* in, size_t len, uint8_t* out, size_t out_cap);
size_t ivp_cobs_decode(const uint8_t* in, size_t len, uint8_t* out, size_t out_cap);

/* ========================================================================
 * Packet helpers.
 * ======================================================================== */

/* Validate and fill out a header from a raw buffer. Does not check CRC. */
ivp_result_t ivp_header_decode(const uint8_t* buf, ivp_header_t* out);

/* Compute the CRC over a decoded packet buffer (header + payload). */
uint16_t ivp_packet_crc(const uint8_t* packet, size_t packet_len);

/* Size of the encoded packet including header, payload, and CRC. */
static inline size_t ivp_packet_size(size_t payload_len) {
    return IVP_HEADER_SIZE + payload_len + 2u;
}

#ifdef __cplusplus
} /* extern "C" */
#endif
