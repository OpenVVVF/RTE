/*
 * sil_app_sensors.cpp — SIL replacement for
 * Src/Inverter/Drivers/Sensors/ApplicationSensors.cpp.
 *
 * Slow application analog samplers without ADC hardware: the throttle pins
 * read the scenario profile through silWorld and the DC-link current pair
 * (ADC1 ranks 5/6 in hardware) reads the plant power-balance estimate.
 * Normalization keeps the hardware convention: normalized = (V - MinV) /
 * (MaxV - MinV), plausible while |A-B| <= 0.10.
 *
 * Temperature channels are driven by the scenario through silWorld().temp_c[]
 * (NaN = channel not modeled, the default).  A driven channel runs the full
 * sensor chain — temperature -> sensor resistance -> divider voltage
 * (parametrized by the same KV config the hardware driver uses), then the
 * firmware's own out-of-range and over-temperature evaluation (ported
 * verbatim below, same FaultManager raise paths:
 * TempSensor / OvertemperatureMotor / OvertemperatureInverter).
 */
#include "Inverter/Drivers/Sensors/ApplicationSensors.h"
#include "Inverter/Control/FaultManager.h"
#include "Inverter/Drivers/Storage/RteParamStore.h"
#include "Inverter/Telemetry.h"

#include "main.h"
#include "sil_world.h"

#include <cmath>
#include <cstdio>

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

/* KV namespace per temperature channel (same as the hardware driver). */
constexpr const char* KV_PREFIX[ApplicationSensors::NUM_CHANNELS] = {
    "Hw.Temp.B1", "Hw.Temp.B2", "Hw.Temp.B3", "Motor.Temp",
};
} // namespace

/* Verbatim port of the hardware driver's KV config load (same keys, same
 * defaults); replaced only where it touches the ADC hardware. */
void ApplicationSensors::loadConfig(bool persist_defaults) {
    struct DefaultCfg {
        float en, type, r25, beta, rser, orient, crit;
    };
    /* Board temp sensors are not populated on current hardware: default to
     * disabled so a floating pin can never trip the over-temperature fault.
     * Enable per channel via `config set Hw.Temp.Bx.En 1` once stuffed. */
    static constexpr DefaultCfg BOARD_DEF = {0.0f, 1.0f, 10000.0f, 3950.0f,
                                             10000.0f, 0.0f, 90.0f};
    static constexpr DefaultCfg MOTOR_DEF = {1.0f, 3.0f, 603.0f, 0.0f,
                                             10000.0f, 0.0f, 150.0f};

    auto loadOne = [persist_defaults](const char* key, float def) -> float {
        float value = def;
        if (!RteParamStore::isReady()) {
            return def;
        }
        if (!RteParamStore::get(key, &value) && persist_defaults) {
            RteParamStore::set(key, def);
            value = def;
        }
        return value;
    };

    char key[40];
    for (uint8_t i = 0; i < NUM_CHANNELS; ++i) {
        const DefaultCfg& d = (i == 3) ? MOTOR_DEF : BOARD_DEF;
        Config& c = m_ch[i].cfg;

        std::snprintf(key, sizeof(key), "%s.En", KV_PREFIX[i]);
        c.enabled = loadOne(key, d.en) != 0.0f;
        std::snprintf(key, sizeof(key), "%s.Type", KV_PREFIX[i]);
        c.type = static_cast<uint8_t>(loadOne(key, d.type));
        std::snprintf(key, sizeof(key), "%s.R25", KV_PREFIX[i]);
        c.r25 = loadOne(key, d.r25);
        std::snprintf(key, sizeof(key), "%s.Beta", KV_PREFIX[i]);
        c.beta = loadOne(key, d.beta);
        std::snprintf(key, sizeof(key), "%s.RSer", KV_PREFIX[i]);
        c.rser = loadOne(key, d.rser);
        std::snprintf(key, sizeof(key), "%s.Orient", KV_PREFIX[i]);
        c.orient = static_cast<uint8_t>(loadOne(key, d.orient));
        std::snprintf(key, sizeof(key), "%s.CritC", KV_PREFIX[i]);
        c.crit_c = loadOne(key, d.crit);
        std::snprintf(key, sizeof(key), "%s.Vexc", KV_PREFIX[i]);
        c.vexc = loadOne(key, 0.0f);
        std::snprintf(key, sizeof(key), "%s.Gain", KV_PREFIX[i]);
        c.gain = loadOne(key, 1.0f);
    }

    m_vcc = loadOne("Hw.Temp.Vcc", 3.3f);
    m_thr_min_v[0] = loadOne("Hw.ThrA.MinV", 0.5f);
    m_thr_max_v[0] = loadOne("Hw.ThrA.MaxV", 4.5f);
    m_thr_min_v[1] = loadOne("Hw.ThrB.MinV", 0.5f);
    m_thr_max_v[1] = loadOne("Hw.ThrB.MaxV", 4.5f);

    if (persist_defaults && RteParamStore::isReady()) {
        RteParamStore::flush();
    }
}

bool ApplicationSensors::init() {
    for (uint8_t i = 0; i < NUM_CHANNELS; ++i) {
        m_ch[i].temp_c = NAN;
        m_ch[i].voltage = NAN;
        m_ch[i].resistance = NAN;
    }

    /* Loading KV conversion config: with an empty store the board channels
     * are TYPE_DISABLED and the motor channel defaults to KTY84/150 degC —
     * the same defaults as the hardware driver.  Channels still report NAN
     * until the scenario drives a temperature for them. */
    loadConfig(/*persist_defaults=*/true);
    m_initialized = true;
    m_last_window_ms = HAL_GetTick();
    return true;
}

void ApplicationSensors::reloadConfig() {
    loadConfig(/*persist_defaults=*/false);
}

/* --- Temperature-fault support ------------------------------------------------
 * updateOutOfRange / updateOverTemp are ports of the hardware driver's
 * evaluation (Src/Inverter/Drivers/Sensors/ApplicationSensors.cpp) with the
 * same sustain windows, hysteresis, and FaultManager raise paths; the SIL
 * feeds them synthesized channel voltages instead of ADC reads. */

void ApplicationSensors::updateOutOfRange(uint8_t ch, uint32_t now_ms) {
    Channel& c = m_ch[ch];
    const bool oor = (c.voltage > OOR_OPEN_RATIO * m_vcc) ||
                     (c.voltage < OOR_SHORT_RATIO * m_vcc);

    if (!oor) {
        /* Back in range: clear the condition; the latched fault stays until
         * the user clears it via the shell. */
        c.oor_pending = false;
        c.out_of_range = false;
        return;
    }

    if (!c.oor_pending) {
        c.oor_pending = true;
        c.oor_since_ms = now_ms;
        return;
    }

    if ((now_ms - c.oor_since_ms) < FAULT_SUSTAIN_MS) {
        return;
    }

    if (!c.out_of_range) {
        c.out_of_range = true;
        static const FaultReason OPEN_REASONS[NUM_CHANNELS] = {
            FaultReason::TempSensorOpenInv1, FaultReason::TempSensorOpenInv2,
            FaultReason::TempSensorOpenInv3, FaultReason::TempSensorOpenMot,
        };
        static const FaultReason SHORT_REASONS[NUM_CHANNELS] = {
            FaultReason::TempSensorShortInv1, FaultReason::TempSensorShortInv2,
            FaultReason::TempSensorShortInv3, FaultReason::TempSensorShortMot,
        };
        const bool open = (c.voltage > OOR_OPEN_RATIO * m_vcc);
        FaultManager::instance().raise(FaultSource::TempSensor,
                                       open ? OPEN_REASONS[ch] : SHORT_REASONS[ch]);
    }
}

void ApplicationSensors::updateOverTemp(uint8_t ch, uint32_t now_ms) {
    Channel& c = m_ch[ch];
    const Config& cfg = c.cfg;

    if (!cfg.enabled || c.out_of_range || !std::isfinite(c.temp_c)) {
        c.over_temp_cond = false;
        c.over_temp_raised = false;
        return;
    }

    /* 5 degC hysteresis on the condition. */
    if (c.over_temp_cond) {
        c.over_temp_cond = (c.temp_c > (cfg.crit_c - OVERTEMP_HYST_C));
    } else {
        c.over_temp_cond = (c.temp_c > cfg.crit_c);
        if (c.over_temp_cond) {
            c.ot_since_ms = now_ms;
        }
    }

    if (!c.over_temp_cond) {
        c.over_temp_raised = false;
        return;
    }

    if ((now_ms - c.ot_since_ms) < FAULT_SUSTAIN_MS) {
        return;
    }

    if (!c.over_temp_raised) {
        c.over_temp_raised = true;
        if (ch == 3) {
            FaultManager::instance().raise(FaultSource::OvertemperatureMotor,
                                           FaultReason::OvertemperatureMotor);
            Telemetry::printf("[TMP] motor over-temperature: %.1f C (limit %.0f C)",
                              static_cast<double>(c.temp_c),
                              static_cast<double>(cfg.crit_c));
        } else {
            static const FaultReason OT_REASONS[BOARD_CHANNELS] = {
                FaultReason::OvertemperatureInv1, FaultReason::OvertemperatureInv2,
                FaultReason::OvertemperatureInv3,
            };
            FaultManager::instance().raise(FaultSource::OvertemperatureInverter,
                                           OT_REASONS[ch]);
            Telemetry::printf("[TMP] inverter ch%u over-temperature: %.1f C (limit %.0f C)",
                              static_cast<unsigned>(ch + 1),
                              static_cast<double>(c.temp_c),
                              static_cast<double>(cfg.crit_c));
        }
    }
}

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

    /* Temperature channels: driven by the scenario through silWorld().temp_c
     * (NaN = channel not modeled).  A driven channel is converted back
     * through the sensor chain (temperature -> resistance -> divider voltage,
     * the exact inverse of the firmware conversion) at the hardware driver's
     * WINDOW_MS cadence, then the ported out-of-range/over-temperature
     * evaluation runs on the synthesized voltage. */
    const uint32_t now_ms = HAL_GetTick();
    if ((now_ms - m_last_window_ms) < WINDOW_MS) return;
    m_last_window_ms = now_ms;

    /* KTY84 quadratic coefficients: R = R25 * (1 + A*dT + B*dT^2) — the same
     * constants the hardware driver inverts in resistanceToTempC. */
    constexpr float KTY84_A = 7.418e-3f;
    constexpr float KTY84_B = 1.815e-5f;

    auto tempToResistance = [](const Config& cfg, float t) -> float {
        switch (cfg.type) {
            case TYPE_NTC_BETA:
            case TYPE_PTC_BETA:
                if (cfg.r25 <= 0.0f || cfg.beta == 0.0f) return NAN;
                return cfg.r25 * std::exp(cfg.beta * (1.0f / (t + 273.15f) -
                                                      1.0f / 298.15f));
            case TYPE_KTY84: {
                if (cfg.r25 <= 0.0f) return NAN;
                const float dt = t - 25.0f;
                return cfg.r25 * (1.0f + KTY84_A * dt + KTY84_B * dt * dt);
            }
            case TYPE_KTY83_110: {
                const float dt = t - 25.0f;
                return 1000.0f * (1.0f + KTY84_A * dt + KTY84_B * dt * dt);
            }
            case TYPE_LINEAR_RTD:
                if (cfg.r25 <= 0.0f || cfg.beta == 0.0f) return NAN;
                return cfg.r25 * (1.0f + cfg.beta * t);
            case TYPE_PT1000: return 1000.0f * (1.0f + 0.00385f * t);
            case TYPE_PT100:  return 100.0f * (1.0f + 0.00385f * t);
            default: return NAN;
        }
    };
    auto dividerVoltage = [this](const Config& cfg, float r) -> float {
        if (!std::isfinite(r)) return NAN;
        if (cfg.orient == 0) {
            /* Sensor to GND, RSer pull-up: V = Vcc * R/(RSer + R). */
            return (cfg.rser > 0.0f) ? m_vcc * r / (cfg.rser + r) : NAN;
        }
        /* Sensor to VCC: V = gain * vexc * RSer/(R + RSer) — the inverse of
         * the hardware driver's evaluateChannel relation. */
        const float vexc = (cfg.vexc > 0.0f) ? cfg.vexc : m_vcc;
        return (r > 0.0f) ? cfg.gain * vexc * cfg.rser / (cfg.rser + r) : NAN;
    };

    for (uint8_t ch = 0; ch < NUM_CHANNELS; ++ch) {
        Channel& c = m_ch[ch];
        const float t = silWorld().temp_c[ch];
        if (!c.cfg.enabled || !std::isfinite(t)) {
            c.temp_c = NAN;
            c.voltage = NAN;
            c.resistance = NAN;
            c.out_of_range = false;
            c.oor_pending = false;
            c.over_temp_cond = false;
            c.over_temp_raised = false;
            continue;
        }
        c.resistance = tempToResistance(c.cfg, t);
        c.voltage = dividerVoltage(c.cfg, c.resistance);
        c.temp_c = std::isfinite(c.voltage) ? t : NAN;
        updateOutOfRange(ch, now_ms);
        updateOverTemp(ch, now_ms);
    }
}

float ApplicationSensors::motorTemperatureC() const {
    return m_ch[3].temp_c;
}

float ApplicationSensors::inverterTemperatureC(uint8_t channel) const {
    if (channel >= BOARD_CHANNELS) return NAN;
    return m_ch[channel].temp_c;
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
    if (ch >= NUM_CHANNELS) {
        enabled = false; type = TYPE_DISABLED; volts = NAN; ohms = NAN;
        tempC = NAN; outOfRange = false;
        return;
    }
    const Channel& c = m_ch[ch];
    enabled    = c.cfg.enabled;
    type       = c.cfg.type;
    volts      = c.voltage;
    ohms       = c.resistance;
    tempC      = c.temp_c;
    outOfRange = c.out_of_range;
}

void ApplicationSensors::debugStatus() const {
    Telemetry::printf("[SHELL] appSensors (SIL): temps scenario-driven, throttle %.2f/%.2f V",
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
