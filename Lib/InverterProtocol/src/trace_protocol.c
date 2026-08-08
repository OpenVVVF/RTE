#include "inverter_protocol/trace_protocol.h"

#include <string.h>

static void put_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8U);
}

static uint16_t get_u16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8U));
}

static void put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8U);
    p[2] = (uint8_t)(v >> 16U);
    p[3] = (uint8_t)(v >> 24U);
}

static uint32_t get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) |
           ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static void put_f32(uint8_t* p, float value) {
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    put_u32(p, bits);
}

static float get_f32(const uint8_t* p) {
    const uint32_t bits = get_u32(p);
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static bool begin_encode(uint8_t type, uint8_t* payload) {
    if (payload == NULL) return false;
    memset(payload, 0, IVP_TRACE_PAYLOAD_SIZE);
    payload[0] = IVP_TRACE_MAGIC;
    payload[1] = IVP_TRACE_VERSION;
    payload[2] = type;
    return true;
}

static bool valid(const uint8_t* payload, uint8_t type) {
    return payload != NULL && payload[0] == IVP_TRACE_MAGIC &&
           payload[1] == IVP_TRACE_VERSION && payload[2] == type;
}

ivp_trace_frame_type_t ivp_trace_frame_type(const uint8_t payload[IVP_TRACE_PAYLOAD_SIZE]) {
    if (payload == NULL || payload[0] != IVP_TRACE_MAGIC ||
        payload[1] != IVP_TRACE_VERSION || payload[2] < IVP_TRACE_FRAME_DATA ||
        payload[2] > IVP_TRACE_FRAME_EVENT) {
        return (ivp_trace_frame_type_t)0;
    }
    return (ivp_trace_frame_type_t)payload[2];
}

bool ivp_trace_encode_data(const ivp_trace_data_frame_t* frame, uint8_t payload[IVP_TRACE_PAYLOAD_SIZE]) {
    if (frame == NULL || !begin_encode(IVP_TRACE_FRAME_DATA, payload)) return false;
    payload[3] = frame->capture_id;
    put_u32(payload + 4, frame->first_sample_sequence);
    put_u32(payload + 8, frame->first_sample_cycles);
    put_u16(payload + 12, frame->delta_cycles_div8[0]);
    put_u16(payload + 14, frame->delta_cycles_div8[1]);
    size_t offset = 16;
    for (size_t sample = 0; sample < IVP_TRACE_SAMPLES_PER_DATA_FRAME; ++sample) {
        for (size_t channel = 0; channel < IVP_TRACE_CHANNELS; ++channel) {
            put_u16(payload + offset, (uint16_t)frame->samples[sample][channel]);
            offset += 2;
        }
    }
    return true;
}

bool ivp_trace_decode_data(const uint8_t payload[IVP_TRACE_PAYLOAD_SIZE], ivp_trace_data_frame_t* frame) {
    if (frame == NULL || !valid(payload, IVP_TRACE_FRAME_DATA)) return false;
    frame->capture_id = payload[3];
    frame->first_sample_sequence = get_u32(payload + 4);
    frame->first_sample_cycles = get_u32(payload + 8);
    frame->delta_cycles_div8[0] = get_u16(payload + 12);
    frame->delta_cycles_div8[1] = get_u16(payload + 14);
    size_t offset = 16;
    for (size_t sample = 0; sample < IVP_TRACE_SAMPLES_PER_DATA_FRAME; ++sample) {
        for (size_t channel = 0; channel < IVP_TRACE_CHANNELS; ++channel) {
            frame->samples[sample][channel] = (int16_t)get_u16(payload + offset);
            offset += 2;
        }
    }
    return true;
}

bool ivp_trace_encode_schema(const ivp_trace_schema_frame_t* frame, uint8_t payload[IVP_TRACE_PAYLOAD_SIZE]) {
    if (frame == NULL || frame->channel >= IVP_TRACE_MAX_CHANNELS ||
        !begin_encode(IVP_TRACE_FRAME_SCHEMA, payload)) return false;
    payload[3] = frame->capture_id;
    payload[4] = frame->channel;
    put_f32(payload + 8, frame->scale);
    memcpy(payload + 12, frame->name, IVP_TRACE_SCHEMA_NAME_SIZE);
    return true;
}

bool ivp_trace_decode_schema(const uint8_t payload[IVP_TRACE_PAYLOAD_SIZE], ivp_trace_schema_frame_t* frame) {
    if (frame == NULL || !valid(payload, IVP_TRACE_FRAME_SCHEMA) ||
        payload[4] >= IVP_TRACE_MAX_CHANNELS) return false;
    frame->capture_id = payload[3];
    frame->channel = payload[4];
    frame->scale = get_f32(payload + 8);
    memcpy(frame->name, payload + 12, IVP_TRACE_SCHEMA_NAME_SIZE);
    frame->name[IVP_TRACE_SCHEMA_NAME_SIZE - 1] = '\0';
    return true;
}

bool ivp_trace_encode_status(const ivp_trace_status_frame_t* frame, uint8_t payload[IVP_TRACE_PAYLOAD_SIZE]) {
    if (frame == NULL || !begin_encode(IVP_TRACE_FRAME_STATUS, payload)) return false;
    payload[3] = frame->capture_id;
    put_u32(payload + 4, frame->samples_captured);
    put_u32(payload + 8, frame->samples_dropped);
    put_u32(payload + 12, frame->frames_dropped);
    return true;
}

bool ivp_trace_decode_status(const uint8_t payload[IVP_TRACE_PAYLOAD_SIZE], ivp_trace_status_frame_t* frame) {
    if (frame == NULL || !valid(payload, IVP_TRACE_FRAME_STATUS)) return false;
    frame->capture_id = payload[3];
    frame->samples_captured = get_u32(payload + 4);
    frame->samples_dropped = get_u32(payload + 8);
    frame->frames_dropped = get_u32(payload + 12);
    return true;
}

bool ivp_trace_encode_event(const ivp_trace_event_frame_t* frame, uint8_t payload[IVP_TRACE_PAYLOAD_SIZE]) {
    if (frame == NULL || frame->channel >= IVP_TRACE_MAX_CHANNELS ||
        !begin_encode(IVP_TRACE_FRAME_EVENT, payload)) return false;
    payload[3] = frame->capture_id;
    payload[4] = frame->channel;
    payload[5] = frame->snapshot ? 1U : 0U;
    put_u32(payload + 8, frame->sample_sequence);
    put_u32(payload + 12, frame->event_sequence);
    put_f32(payload + 16, frame->value);
    return true;
}

bool ivp_trace_decode_event(const uint8_t payload[IVP_TRACE_PAYLOAD_SIZE], ivp_trace_event_frame_t* frame) {
    if (frame == NULL || !valid(payload, IVP_TRACE_FRAME_EVENT) ||
        payload[4] >= IVP_TRACE_MAX_CHANNELS) return false;
    frame->capture_id = payload[3];
    frame->channel = payload[4];
    frame->snapshot = payload[5] != 0U;
    frame->sample_sequence = get_u32(payload + 8);
    frame->event_sequence = get_u32(payload + 12);
    frame->value = get_f32(payload + 16);
    return true;
}
