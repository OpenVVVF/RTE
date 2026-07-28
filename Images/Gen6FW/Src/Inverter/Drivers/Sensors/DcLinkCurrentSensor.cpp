#include "Inverter/Drivers/Sensors/DcLinkCurrentSensor.h"
#include "Inverter/Drivers/Sensors/ApplicationSensors.h"
#include "Inverter/Drivers/Sensors/DcLinkVoltageSensor.h"
#include "Inverter/Drivers/Sensors/PhaseCurrentADC.h"
#include "Inverter/Telemetry.h"

#include "main.h"

#include <cmath>

namespace Inverter {

namespace {

DcLinkCurrentSensor s_instance;

/* Same LA37S600 scaling as the phase-current sensors. */
constexpr uint32_t ADC_BITS       = 16;
constexpr float    ADC_VREF       = 3.3f;
constexpr float    DIVIDER        = 2.0f / 3.0f;
constexpr float    SENSITIVITY_VA = 1.042e-3f;

/* The DC-link bus current is chopped at the switching frequency; any single
 * sample lands at a random point on the chop.  The ~1020 Hz scan is not an
 * integer ratio of the 2.5 kHz PWM, so scan phase walks the PWM cycle and a
 * 40-sample moving average (~40 ms, 100 PWM periods) converges to the true
 * mean bus current. */
constexpr size_t   AVG_SAMPLES  = 40;
constexpr uint32_t ZERO_SAMPLES = 50U;  /* ~2 s on the averaged stream */

} // namespace

DcLinkCurrentSensor& DcLinkCurrentSensor::instance() {
    return s_instance;
}

DcLinkCurrentSensor& dcLinkCurrentSensor() {
    return DcLinkCurrentSensor::instance();
}

bool DcLinkCurrentSensor::init() {
    /* Zero offset is captured from the injected stream (PWM-synchronized
     * ranks 3/4 on ADC1) once it has been running long enough to settle. */
    m_zero_samples_left = ZERO_SAMPLES;
    m_zero_acc = 0.0;
    m_offset_valid = false;
    m_offset_a = 0.0f;
    m_energy_wh = 0.0f;
    m_last_energy_ms = HAL_GetTick();
    return true;
}

bool DcLinkCurrentSensor::zeroCalibrate() {
    return init();
}

float DcLinkCurrentSensor::countsToCurrent(uint32_t sig, uint32_t ref) const {
    const float lsb   = ADC_VREF / static_cast<float>((1U << ADC_BITS) - 1U);
    const float scale = lsb / (DIVIDER * SENSITIVITY_VA);
    return (static_cast<float>(sig) - static_cast<float>(ref)) * scale;
}

void DcLinkCurrentSensor::update() {
    /* Samples come from the ApplicationSensors TIM3-triggered ADC1 scan
     * (ranks 5/6, 100 Hz, DMA): signal and reference are converted
     * microseconds apart in the same scan, so the sensor supply bounce is
     * common-mode and cancels in the difference.  Nothing here ever blocks
     * on the ADC. */
    const uint32_t seq = appSensors().dcLinkSeq();
    if (seq == m_last_seq) {
        return;
    }
    m_last_seq = seq;

    m_raw_sig = appSensors().dcLinkSigCounts();
    m_raw_ref = appSensors().dcLinkRefCounts();

    /* Dead-channel guard: a live LA37S600 reference sits at mid-scale
     * (~32k counts).  Far below that the sensor is unpowered/unpopulated and
     * the differential is leakage, not current — report invalid instead of
     * garbage amps. */
    constexpr uint32_t REF_ALIVE_MIN = 20000U;
    if (m_raw_ref < REF_ALIVE_MIN) {
        if (m_offset_valid) {
            Telemetry::printf("[DCL] sensor reference lost (unpowered?)");
        }
        m_offset_valid = false;
        m_zero_samples_left = ZERO_SAMPLES;
        m_zero_acc = 0.0;
        m_avg_sum = 0.0;
        m_avg_head = 0;
        m_avg_count = 0;
        m_current_a = NAN;
        m_power_w = NAN;
        Telemetry::log("dclink_i_a", m_current_a);
        Telemetry::log("dclink_p_w", m_power_w);
        return;
    }

    /* Push this scan's differential into the moving average. */
    const float diff_a = countsToCurrent(m_raw_sig, m_raw_ref);
    if (m_avg_count < AVG_SAMPLES) {
        ++m_avg_count;
    } else {
        m_avg_sum -= m_avg_ring[m_avg_head];
    }
    m_avg_ring[m_avg_head] = diff_a;
    m_avg_sum += diff_a;
    m_avg_head = (m_avg_head + 1) % AVG_SAMPLES;
    const float mean_a = m_avg_sum / static_cast<float>(m_avg_count);

    if (m_zero_samples_left > 0) {
        m_zero_acc += mean_a;
        if (--m_zero_samples_left == 0) {
            m_offset_a = static_cast<float>(m_zero_acc /
                                            static_cast<double>(ZERO_SAMPLES));
            m_zero_acc = 0.0;
            m_offset_valid = true;
            Telemetry::printf("[DCL] zero offset %.3f A",
                              static_cast<double>(m_offset_a));
        }
        return;
    }

    m_current_a = mean_a - m_offset_a;
    m_power_w = m_current_a * dcLinkVoltageSensor().voltage();

    const uint32_t now_ms = HAL_GetTick();
    m_energy_wh += static_cast<float>(
        m_power_w * (static_cast<double>(now_ms - m_last_energy_ms) / 3600000.0));
    m_last_energy_ms = now_ms;

    Telemetry::log("dclink_i_a", m_current_a);
    Telemetry::log("dclink_p_w", m_power_w);
}

} // namespace Inverter

