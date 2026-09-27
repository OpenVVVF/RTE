#include "safety_fault_wire.h"

static char hex_digit(uint8_t value)
{
    return "0123456789ABCDEF"[value & 15u];
}

static int8_t hex_value(char value)
{
    if (value >= '0' && value <= '9') return (int8_t)(value - '0');
    if (value >= 'A' && value <= 'F') return (int8_t)(value - 'A' + 10);
    return -1;
}

static uint8_t crc8(const char *data, size_t length)
{
    uint8_t crc = 0u;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint8_t)data[i];
        for (unsigned bit = 0u; bit < 8u; ++bit) {
            crc = (crc & 0x80u) != 0u
                ? (uint8_t)((crc << 1) ^ 0x07u) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

void SafetyFaultWire_Encode(const SafetyFaultFrame *frame,
                            char out[SAFETY_FAULT_WIRE_LENGTH])
{
    out[0] = '!'; out[1] = 'S'; out[2] = 'F'; out[3] = '1';
    for (unsigned i = 0u; i < 8u; ++i) {
        out[4u + i] = hex_digit((uint8_t)(frame->faults >> (28u - 4u * i)));
    }
    out[12] = hex_digit(frame->state);
    for (unsigned i = 0u; i < 4u; ++i) {
        out[13u + i] = hex_digit((uint8_t)(frame->generation >> (12u - 4u * i)));
    }
    out[17] = hex_digit(frame->clear_result);
    const uint8_t crc = crc8(out, 18u);
    out[18] = hex_digit((uint8_t)(crc >> 4));
    out[19] = hex_digit(crc);
    out[20] = '\n';
}

bool SafetyFaultWire_Decode(const char *body, size_t length,
                            SafetyFaultFrame *frame)
{
    if (body == NULL || frame == NULL || length != 20u ||
        body[0] != '!' || body[1] != 'S' ||
        body[2] != 'F' || body[3] != '1') {
        return false;
    }
    uint32_t faults = 0u;
    uint16_t generation = 0u;
    for (unsigned i = 0u; i < 8u; ++i) {
        const int8_t digit = hex_value(body[4u + i]);
        if (digit < 0) return false;
        faults = (faults << 4) | (uint8_t)digit;
    }
    const int8_t state = hex_value(body[12]);
    for (unsigned i = 0u; i < 4u; ++i) {
        const int8_t digit = hex_value(body[13u + i]);
        if (digit < 0) return false;
        generation = (uint16_t)((generation << 4) | (uint8_t)digit);
    }
    const int8_t result = hex_value(body[17]);
    const int8_t crc_high = hex_value(body[18]);
    const int8_t crc_low = hex_value(body[19]);
    if (state < 0 || state > 3 || result < 0 || result > 3 ||
        crc_high < 0 || crc_low < 0 ||
        (uint8_t)((crc_high << 4) | crc_low) != crc8(body, 18u)) {
        return false;
    }
    *frame = (SafetyFaultFrame){
        .faults = faults,
        .generation = generation,
        .state = (uint8_t)state,
        .clear_result = (uint8_t)result
    };
    return true;
}
