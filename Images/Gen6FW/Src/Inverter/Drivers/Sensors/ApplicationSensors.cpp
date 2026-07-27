#include "Inverter/Drivers/Sensors/ApplicationSensors.h"

#include "Inverter/Control/FaultManager.h"
#include "Inverter/Drivers/Storage/RteParamStore.h"
#include "Inverter/Telemetry.h"

#include "main.h"
#include "adc.h"
#include "dma.h"
#include "tim.h"

#include <cmath>
#include <cstdio>

namespace Inverter {

namespace {

ApplicationSensors s_instance;

/* ADC1 runs plain 16-bit single conversions (full scale 65535) — regular
 * oversampling is deliberately OFF because it delays injected phase-current
 * triggers to sequence boundaries.  ADC3 runs plain 12-bit (full scale 4095).
 * No software averaging anywhere. */
constexpr float ADC1_FULL_SCALE = 65535.0f;
constexpr float ADC3_FULL_SCALE = 4095.0f;

constexpr const char* KV_PREFIX[ApplicationSensors::NUM_CHANNELS] = {
    "Hw.Temp.B1", "Hw.Temp.B2", "Hw.Temp.B3", "Motor.Temp",
};

/* ADC1 scan rank order (board temp 1/2/3, throttle A/B).  The throttle pins
 * PA3/PA4 are shared ADC1/ADC2 inputs, so ADC1 can read them while the
 * encoder owns ADC2's regular group. */
constexpr uint32_t ADC1_RANK_CHANNELS[ApplicationSensors::ADC1_RANKS] = {
    ADC_CHANNEL_19, ADC_CHANNEL_17, ADC_CHANNEL_16, ADC_CHANNEL_15, ADC_CHANNEL_18,
};

/* KTY84 quadratic coefficients: R = R25 * (1 + A*dT + B*dT^2). */
constexpr float KTY84_A = 7.418e-3f;
constexpr float KTY84_B = 1.815e-5f;

/* TIM3 TRGO (1 kHz) -> ADC1 scan -> circular DMA, mirroring the encoder's
 * TIM2 -> ADC2 setup.  Buffer holds the latest scan (one value per rank).
 * Must live in .dma_buffers (AXI SRAM): DMA1/DMA2 cannot access DTCM. */
TIM_HandleTypeDef htim3_appsens;
DMA_HandleTypeDef hdma_adc1_appsens;
uint16_t s_adc1_buf[ApplicationSensors::ADC1_RANKS]
    __attribute__((section(".dma_buffers")));

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

float ApplicationSensors::throttleA() const {
    return m_thr_a_norm;
}

float ApplicationSensors::throttleB() const {
    return m_thr_b_norm;
}

bool ApplicationSensors::throttlePlausible() const {
    return m_thr_plausible;
}

void ApplicationSensors::updateThrottlePlausibility(uint32_t now_ms) {
    const float diff = std::fabs(m_thr_a_cand - m_thr_b_cand);
    if (diff <= THROTTLE_PLAUS_TOL) {
        m_thr_plausible = true;
        m_thr_implausible_since = 0;
        m_thr_fault_raised = false;  /**< re-arm: a cleared fault can re-raise */
        return;
    }
    if (m_thr_implausible_since == 0) {
        m_thr_implausible_since = now_ms;
        return;
    }
    if ((now_ms - m_thr_implausible_since) >= THROTTLE_PLAUS_MS) {
        m_thr_plausible = false;
        if (!m_thr_fault_raised) {
            m_thr_fault_raised = true;
            FaultManager::instance().raise(FaultSource::ThrottlePlausibility,
                                           FaultReason::ThrottlePlausibilityMismatch);
        }
    }
}

void ApplicationSensors::debugStatus() const {
    Telemetry::printf("[SHELL] adc3: ISR=%08lX CFGR=%08lX raw_latest=%lu",
                      static_cast<unsigned long>(hadc3.Instance->ISR),
                      static_cast<unsigned long>(hadc3.Instance->CFGR),
                      static_cast<unsigned long>(m_adc3_latest));
    Telemetry::printf("[SHELL] adc1: ISR=%08lX CFGR=%08lX SQR1=%08lX tim3_cr1=%08lX",
                      static_cast<unsigned long>(hadc1.Instance->ISR),
                      static_cast<unsigned long>(hadc1.Instance->CFGR),
                      static_cast<unsigned long>(hadc1.Instance->SQR1),
                      static_cast<unsigned long>(TIM3->CR1));
    Telemetry::printf("[SHELL] dma2s1: CR=%08lX NDTR=%lu buf[0..4]: %u %u %u %u %u",
                      static_cast<unsigned long>(DMA2_Stream1->CR),
                      static_cast<unsigned long>(DMA2_Stream1->NDTR),
                      s_adc1_buf[0], s_adc1_buf[1], s_adc1_buf[2],
                      s_adc1_buf[3], s_adc1_buf[4]);
}

bool ApplicationSensors::configureAdc1ScanDma() {
    /* --- ADC1 regular scan: 5 ranks, TIM3 TRGO, circular DMA -------------
     * Register-level setup (mirrors EncoderADC): HAL_ADC_ConfigChannel would
     * fight the injected phase-current state machine; direct writes to the
     * regular-group registers do not disturb injected conversions. */
    LL_ADC_REG_SetSequencerLength(hadc1.Instance, LL_ADC_REG_SEQ_SCAN_ENABLE_5RANKS);
    static constexpr uint32_t RANKS[ADC1_RANKS] = {
        LL_ADC_REG_RANK_1, LL_ADC_REG_RANK_2, LL_ADC_REG_RANK_3,
        LL_ADC_REG_RANK_4, LL_ADC_REG_RANK_5,
    };
    for (uint8_t r = 0; r < ADC1_RANKS; ++r) {
        LL_ADC_REG_SetSequencerRanks(hadc1.Instance, RANKS[r],
                                     ADC1_RANK_CHANNELS[r]);
        LL_ADC_SetChannelSamplingTime(hadc1.Instance, ADC1_RANK_CHANNELS[r],
                                      LL_ADC_SAMPLINGTIME_64CYCLES_5);
    }
    CLEAR_BIT(hadc1.Instance->CFGR, ADC_CFGR_DISCEN | ADC_CFGR_DISCNUM);
    /* No regular oversampling: an injected (phase current) trigger can only
     * preempt a regular OVERSAMPLED sequence at sequence boundaries, delaying
     * the current sample by tens of microseconds (onto a PWM switching edge)
     * and tripping software overcurrent.  Plain single conversions preempt
     * in ~3 us.  16-bit single samples are quiet enough for these channels. */
    CLEAR_BIT(hadc1.Instance->CFGR2, ADC_CFGR2_ROVSE);
    /* Trigger from TIM3 TRGO, rising edge. */
    MODIFY_REG(hadc1.Instance->CFGR, ADC_CFGR_EXTEN | ADC_CFGR_EXTSEL,
               ADC_EXTERNALTRIGCONVEDGE_RISING | ADC_EXTERNALTRIG_T3_TRGO);

    /* Tell HAL_ADC_Start_DMA() to use circular DMA mode. */
    hadc1.Init.ScanConvMode = ADC_SCAN_ENABLE;
    hadc1.Init.ContinuousConvMode = DISABLE;
    hadc1.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DMA_CIRCULAR;
    hadc1.Init.DMAContinuousRequests = ENABLE;

    /* --- TIM3: 1 kHz update event as ADC1 trigger -------------------------
     * APB1 timer clock is 137.5 MHz (same as the encoder's TIM2):
     * 137.5 MHz / 1375 / 100 = 1 kHz. */
    __HAL_RCC_TIM3_CLK_ENABLE();
    htim3_appsens.Instance = TIM3;
    htim3_appsens.Init.Prescaler = 1374U;
    htim3_appsens.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim3_appsens.Init.Period = 99U;
    htim3_appsens.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim3_appsens.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_Base_Init(&htim3_appsens) != HAL_OK) {
        return false;
    }
    TIM_MasterConfigTypeDef sMasterConfig = {};
    sMasterConfig.MasterOutputTrigger = TIM_TRGO_UPDATE;
    sMasterConfig.MasterOutputTrigger2 = TIM_TRGO2_RESET;
    sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
    if (HAL_TIMEx_MasterConfigSynchronization(&htim3_appsens, &sMasterConfig) != HAL_OK) {
        return false;
    }

    /* --- DMA2_Stream1: ADC1 -> circular buffer --------------------------- */
    __HAL_RCC_DMA2_CLK_ENABLE();
    hdma_adc1_appsens.Instance = DMA2_Stream1;
    hdma_adc1_appsens.Init.Request = DMA_REQUEST_ADC1;
    hdma_adc1_appsens.Init.Direction = DMA_PERIPH_TO_MEMORY;
    hdma_adc1_appsens.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_adc1_appsens.Init.MemInc = DMA_MINC_ENABLE;
    hdma_adc1_appsens.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma_adc1_appsens.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
    hdma_adc1_appsens.Init.Mode = DMA_CIRCULAR;
    hdma_adc1_appsens.Init.Priority = DMA_PRIORITY_MEDIUM;
    hdma_adc1_appsens.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_adc1_appsens) != HAL_OK) {
        return false;
    }
    __HAL_LINKDMA(&hadc1, DMA_Handle, hdma_adc1_appsens);

    return true;
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
        Telemetry::printf("[TMP] %s: en=%d type=%s R25=%.0f beta=%.4f RSer=%.0f orient=%d crit=%.0fC vexc=%.2f gain=%.3f",
                          KV_PREFIX[i],
                          c.enabled ? 1 : 0,
                          typeName(c.type),
                          static_cast<double>(c.r25),
                          static_cast<double>(c.beta),
                          static_cast<double>(c.rser),
                          static_cast<int>(c.orient),
                          static_cast<double>(c.crit_c),
                          static_cast<double>(c.vexc),
                          static_cast<double>(c.gain));
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

    /* --- ADC1 scan: board temps + throttle via TIM3 TRGO + DMA ----------- */
    if (!configureAdc1ScanDma()) {
        Telemetry::printf("[TMP] ERROR: ADC1/trigger/DMA setup failed");
        return false;
    }
    if (HAL_ADC_Start_DMA(&hadc1, reinterpret_cast<uint32_t*>(s_adc1_buf),
                          ADC1_RANKS) != HAL_OK) {
        Telemetry::printf("[TMP] ERROR: ADC1 DMA start failed");
        return false;
    }
    if (HAL_TIM_Base_Start(&htim3_appsens) != HAL_OK) {
        HAL_ADC_Stop_DMA(&hadc1);
        Telemetry::printf("[TMP] ERROR: TIM3 start failed");
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

void ApplicationSensors::evaluateChannel(uint8_t ch, uint32_t now_ms) {
    Channel& c = m_ch[ch];
    const Config& cfg = c.cfg;

    /* A railed voltage means open/short sensor (or pending confirmation of
     * it): the channel is never "in range", so the temperature is NAN and
     * the over-temperature check must not run on a divider railed at Vcc/GND. */
    const bool railed = (c.voltage > OOR_OPEN_RATIO * m_vcc) ||
                        (c.voltage < OOR_SHORT_RATIO * m_vcc);

    if (cfg.enabled) {
        updateOutOfRange(ch, now_ms);
    }

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
            /* Sensor to VCC (or higher rail via post-divider):
             * V = gain * vexc * RSer/(R + RSer)  ->  R = RSer * (gain*vexc/V - 1). */
            const float vexc = (cfg.vexc > 0.0f) ? cfg.vexc : m_vcc;
            if (v > 0.0f) {
                r = cfg.rser * (cfg.gain * vexc / v - 1.0f);
            }
        }
        c.resistance = r;
        c.temp_c = std::isfinite(r) ? resistanceToTempC(cfg, r) : NAN;
    }

    updateOverTemp(ch, now_ms);
}

void ApplicationSensors::computeWindow(uint32_t now_ms) {
    /* The DMA buffer holds the latest 1 kHz scan; just read it. */
    for (uint8_t r = 0; r < ADC1_RANKS; ++r) {
        const float volts = (static_cast<float>(s_adc1_buf[r]) / ADC1_FULL_SCALE) * m_vcc;
        if (r < BOARD_CHANNELS) {
            m_ch[r].voltage = volts;
        } else if (r == 3) {
            m_thr_a_v = volts;
        } else {
            m_thr_b_v = volts;
        }
    }
    Telemetry::log("thr_a_v", m_thr_a_v);
    Telemetry::log("thr_b_v", m_thr_b_v);

    /* Normalize via KV min/max; candidates feed the plausibility check, and
     * the exposed values zero out while the channels disagree. */
    auto normalize = [](float v, float lo, float hi) -> float {
        if (hi <= lo) {
            return 0.0f;
        }
        float n = (v - lo) / (hi - lo);
        if (n < 0.0f) n = 0.0f;
        if (n > 1.0f) n = 1.0f;
        return n;
    };
    m_thr_a_cand = normalize(m_thr_a_v, m_thr_min_v[0], m_thr_max_v[0]);
    m_thr_b_cand = normalize(m_thr_b_v, m_thr_min_v[1], m_thr_max_v[1]);
    m_thr_a_norm = m_thr_plausible ? m_thr_a_cand : 0.0f;
    m_thr_b_norm = m_thr_plausible ? m_thr_b_cand : 0.0f;
    Telemetry::log("thr_a", m_thr_a_norm);
    Telemetry::log("thr_b", m_thr_b_norm);

    /* Motor temp (ADC3 continuous): latest sample. */
    m_ch[3].voltage = (static_cast<float>(m_adc3_latest) / ADC3_FULL_SCALE) * m_vcc;

    for (uint8_t ch = 0; ch < NUM_CHANNELS; ++ch) {
        evaluateChannel(ch, now_ms);
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
        computeWindow(now_ms);
        m_last_window_ms = now_ms;
    }
    updateThrottlePlausibility(now_ms);
}

} // namespace Inverter
