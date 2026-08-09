#ifndef INVERTER_PROTOCOL_TRACE_PROTOCOL_H
#define INVERTER_PROTOCOL_TRACE_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IVP_TRACE_MAGIC 0xA7U
#define IVP_TRACE_VERSION 1U
#define IVP_TRACE_PAYLOAD_SIZE 64U
#define IVP_TRACE_CHANNELS 8U
#define IVP_TRACE_MAX_CHANNELS 32U
#define IVP_TRACE_SAMPLES_PER_DATA_FRAME 3U
#define IVP_TRACE_SCHEMA_NAME_SIZE 32U

typedef enum {
    IVP_TRACE_FRAME_DATA = 1,
    IVP_TRACE_FRAME_SCHEMA = 2,
    IVP_TRACE_FRAME_STATUS = 3,
    IVP_TRACE_FRAME_EVENT = 4
} ivp_trace_frame_type_t;

typedef struct {
    uint8_t capture_id;
    uint32_t first_sample_sequence;
    uint32_t first_sample_cycles;
    /* DWT deltas from sample 0->1 and 1->2, divided by 8.  This gives
     * sub-20 ns resolution at H723 clocks while preserving the 64-byte frame. */
    uint16_t delta_cycles_div8[2];
    int16_t samples[IVP_TRACE_SAMPLES_PER_DATA_FRAME][IVP_TRACE_CHANNELS];
} ivp_trace_data_frame_t;

typedef struct {
    uint8_t capture_id;
    uint8_t channel; /* 0..7 fast, 8..31 sparse event/snapshot */
    float scale;
    char name[IVP_TRACE_SCHEMA_NAME_SIZE];
} ivp_trace_schema_frame_t;

typedef struct {
    uint8_t capture_id;
    uint32_t samples_captured;
    uint32_t samples_dropped;
    uint32_t frames_dropped;
} ivp_trace_status_frame_t;

typedef struct {
    uint8_t capture_id;
    uint8_t channel;
    bool snapshot;
    uint32_t sample_sequence;
    uint32_t event_sequence;
    float value;
} ivp_trace_event_frame_t;

ivp_trace_frame_type_t ivp_trace_frame_type(const uint8_t payload[IVP_TRACE_PAYLOAD_SIZE]);
bool ivp_trace_encode_data(const ivp_trace_data_frame_t* frame,
                           uint8_t payload[IVP_TRACE_PAYLOAD_SIZE]);
bool ivp_trace_decode_data(const uint8_t payload[IVP_TRACE_PAYLOAD_SIZE],
                           ivp_trace_data_frame_t* frame);
bool ivp_trace_encode_schema(const ivp_trace_schema_frame_t* frame,
                             uint8_t payload[IVP_TRACE_PAYLOAD_SIZE]);
bool ivp_trace_decode_schema(const uint8_t payload[IVP_TRACE_PAYLOAD_SIZE],
                             ivp_trace_schema_frame_t* frame);
bool ivp_trace_encode_status(const ivp_trace_status_frame_t* frame,
                             uint8_t payload[IVP_TRACE_PAYLOAD_SIZE]);
bool ivp_trace_decode_status(const uint8_t payload[IVP_TRACE_PAYLOAD_SIZE],
                             ivp_trace_status_frame_t* frame);
bool ivp_trace_encode_event(const ivp_trace_event_frame_t* frame,
                            uint8_t payload[IVP_TRACE_PAYLOAD_SIZE]);
bool ivp_trace_decode_event(const uint8_t payload[IVP_TRACE_PAYLOAD_SIZE],
                            ivp_trace_event_frame_t* frame);

#ifdef __cplusplus
}
#endif

#endif
