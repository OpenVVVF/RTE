#pragma once

#include <cstddef>
#include <cstdint>

namespace Inverter {

/**
 * @brief DC-link current sensor (LA37S600 on ADC1_INP2 sig / ADC1_INP6 ref).
 *
 * The DC-link current uses the same differential signal + reference scheme
 * as the phase-current sensors and is sampled by the ApplicationSensors
 * TIM3-triggered ADC1 regular scan (ranks 5/6, 100 Hz, circular DMA).
 * Signal and reference are converted microseconds apart in the same scan,
 * so the sensor supply bounce is common-mode and cancels in the
 * difference — and the injected phase-current path is never touched
 * (an earlier injected-rank attempt degraded it; polled regular
 * conversions aliased the supply ripple).
 *
 * Also integrates input power into cumulative energy [Wh].
 */
class DcLinkCurrentSensor {
public:
    bool init();

    /** Main-loop poll: consumes the latest injected sample, finishes the
     *  zero-offset capture window, then publishes telemetry. */
    void update();

    /** Restart the zero-offset capture and reset the energy counter.
     *  Only meaningful with the drive idle (no switching). */
    bool zeroCalibrate();

    float current() const { return m_current_a; }
    float power() const { return m_power_w; }
    float energyWh() const { return m_energy_wh; }
    bool offsetValid() const { return m_offset_valid; }

    uint32_t lastRawSig() const { return m_raw_sig; }
    uint32_t lastRawRef() const { return m_raw_ref; }
    float    lastOffset() const { return m_offset_a; }

    static DcLinkCurrentSensor& instance();

private:
    float countsToCurrent(uint32_t sig, uint32_t ref) const;

    uint32_t m_last_energy_ms = 0;
    uint32_t m_last_seq = 0;
    uint32_t m_zero_samples_left = 0;
    double   m_zero_acc = 0.0;

    /* Moving-average state (see AVG_SAMPLES above). */
    float    m_avg_ring[40] = {};
    double   m_avg_sum = 0.0;
    size_t   m_avg_head = 0;
    size_t   m_avg_count = 0;

    uint32_t m_raw_sig = 0;
    uint32_t m_raw_ref = 0;
    float m_offset_a = 0.0f;
    bool  m_offset_valid = false;

    float m_current_a = 0.0f;
    float m_power_w = 0.0f;
    float m_energy_wh = 0.0f;
};

DcLinkCurrentSensor& dcLinkCurrentSensor();

} // namespace Inverter
