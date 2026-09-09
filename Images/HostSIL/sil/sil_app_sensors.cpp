/*
 * sil_app_sensors.cpp — SIL replacement for
 * Src/Inverter/Drivers/Sensors/ApplicationSensors.cpp.
 *
 * Slow application analog samplers without ADC hardware: temperatures are
 * disabled/NaN (channels are TYPE_DISABLED with an empty KV store), the
 * throttle pins read the scenario profile through silWorld, and the DC-link
 * current pair (ADC1 ranks 5/6 in hardware) reads the plant power-balance
 * estimate.  Normalization keeps the hardware convention: normalized =
 * (V - MinV) / (MaxV - MinV), plausible while |A-B| <= 0.10.
 */
#include "Inverter/Drivers/Sensors/ApplicationSensors.h"
#include "Inverter/Drivers/Storage/RteParamStore.h"
#include "Inverter/Telemetry.h"

#include "sil_world.h"

#include <cmath>

namespace Inverter {

static ApplicationSensors s_instance;

ApplicationSensors& appSensors() {
    return s_instance;
}

namespace {
/* Differential LA37S600 counts per amp (same physical chain as the
 * phase-current shim). */
constexpr float kCountsPerAmp = ((2.0f / 3.0f) * 1.042e-3f * 65535.0f) / 3.3f;
constexpr float kRefMidCounts = 65535.0f * 0.5f;
/* DC-link transducer reference rail: ~2.5 V (Hw.DclCur.RefMin/MaxV window
 * 2.0..3.0 V in DcLinkCurrentSensor). */
constexpr float kDclRefCounts = (2.5f / 3.3f) * 65535.0f;
} // namespace

bool ApplicationSensors::init() {
    /* Loading KV conversion config: with an empty store every temperature
     * channel is TYPE_DISABLED and throttle bounds use the header defaults
     * (0.5..4.5 V).  Nothing else to start in SIL. */
    m_initialized = true;
    return true;
}

void ApplicationSensors::reloadConfig() {}

void ApplicationSensors::update() {
    if (!m_initialized) return;

    /* Throttle: pin voltages from the scenario, normalized [0..1]. */
    m_thr_a_v = silWorld().throttle_a_v;
    m_thr_b_v = silWorld().throttle_b_v;
    const float span_a = m_thr_max_v[0] - m_thr_min_v[0];
    const float span_b = m_thr_max_v[1] - m_thr_min_v[1];
    m_thr_a_cand = (span_a > 1e-6f)
        ? (m_thr_a_v - m_thr_min_v[0]) / span_a : 0.0f;
    m_thr_b_cand = (span_b > 1e-6f)
        ? (m_thr_b_v - m_thr_min_v[1]) / span_b : 0.0f;
    if (m_thr_a_cand < 0.0f) m_thr_a_cand = 0.0f;
    if (m_thr_a_cand > 1.0f) m_thr_a_cand = 1.0f;
    if (m_thr_b_cand < 0.0f) m_thr_b_cand = 0.0f;
    if (m_thr_b_cand > 1.0f) m_thr_b_cand = 1.0f;

    m_thr_plausible =
        std::fabs(m_thr_a_cand - m_thr_b_cand) <= THROTTLE_PLAUS_TOL;
    m_thr_a_norm = m_thr_plausible ? m_thr_a_cand : 0.0f;
    m_thr_b_norm = m_thr_plausible ? m_thr_b_cand : 0.0f;

    /* DC-link current pair (power-balance estimate fed by the scheduler).
     * This transducer's reference is ~2.5 V (KV window 2.0..3.0 V), NOT the
     * phase-current 1.65 V — getting this wrong latches CurrentSensorRef
     * after the 500 ms sustain timer. */
    const float i_dc = silWorld().dc_link_current_a;
    float sig = kDclRefCounts + i_dc * kCountsPerAmp;
    if (sig < 0.0f) sig = 0.0f;
    if (sig > 65535.0f) sig = 65535.0f;
    m_dclink_sig = static_cast<uint16_t>(sig);
    m_dclink_ref = static_cast<uint16_t>(kDclRefCounts);
    ++m_dclink_seq;
}

float ApplicationSensors::motorTemperatureC() const {
    return NAN;   /* channel disabled in SIL */
}

float ApplicationSensors::inverterTemperatureC(uint8_t channel) const {
    (void)channel;
    return NAN;   /* channels disabled in SIL */
}

float ApplicationSensors::throttleAVoltage() const { return m_thr_a_v; }
float ApplicationSensors::throttleBVoltage() const { return m_thr_b_v; }

float ApplicationSensors::throttleA() const { return m_thr_a_norm; }
float ApplicationSensors::throttleB() const { return m_thr_b_norm; }

bool ApplicationSensors::throttlePlausible() const { return m_thr_plausible; }

uint16_t ApplicationSensors::dcLinkSigCounts() const { return m_dclink_sig; }
uint16_t ApplicationSensors::dcLinkRefCounts() const { return m_dclink_ref; }
uint32_t ApplicationSensors::dcLinkSeq() const { return m_dclink_seq; }

void ApplicationSensors::channelStatus(uint8_t ch, bool& enabled, uint8_t& type,
                                       float& volts, float& ohms, float& tempC,
                                       bool& outOfRange) const {
    (void)ch;
    enabled     = false;
    type        = TYPE_DISABLED;
    volts       = 0.0f;
    ohms        = NAN;
    tempC       = NAN;
    outOfRange  = false;
}

void ApplicationSensors::debugStatus() const {
    Telemetry::printf("[SHELL] appSensors (SIL): temps disabled, throttle %.2f/%.2f V",
                      static_cast<double>(m_thr_a_v),
                      static_cast<double>(m_thr_b_v));
}

const char* ApplicationSensors::typeName(uint8_t type) {
    switch (type) {
        case 0: return "disabled";
        case 1: return "NTC-beta";
        case 2: return "PTC-beta";
        case 3: return "KTY84";
        case 4: return "linear-RTD";
        case 5: return "PT1000";
        case 6: return "PT100";
        case 7: return "KTY83-110";
        default: return "?";
    }
}

} // namespace Inverter
