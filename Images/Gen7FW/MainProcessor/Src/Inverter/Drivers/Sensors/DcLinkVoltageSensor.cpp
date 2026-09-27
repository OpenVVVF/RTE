#include "Inverter/Drivers/Sensors/DcLinkVoltageSensor.h"

#include "Inverter/Control/FaultManager.h"
#include "Inverter/Drivers/Storage/RteParamStore.h"
#include "Inverter/Telemetry.h"
#include "main.h"

#include <cmath>

namespace Inverter {

namespace {

/* Default scaled-voltage offset observed when the high-voltage input is at
 * 0 V.  This is converted to raw ADC volts using the configured scale. */
constexpr float DEFAULT_VZERO_OFFSET_SCALED_V = 0;

/* Global hardware instance.  Construction only stores pointers; HAL calls
 * happen later in init(). */
MAX22530 s_adc(
    &hspi2,
    SPI2_CS_GPIO_Port, SPI2_CS_Pin,
    VSENSE_ISO_ADC_INTERRUPT_GPIO_Port, VSENSE_ISO_ADC_INTERRUPT_Pin,
    EXTI1_IRQn);

DcLinkVoltageSensor s_vdc(s_adc);

} // namespace

DcLinkVoltageSensor& dcLinkVoltageSensor() {
    return s_vdc;
}

DcLinkVoltageSensor::DcLinkVoltageSensor(MAX22530& adc,
                                         const char* telemetry_key,
                                         float scale)
    : m_adc(adc),
      m_key(telemetry_key ? telemetry_key : "vdc_v"),
      m_scale(scale),
      m_zero_offset_v(DEFAULT_VZERO_OFFSET_SCALED_V / scale),
      m_voltage(0.0f),
      m_ov_threshold_v(190.0f),         /* 200 V-class OV trip */
      m_uv_threshold_v(10.0f),          /* 200 V-class UV trip */
      m_initialized(false),
      m_has_sample(false) {
}

bool DcLinkVoltageSensor::init() {
    m_initialized = m_adc.init();
    if (m_initialized) {
        (void)applyComparatorThresholds();
    }
    return m_initialized;
}

bool DcLinkVoltageSensor::setOvervoltageThreshold(float v) {
    /* A 200 V capacitor bank must not lose its 190 V software comparator
     * limit through a shell command. Lower limits remain available. */
    if (!std::isfinite(v) || v <= 0.0f || v > 190.0f) return false;
    m_ov_threshold_v = v;
    return applyComparatorThresholds();
}

bool DcLinkVoltageSensor::setUndervoltageThreshold(float v) {
    m_uv_threshold_v = v;
    return applyComparatorThresholds();
}

bool DcLinkVoltageSensor::applyComparatorThresholds() {
    /* Convert scaled high-side volts to raw MAX22530 input volts. */
    const float raw_ov = m_ov_threshold_v / m_scale;
    const float raw_uv = m_uv_threshold_v / m_scale;

    /* Only enable a comparator direction if its threshold is inside the ADC
     * range. A 0 or out-of-range UV threshold disables that direction; the
     * 190 V OV default is active on the Gen7 board's 1001:1 divider. */
    constexpr float VREF = 1.8f;
    const bool enable_ov = (raw_ov > 0.0f && raw_ov < VREF);
    const bool enable_uv = (raw_uv > 0.0f && raw_uv < VREF);

    /* Digital-status mode with filtered input.  On the Gen7 control board
     * AIN4 (channel index 3) is VSENSE_DC_LINK_B. */
    return m_adc.setComparatorThreshold(3, raw_ov, raw_uv, true, true,
                                        enable_ov, enable_uv);
}

/* TIME_DOMAIN: APPLICATION_SENSOR_POLL_100HZ
 *   Main-loop poll of the isolated DC-link voltage sensor.
 * CODEGEN: Add similar update() functions for codegen application sensors
 *   (temperature, throttle, auxiliary voltages, etc.).
 */
void DcLinkVoltageSensor::update() {
    /* A true dataReady() means the ISR has collected a new ADC sample since
     * the last loop iteration. */
    const bool had_new = m_adc.dataReady();
    m_adc.update();

    if (had_new) {
        m_has_sample = true;
    }

    if (m_has_sample) {
        /* voltage(3) reads AIN4 = VSENSE_DC_LINK_B (see ADC2.kicad_sch). */
        const float raw_v = m_adc.voltage(3);
        m_voltage = (raw_v - m_zero_offset_v) * m_scale;
    }

    Telemetry::log(m_key, m_voltage);

    evaluateProtection();
}

void DcLinkVoltageSensor::evaluateProtection() {
    /* SG-10 DC-link protection, evaluated on the polled (filtered) bus
     * voltage at main-loop rate; the fast MAX22530 comparator path handles
     * the critical OV/UV trips independently.
     *
     * OV warning (FSR-11): above Hw.DcLink.OvWarnV, regen is disabled at the
     * command path while the condition persists (hysteresis on release).
     * UV derate (FSR-21 candidate): below Hw.DcLink.UvDerateV, the current
     * ceiling drops to Hw.DcLink.UvDerateMaxA while the condition persists.
     * Warning-severity faults latch until fault clear; the clamps
     * auto-recover.  Either threshold <= 0 disables that feature.
     *
     * Suppressed while any Critical fault is active: the safety sequence
     * dumps machine energy into the bus and a UV (or second OV) warning
     * after a critical trip would only muddy the flag word. */
    constexpr float OV_WARN_HYST_V   = 2.0f;
    constexpr float UV_DERATE_HYST_V = 2.0f;

    float ov_warn_v = 55.0f;
    float uv_derate_v = 40.0f;
    if (RteParamStore::isReady()) {
        (void)RteParamStore::get("Hw.DcLink.OvWarnV", &ov_warn_v);
        (void)RteParamStore::get("Hw.DcLink.UvDerateV", &uv_derate_v);
        (void)RteParamStore::get("Hw.DcLink.UvDerateMaxA", &m_uv_derate_max_a);
    }

    if (!m_has_sample ||
        FaultManager::instance().isSeverityActive(FaultSeverity::Critical)) {
        return;
    }

    if (!m_regen_disabled && ov_warn_v > 0.0f && m_voltage > ov_warn_v) {
        m_regen_disabled = true;
        FaultManager::instance().raise(FaultSource::DcLinkOvWarning,
                                       FaultReason::DcLinkOvervoltageWarning);
    } else if (m_regen_disabled && (ov_warn_v <= 0.0f ||
                                    m_voltage < ov_warn_v - OV_WARN_HYST_V)) {
        m_regen_disabled = false;
    }

    if (!m_derate_active && uv_derate_v > 0.0f && m_voltage < uv_derate_v) {
        m_derate_active = true;
        FaultManager::instance().raise(FaultSource::DcLinkUvDerate,
                                       FaultReason::DcLinkUndervoltageDerate);
    } else if (m_derate_active && (uv_derate_v <= 0.0f ||
                                   m_voltage > uv_derate_v + UV_DERATE_HYST_V)) {
        m_derate_active = false;
    }
}

bool DcLinkVoltageSensor::zeroCalibrate() {
    /* Make sure we have the freshest sample possible. */
    m_adc.update();

    if (!m_has_sample) {
        return false;
    }

    m_zero_offset_v = m_adc.voltage(3);
    return true;
}

} // namespace Inverter
