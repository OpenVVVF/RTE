#include "Inverter/Drivers/Sensors/TemperatureSensors.h"

#include "Inverter/Control/FaultManager.h"
#include "Inverter/Drivers/Storage/RteParamStore.h"
#include "Inverter/Telemetry.h"

#include "main.h"
#include "adc.h"

#include <cmath>
#include <cstdio>

namespace Inverter {

namespace {

TemperatureSensors s_instance;

/* adc.c configures ADC1 regular oversampling (ratio 16, no shift), but the
 * measured raw full scale on software-polled regular conversions is the
 * plain 16-bit range (railed pins read ~65535, not ~16*65535).  Use the
 * 16-bit full scale; this is correct to within 0.02% even if the oversampler
 * sums 12-bit conversions.  ADC3 is 12-bit with no oversampling. */
constexpr float ADC1_FULL_SCALE = 65535.0f;
constexpr float ADC3_FULL_SCALE = 4095.0f;

constexpr const char* TELEM_KEYS[TemperatureSensors::NUM_CHANNELS] = {
    "temp_inv1_c", "temp_inv2_c", "temp_inv3_c", "temp_motor_c",
};

constexpr const char* TELEM_VOLT_KEYS[TemperatureSensors::NUM_CHANNELS] = {
    "temp_inv1_v", "temp_inv2_v", "temp_inv3_v", "temp_motor_v",
};

constexpr const char* KV_PREFIX[TemperatureSensors::NUM_CHANNELS] = {
    "Hw.Temp.B1", "Hw.Temp.B2", "Hw.Temp.B3", "Motor.Temp",
};

constexpr uint32_t ADC1_CHANNELS[TemperatureSensors::BOARD_CHANNELS] = {
    ADC_CHANNEL_19, ADC_CHANNEL_17, ADC_CHANNEL_16,
};

/* KTY84 quadratic coefficients: R = R25 * (1 + A*dT + B*dT^2). */
constexpr float KTY84_A = 7.418e-3f;
constexpr float KTY84_B = 1.815e-5f;

} // namespace

TemperatureSensors& temperatureSensors() {
    return s_instance;
}

const char* TemperatureSensors::typeName(uint8_t type) {
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

void TemperatureSensors::channelStatus(uint8_t ch, bool& enabled, uint8_t& type,
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

float TemperatureSensors::motorTemperatureC() const {
    return m_ch[3].temp_c;
}

float TemperatureSensors::inverterTemperatureC(uint8_t channel) const {
    if (channel >= BOARD_CHANNELS) {
        return NAN;
    }
    return m_ch[channel].temp_c;
}

void TemperatureSensors::loadConfig(bool persist_defaults) {
    struct DefaultCfg {
        float en, type, r25, beta, rser, orient, crit;
    };
    static constexpr DefaultCfg BOARD_DEF = {1.0f, 1.0f, 10000.0f, 3950.0f,
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

bool TemperatureSensors::init() {
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

    m_initialized = true;
    m_last_cfg_ms = HAL_GetTick();
    return true;
}

bool TemperatureSensors::sampleOnce(uint8_t ch, uint32_t& raw) {
    ADC_HandleTypeDef* hadc = (ch == 3) ? &hadc3 : &hadc1;
    ADC_TypeDef* adc = hadc->Instance;

    /* Never use HAL_ADC_Stop()/HAL_ADC_Start() here: on H7 HAL_ADC_Stop stops
     * the INJECTED group as well as the regular one and then disables the
     * whole ADC peripheral.  That kills the phase-current (injected, dual
     * mode with ADC2) and encoder conversions for good, since injected start
     * is only issued once at init.  Program the regular sequence and start a
     * single conversion via LL instead; injected conversions keep running. */

    /* Stop a lingering regular conversion (regular group only). */
    if (LL_ADC_REG_IsConversionOngoing(adc)) {
        LL_ADC_REG_StopConversion(adc);
        uint32_t guard = 100000U;
        while (LL_ADC_REG_IsConversionOngoing(adc) && guard-- != 0U) {
        }
        if (LL_ADC_REG_IsConversionOngoing(adc)) {
            return false;
        }
    }

    ADC_ChannelConfTypeDef sConfig = {};
    sConfig.Channel = (ch == 3) ? ADC_CHANNEL_9 : ADC1_CHANNELS[ch];
    sConfig.Rank = ADC_REGULAR_RANK_1;
    sConfig.SamplingTime = (ch == 3) ? ADC3_SAMPLETIME_92CYCLES_5
                                     : ADC_SAMPLETIME_64CYCLES_5;
    sConfig.SingleDiff = ADC_SINGLE_ENDED;
    sConfig.OffsetNumber = ADC_OFFSET_NONE;
    sConfig.Offset = 0;
    if (HAL_ADC_ConfigChannel(hadc, &sConfig) != HAL_OK) {
        return false;
    }

    LL_ADC_ClearFlag_EOC(adc);
    LL_ADC_REG_StartConversion(adc);

    uint32_t guard = 200000U;
    while (!LL_ADC_IsActiveFlag_EOC(adc) && guard-- != 0U) {
    }
    if (!LL_ADC_IsActiveFlag_EOC(adc)) {
        return false;
    }

    raw = LL_ADC_REG_ReadConversionData32(adc);
    return true;
}

float TemperatureSensors::resistanceToTempC(const Config& cfg, float r) const {
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

void TemperatureSensors::updateOutOfRange(uint8_t ch, uint32_t now_ms) {
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

void TemperatureSensors::updateOverTemp(uint8_t ch, uint32_t now_ms) {
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
        } else {
            static const FaultReason OT_REASONS[BOARD_CHANNELS] = {
                FaultReason::OvertemperatureInv1, FaultReason::OvertemperatureInv2,
                FaultReason::OvertemperatureInv3,
            };
            FaultManager::instance().raise(FaultSource::OvertemperatureInverter,
                                           OT_REASONS[ch]);
        }
    }
}

void TemperatureSensors::finishAverage(uint8_t ch, uint32_t now_ms) {
    Channel& c = m_ch[ch];
    const float raw_avg = static_cast<float>(c.raw_accum) /
                          static_cast<float>(ACCUM_SAMPLES);
    const float full_scale = (ch == 3) ? ADC3_FULL_SCALE : ADC1_FULL_SCALE;
    c.voltage = (raw_avg / full_scale) * m_vcc;

    const Config& cfg = c.cfg;

    /* A railed voltage means open/short sensor (or pending confirmation of
     * it): the channel is never "in range", so the temperature is NAN and
     * the over-temperature check must not run on a divider railed at Vcc/GND. */
    const bool railed = (c.voltage > OOR_OPEN_RATIO * m_vcc) ||
                        (c.voltage < OOR_SHORT_RATIO * m_vcc);

    updateOutOfRange(ch, now_ms);

    if (!cfg.enabled || railed) {
        c.resistance = NAN;
        c.temp_c = NAN;
    } else {
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

    updateOverTemp(ch, now_ms);

    Telemetry::log(TELEM_KEYS[ch], c.temp_c);
    Telemetry::log(TELEM_VOLT_KEYS[ch], c.voltage);
}

void TemperatureSensors::update() {
    if (!m_initialized) {
        return;
    }

    const uint32_t now_ms = HAL_GetTick();

    /* Re-read the KV RAM cache once per second so `config set` applies live. */
    if ((now_ms - m_last_cfg_ms) >= CONFIG_RELOAD_MS) {
        loadConfig(/*persist_defaults=*/false);
        m_last_cfg_ms = now_ms;
    }

    /* One software-triggered conversion per call, round-robin. */
    Channel& c = m_ch[m_rr];
    uint32_t raw = 0;
    if (sampleOnce(m_rr, raw)) {
        c.raw_accum += raw;
        if (++c.accum_count >= ACCUM_SAMPLES) {
            finishAverage(m_rr, now_ms);
            c.raw_accum = 0;
            c.accum_count = 0;
        }
    }
    m_rr = static_cast<uint8_t>((m_rr + 1U) % NUM_CHANNELS);
}

} // namespace Inverter
