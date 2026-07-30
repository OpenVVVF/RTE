#pragma once

#include <cstddef>
#include <cstdint>

namespace Inverter {

/**
 * @brief Spike event recorder for current-sense / encoder glitch hunting.
 *
 * A rolling ring of synchronized samples captured at the injected-ADC rate
 * (5 kHz): raw current ADC quads, computed phase currents, and raw encoder
 * sin/cos + decoded angle.  When |iu| or |iv| exceeds the threshold the
 * recorder keeps POST_TRIGGER more samples, then freezes for inspection via
 * the `spikes` shell command.  Pre-trigger history shows which signal moved
 * first: the current ADCs (measurement glitch) or the encoder angle
 * (commutation error -> real current spike).
 *
 * Runs entirely in ISR context; dump happens from the shell (main loop).
 */
class SpikeRecorder {
public:
    static constexpr size_t RING = 64;
    static constexpr size_t POST_TRIGGER = 16;

    struct Sample {
        uint32_t tick_ms;
        uint16_t raw_u_sig;
        uint16_t raw_v_sig;
        uint16_t raw_u_ref;
        uint16_t raw_v_ref;
        uint16_t enc_sin;
        uint16_t enc_cos;
        float    iu;
        float    iv;
        float    enc_angle;
        float    duty_u;  /**< commanded duties [%], written by the tim_isr domain */
        float    duty_v;
        float    duty_w;
    };

    /** @brief Push one synchronized sample; call from the injected ADC ISR. */
    void onSample(uint32_t tick_ms,
                  uint16_t raw_u_sig, uint16_t raw_v_sig,
                  uint16_t raw_u_ref, uint16_t raw_v_ref,
                  float iu, float iv,
                  float enc_angle, uint16_t enc_sin, uint16_t enc_cos,
                  float duty_u, float duty_v, float duty_w);

    void  setThreshold(float amps);
    float threshold() const { return m_threshold; }
    uint32_t triggerCount() const { return m_trigger_count; }

    /** @brief true when a frozen capture is waiting to be dumped. */
    bool ready() const { return m_ready; }

    /** @brief Print the frozen capture via Telemetry and re-arm. */
    void dump();

private:
    Sample   m_ring[RING] = {};
    size_t   m_head = 0;            /**< next write index */
    float    m_threshold = 50.0f;
    uint32_t m_trigger_count = 0;
    uint32_t m_holdoff_until_ms = 0;
    int      m_post_remaining = -1; /**< <0 = armed (pre-trigger) */
    size_t   m_trigger_idx = 0;
    bool     m_ready = false;
};

/** @brief Global spike recorder instance. */
SpikeRecorder& spikeRecorder();

} // namespace Inverter
