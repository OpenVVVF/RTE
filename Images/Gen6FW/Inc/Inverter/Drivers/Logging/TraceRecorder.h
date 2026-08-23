#pragma once

#include <cstdint>

namespace Inverter {

/**
 * Supplemental high-rate recorder.  capture8() is the only ISR-facing path:
 * one enable branch, fixed copies, one DWT read, and SPSC index publication.
 * Encoding, quantization, CAN access, diagnostics, and strings stay in the
 * foreground update().
 */
class TraceRecorder {
public:
    static constexpr uint8_t CHANNELS = 8;
    static constexpr uint8_t MAX_CHANNELS = 32;
    static constexpr uint16_t SAMPLE_RING = 256;

    struct Sample {
        uint32_t sequence;
        uint32_t cycles;
        float values[CHANNELS];
    };

    void init();
    void update();
    void start();
    void stop();
    bool running() const { return m_running; }

    void configure8(const char* const names[CHANNELS], const float scales[CHANNELS]);
    bool registerEventChannel(uint8_t channel, const char* name);
    void capture8(float value0, float value1, float value2, float value3,
                  float value4, float value5, float value6, float value7);

    /** Sparse channels are sent only when their applied value changes. */
    bool publishEvent(uint8_t channel, float value, bool snapshot = false);
    void printStatus() const;

private:
    void resetCapture();
    bool sendSchemas();
    bool sendStatus();
    static int16_t quantize(float value, float scale);

    bool m_enabled = false;
    volatile bool m_running = false;
    uint8_t m_bus = 0;
    uint16_t m_id_base = 0x680;
    uint8_t m_capture_id = 0;
    uint8_t m_schema_next = 0;
    char m_names[MAX_CHANNELS][32] = {};
    bool m_registered[MAX_CHANNELS] = {};
    float m_scales[CHANNELS] = {};

    volatile uint16_t m_head = 0;
    volatile uint16_t m_tail = 0;
    volatile uint32_t m_next_sample_sequence = 0;
    volatile uint32_t m_samples_captured = 0;
    volatile uint32_t m_samples_dropped = 0;
    uint32_t m_block_sequence = 0;
    uint32_t m_event_sequence = 0;
    uint32_t m_frames_dropped = 0;
    uint32_t m_last_status_ms = 0;

    friend TraceRecorder& traceRecorder();
};

TraceRecorder& traceRecorder();

} // namespace Inverter
