#include "Inverter/Drivers/Sensors/ApplicationSensors.h"

#include "Inverter/Drivers/Storage/RteParamStore.h"
#include "Inverter/Telemetry.h"

#include "main.h"
#include "adc.h"

#include <cmath>
#include <cstdio>

namespace Inverter {

namespace {

ApplicationSensors s_instance;

/* ADC3 runs plain 12-bit single conversions (full scale 4095). */
constexpr float ADC3_FULL_SCALE = 4095.0f;

constexpr const char* KV_PREFIX[ApplicationSensors::NUM_CHANNELS] = {
    "Hw.Temp.B1", "Hw.Temp.B2", "Hw.Temp.B3", "Motor.Temp",
};

/* KTY84 quadratic coefficients: R = R25 * (1 + A*dT + B*dT^2). */
constexpr float KTY84_A = 7.418e-3f;
constexpr float KTY84_B = 1.815e-5f;

} // namespace

ApplicationSensors& appSensors() {
    return s_instance;
}

const char* ApplicationSensors::typeName(uint8_t type) {
    switch (type) {
        case TYPE_DISABLED:   return "disabled";
        case TYPE_NTC_BETA:   return "NTC-beta";
        case TYPE_PTC_BETA:   return "PTC-beta";
        case TYPE_KTY84:      return "KTY84-130/150";
        case TYPE_LINEAR_RTD: return "linear-RTD";
        case TYPE_PT1000:     return "PT1000";
        case TYPE_PT100:      return "PT100";
        case TYPE_KTY83_110:  return "KTY83-110";
        default:              return "?";
    }
}

float ApplicationSensors::motorTemperatureC() const {
    return m_ch[3].temp_c;
}

float ApplicationSensors::inverterTemperatureC(uint8_t channel) const {
    if (channel >= BOARD_CHANNELS) {
        return NAN;
    }
    return m_ch[channel].temp_c;
}

float ApplicationSensors::throttleAVoltage() const {
    return m_thr_a_v;
}

float ApplicationSensors::throttleBVoltage() const {
    return m_thr_b_v;
}

void ApplicationSensors::debugStatus() const {
    Telemetry::printf("[SHELL] adc3: ISR=%08lX CFGR=%08lX raw_latest=%lu",
                      static_cast<unsigned long>(hadc3.Instance->ISR),
                      static_cast<unsigned long>(hadc3.Instance->CFGR),
                      static_cast<unsigned long>(m_adc3_latest));
}

void ApplicationSensors::channelStatus(uint8_t ch, bool& enabled, uint8_t& type,
                                       float& volts, float& ohms, float& tempC,
                                       bool& outOfRange) const {
    if (ch >= NUM_CHANNELS) {
        enabled = false; type = 0; volts = NAN; ohms = NAN; tempC = NAN;
        outOfRange = false;
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
    }

    m_vcc = loadOne("Hw.Temp.Vcc", 3.3f);

    if (persist_defaults && RteParamStore::isReady()) {
        RteParamStore::flush();
    }
}

void ApplicationSensors::reloadConfig() {
    loadConfig(/*persist_defaults=*/false);
    Telemetry::printf("[TMP] config reloaded");
}

bool ApplicationSensors::init() {
    for (uint8_t i = 0; i < NUM_CHANNELS; ++i) {
        m_ch[i].temp_c = NAN;
        m_ch[i].resistance = NAN;
    }

    loadConfig(/*persist_defaults=*/true);

    Telemetry::printf("[TMP] initialized: Vcc=%.2f V", static_cast<double>(m_vcc));
    for (uint8_t i = 0; i < NUM_CHANNELS; ++i) {
        const Config& c = m_ch[i].cfg;
        Telemetry::printf("[TMP] %s: en=%d type=%s R25=%.0f beta=%.4f RSer=%.0f orient=%d crit=%.0fC",
                          KV_PREFIX[i],
                          c.enabled ? 1 : 0,
                          typeName(c.type),
                          static_cast<double>(c.r25),
                          static_cast<double>(c.beta),
                          static_cast<double>(c.rser),
                          static_cast<int>(c.orient),
                          static_cast<double>(c.crit_c));
    }

    /* --- ADC3 (motor temp): continuous conversions, polled via EOC --------
     * Register-level setup at init only; the CPU never touches the ADC at
     * runtime beyond reading DR.  ADC3 (v90 IP) does not accept the ADC1/2
     * CFGR2 oversampling layout; keep it plain 12-bit and read the latest
     * sample — fine for a temperature channel. */
    hadc3.Init.ContinuousConvMode = ENABLE;
    LL_ADC_REG_SetContinuousMode(hadc3.Instance, LL_ADC_REG_CONV_CONTINUOUS);
    LL_ADC_REG_SetSequencerLength(hadc3.Instance, LL_ADC_REG_SEQ_SCAN_DISABLE);
    LL_ADC_REG_SetSequencerRanks(hadc3.Instance, LL_ADC_REG_RANK_1, ADC_CHANNEL_9);
    LL_ADC_SetChannelSamplingTime(hadc3.Instance, ADC_CHANNEL_9,
                                  ADC3_SAMPLETIME_92CYCLES_5);
    CLEAR_BIT(hadc3.Instance->CFGR2, ADC_CFGR2_ROVSE);
    MODIFY_REG(hadc3.Instance->CFGR, ADC_CFGR_EXTEN | ADC_CFGR_EXTSEL,
               ADC_EXTERNALTRIGCONVEDGE_NONE | ADC_SOFTWARE_START);

    if (HAL_ADC_Start(&hadc3) != HAL_OK) {
        Telemetry::printf("[TMP] ERROR: ADC3 start failed");
        return false;
    }

    m_initialized = true;
    m_last_window_ms = HAL_GetTick();
    return true;
}

float ApplicationSensors::resistanceToTempC(const Config& cfg, float r) const {
    auto ktyQuadratic = [](float r, float r25) -> float {
        /* Invert R/R25 = 1 + A*dT + B*dT^2 via the quadratic formula. */
        const float ratio = r / r25;
        const float disc = KTY84_A * KTY84_A + 4.0f * KTY84_B * (ratio - 1.0f);
        if (disc < 0.0f) {
            return NAN;
        }
        return (-KTY84_A + std::sqrt(disc)) / (2.0f * KTY84_B);
    };

    switch (cfg.type) {
        case TYPE_NTC_BETA:
        case TYPE_PTC_BETA: {
            /* Same beta equation for NTC and PTC (approximate). */
            if (cfg.r25 <= 0.0f || cfg.beta == 0.0f || r <= 0.0f) {
                return NAN;
            }
            const float t_k = 1.0f / (1.0f / 298.15f + std::log(r / cfg.r25) / cfg.beta);
            return t_k - 273.15f;
        }
        case TYPE_KTY84:
            /* KTY84-130/150; R25 configurable (typ 603 ohm). */
            if (cfg.r25 <= 0.0f) {
                return NAN;
            }
            return ktyQuadratic(r, cfg.r25);
        case TYPE_KTY83_110:
            /* KTY83-110 (R25 ~1000 ohm); KTY84 curve shape is a close
             * approximation (a couple of degC) and fine for sensor-ID. */
            return ktyQuadratic(r, 1000.0f);
        case TYPE_LINEAR_RTD: {
            /* T = (R/R25 - 1) / Beta, Beta = alpha (e.g. 0.00385 for PT1000). */
            if (cfg.r25 <= 0.0f || cfg.beta == 0.0f) {
                return NAN;
            }
            return (r / cfg.r25 - 1.0f) / cfg.beta;
        }
        case TYPE_PT1000:
            /* IEC 60751 class B linear region: R = 1000 * (1 + 0.00385*T). */
            return (r / 1000.0f - 1.0f) / 0.00385f;
        case TYPE_PT100:
            return (r / 100.0f - 1.0f) / 0.00385f;
        default:
            return NAN;
    }
}

void ApplicationSensors::evaluateChannel(uint8_t ch) {
    Channel& c = m_ch[ch];
    const Config& cfg = c.cfg;

    /* A railed voltage means open/short sensor: the channel is never
     * "in range", so the temperature is NAN. */
    const bool railed = (c.voltage > OOR_OPEN_RATIO * m_vcc) ||
                        (c.voltage < OOR_SHORT_RATIO * m_vcc);
    c.out_of_range = cfg.enabled && railed;

    if (!cfg.enabled || railed) {
        c.resistance = NAN;
        c.temp_c = NAN;
        return;
    }

    /* Divider -> resistance, then resistance -> temperature. */
    const float v = c.voltage;
    float r = NAN;
    if (cfg.orient == 0) {
        /* Sensor to GND, RSer pull-up to VCC: R = RSer * V/(VCC - V). */
        if (v < m_vcc) {
            r = cfg.rser * v / (m_vcc - v);
        }
    } else {
        /* Sensor to VCC: R = RSer * (VCC - V)/V. */
        if (v > 0.0f) {
            r = cfg.rser * (m_vcc - v) / v;
        }
    }
    c.resistance = r;
    c.temp_c = std::isfinite(r) ? resistanceToTempC(cfg, r) : NAN;
}

void ApplicationSensors::computeWindow() {
    /* Motor temp (ADC3 continuous): latest sample. */
    m_ch[3].voltage = (static_cast<float>(m_adc3_latest) / ADC3_FULL_SCALE) * m_vcc;

    for (uint8_t ch = 0; ch < NUM_CHANNELS; ++ch) {
        evaluateChannel(ch);
    }
}

void ApplicationSensors::update() {
    if (!m_initialized) {
        return;
    }

    /* Harvest the latest completed ADC3 conversion; never blocks.  Reading
     * DR clears EOC, and continuous mode immediately starts the next one. */
    if (LL_ADC_IsActiveFlag_EOC(hadc3.Instance)) {
        m_adc3_latest = LL_ADC_REG_ReadConversionData32(hadc3.Instance);
    }

    const uint32_t now_ms = HAL_GetTick();
    if ((now_ms - m_last_window_ms) >= WINDOW_MS) {
        computeWindow();
        m_last_window_ms = now_ms;
    }
}

} // namespace Inverter
