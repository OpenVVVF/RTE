#include "Inverter/Drivers/Sensors/DcLinkCurrentSensor.h"
#include "Inverter/Drivers/Sensors/ApplicationSensors.h"
#include "Inverter/Drivers/Sensors/DcLinkVoltageSensor.h"
#include "Inverter/Drivers/Sensors/PhaseCurrentADC.h"
#include "Inverter/Control/FaultManager.h"
#include "Inverter/Drivers/Storage/RteParamStore.h"
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

float kvOr(const char* key, float def) {
    float v = def;
    if (RteParamStore::isReady()) {
        RteParamStore::get(key, &v);
    }
    return v;
}

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
     * (ranks 4/5, ~1 kHz, DMA): signal and reference are converted
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

    /* Reference plausibility: a healthy transducer reference sits inside a
     * known voltage window (KV Hw.DclCur.RefMinV/MaxV, defaults 2.0/3.0 V —
     * this transducer's ref is ~2.5 V).  Checked on every sample, forever:
     * out of window means the sensor is unpowered/unpopulated/failed, the
     * output goes NAN, and a sustained violation latches a fault. */
    constexpr float COUNTS_TO_V = 3.3f / 65535.0f;
    const float ref_v = static_cast<float>(m_raw_ref) * COUNTS_TO_V;
    const float win_lo = kvOr("Hw.DclCur.RefMinV", 2.0f);
    const float win_hi = kvOr("Hw.DclCur.RefMaxV", 3.0f);
    const bool plausible = (ref_v >= win_lo) && (ref_v <= win_hi);
    if (!plausible) {
        if (m_implausible_since_ms == 0) {
            m_implausible_since_ms = HAL_GetTick();
        } else if (!m_fault_raised &&
                   (HAL_GetTick() - m_implausible_since_ms) >= 500U) {
            m_fault_raised = true;
            FaultManager::instance().raise(FaultSource::CurrentSensorRef,
                                           FaultReason::SensorRefOutOfRange);
            Telemetry::printf("[DCL] sensor ref implausible: %.2f V (window %.1f..%.1f)",
                              static_cast<double>(ref_v),
                              static_cast<double>(win_lo),
                              static_cast<double>(win_hi));
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
    m_implausible_since_ms = 0;
    m_fault_raised = false;  /* re-arm: a cleared fault re-raises on relapse */

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

