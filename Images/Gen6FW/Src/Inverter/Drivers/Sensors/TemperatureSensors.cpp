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

/* adc.c configures ADC1 regular oversampling; configureAdcAndTrigger() turns
 * it OFF for the application scan (ROVSE), so the regular conversions are
 * plain 16-bit.  ADC3 is 12-bit with no oversampling. */
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

/* ADC1 scan rank order (board temp 1/2/3, throttle A/B).  The throttle pins
 * PA3/PA4 are shared ADC1/ADC2 inputs, so ADC1 can read them while the
 * encoder owns ADC2's regular group. */
constexpr uint32_t ADC1_RANK_CHANNELS[TemperatureSensors::ADC1_RANKS] = {
    ADC_CHANNEL_19, ADC_CHANNEL_17, ADC_CHANNEL_16, ADC_CHANNEL_15, ADC_CHANNEL_18,
};

/* KTY84 quadratic coefficients: R = R25 * (1 + A*dT + B*dT^2). */
constexpr float KTY84_A = 7.418e-3f;
constexpr float KTY84_B = 1.815e-5f;

/* TIM3 TRGO (1 kHz) -> ADC1 scan -> circular DMA, mirroring the encoder's
 * TIM2 -> ADC2 setup.  Buffer holds SAMPLES_PER_CHANNEL scans per rank.
 * Must live in .dma_buffers (AXI SRAM): DMA1/DMA2 cannot access DTCM. */
TIM_HandleTypeDef htim3_temp;
DMA_HandleTypeDef hdma_adc1_temp;
uint16_t s_adc1_buf[TemperatureSensors::ADC1_RANKS *
                    TemperatureSensors::SAMPLES_PER_CHANNEL]
    __attribute__((section(".dma_buffers")));

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

float TemperatureSensors::motorTemperatureC() const {
    return m_ch[3].temp_c;
}

float TemperatureSensors::inverterTemperatureC(uint8_t channel) const {
    if (channel >= BOARD_CHANNELS) {
        return NAN;
    }
    return m_ch[channel].temp_c;
}

float TemperatureSensors::throttleAVoltage() const {
    return m_thr_a_v;
}

float TemperatureSensors::throttleBVoltage() const {
    return m_thr_b_v;
}

void TemperatureSensors::debugStatus() const {
    Telemetry::printf("[SHELL] adc1: ISR=%08lX CFGR=%08lX SQR1=%08lX tim3_cr1=%08lX",
                      static_cast<unsigned long>(hadc1.Instance->ISR),
                      static_cast<unsigned long>(hadc1.Instance->CFGR),
                      static_cast<unsigned long>(hadc1.Instance->SQR1),
                      static_cast<unsigned long>(TIM3->CR1));
    Telemetry::printf("[SHELL] dma2s1: CR=%08lX NDTR=%lu PAR=%08lX M0AR=%08lX buf=%08lX",
                      static_cast<unsigned long>(DMA2_Stream1->CR),
                      static_cast<unsigned long>(DMA2_Stream1->NDTR),
                      static_cast<unsigned long>(DMA2_Stream1->PAR),
                      static_cast<unsigned long>(DMA2_Stream1->M0AR),
                      static_cast<unsigned long>(reinterpret_cast<uintptr_t>(s_adc1_buf)));
    Telemetry::printf("[SHELL] buf[0..7]: %u %u %u %u %u %u %u %u",
                      s_adc1_buf[0], s_adc1_buf[1], s_adc1_buf[2], s_adc1_buf[3],
                      s_adc1_buf[4], s_adc1_buf[5], s_adc1_buf[6], s_adc1_buf[7]);
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

void TemperatureSensors::loadConfig(bool persist_defaults) {
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

bool TemperatureSensors::configureAdcAndTrigger() {
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
    /* No regular oversampling: the DMA window is averaged in software. */
    CLEAR_BIT(hadc1.Instance->CFGR2, ADC_CFGR2_ROVSE);
    /* Trigger from TIM3 TRGO, rising edge. */
    MODIFY_REG(hadc1.Instance->CFGR, ADC_CFGR_EXTEN | ADC_CFGR_EXTSEL,
               ADC_EXTERNALTRIGCONVEDGE_RISING | ADC_EXTERNALTRIG_T3_TRGO);

    /* Tell HAL_ADC_Start_DMA() to use circular DMA mode. */
    hadc1.Init.ScanConvMode = ADC_SCAN_ENABLE;
    hadc1.Init.ContinuousConvMode = DISABLE;
    hadc1.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DMA_CIRCULAR;
    hadc1.Init.DMAContinuousRequests = ENABLE;

    /* --- ADC3 (motor temp): continuous conversions, polled via EOC ------- */
    hadc3.Init.ContinuousConvMode = ENABLE;
    LL_ADC_REG_SetContinuousMode(hadc3.Instance, LL_ADC_REG_CONV_CONTINUOUS);
    LL_ADC_REG_SetSequencerLength(hadc3.Instance, LL_ADC_REG_SEQ_SCAN_DISABLE);
    LL_ADC_REG_SetSequencerRanks(hadc3.Instance, LL_ADC_REG_RANK_1, ADC_CHANNEL_9);
    LL_ADC_SetChannelSamplingTime(hadc3.Instance, ADC_CHANNEL_9,
                                  ADC3_SAMPLETIME_92CYCLES_5);
    /* No external trigger: software-started continuous. */
    MODIFY_REG(hadc3.Instance->CFGR, ADC_CFGR_EXTEN | ADC_CFGR_EXTSEL,
               ADC_EXTERNALTRIGCONVEDGE_NONE | ADC_SOFTWARE_START);

    /* --- TIM3: 1 kHz update event as ADC1 trigger -------------------------
     * APB1 timer clock is 137.5 MHz (same as the encoder's TIM2):
     * 137.5 MHz / 1375 / 100 = 1 kHz. */
    __HAL_RCC_TIM3_CLK_ENABLE();
    htim3_temp.Instance = TIM3;
    htim3_temp.Init.Prescaler = 1374U;
    htim3_temp.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim3_temp.Init.Period = 99U;
    htim3_temp.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim3_temp.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_Base_Init(&htim3_temp) != HAL_OK) {
        return false;
    }
    TIM_MasterConfigTypeDef sMasterConfig = {};
    sMasterConfig.MasterOutputTrigger = TIM_TRGO_UPDATE;
    sMasterConfig.MasterOutputTrigger2 = TIM_TRGO2_RESET;
    sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
    if (HAL_TIMEx_MasterConfigSynchronization(&htim3_temp, &sMasterConfig) != HAL_OK) {
        return false;
    }

    /* --- DMA2_Stream1: ADC1 -> circular buffer --------------------------- */
    __HAL_RCC_DMA2_CLK_ENABLE();
    hdma_adc1_temp.Instance = DMA2_Stream1;
    hdma_adc1_temp.Init.Request = DMA_REQUEST_ADC1;
    hdma_adc1_temp.Init.Direction = DMA_PERIPH_TO_MEMORY;
    hdma_adc1_temp.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_adc1_temp.Init.MemInc = DMA_MINC_ENABLE;
    hdma_adc1_temp.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma_adc1_temp.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
    hdma_adc1_temp.Init.Mode = DMA_CIRCULAR;
    hdma_adc1_temp.Init.Priority = DMA_PRIORITY_MEDIUM;
    hdma_adc1_temp.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_adc1_temp) != HAL_OK) {
        return false;
    }
    __HAL_LINKDMA(&hadc1, DMA_Handle, hdma_adc1_temp);

    return true;
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

    if (!configureAdcAndTrigger()) {
        Telemetry::printf("[TMP] ERROR: ADC/trigger/DMA setup failed");
        return false;
    }

    if (HAL_ADC_Start_DMA(&hadc1, reinterpret_cast<uint32_t*>(s_adc1_buf),
                          ADC1_RANKS * SAMPLES_PER_CHANNEL) != HAL_OK) {
        Telemetry::printf("[TMP] ERROR: ADC1 DMA start failed");
        return false;
    }
    if (HAL_TIM_Base_Start(&htim3_temp) != HAL_OK) {
        HAL_ADC_Stop_DMA(&hadc1);
        Telemetry::printf("[TMP] ERROR: TIM3 start failed");
        return false;
    }
    /* ADC3 free-runs in continuous mode; update() harvests EOC. */
    if (HAL_ADC_Start(&hadc3) != HAL_OK) {
        Telemetry::printf("[TMP] ERROR: ADC3 start failed");
        return false;
    }

    m_initialized = true;
    m_last_cfg_ms = HAL_GetTick();
    m_last_window_ms = m_last_cfg_ms;
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

void TemperatureSensors::evaluateChannel(uint8_t ch, uint32_t now_ms) {
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

void TemperatureSensors::computeWindow(uint32_t now_ms) {
    /* Average the circular DMA window: 5 ranks x 16 scans at 1 kHz.  A torn
     * window just mixes adjacent 1 ms samples in the average — harmless at
     * these signal speeds. */
    for (uint8_t r = 0; r < ADC1_RANKS; ++r) {
        uint32_t sum = 0;
        for (uint8_t k = 0; k < SAMPLES_PER_CHANNEL; ++k) {
            sum += s_adc1_buf[k * ADC1_RANKS + r];
        }
        const float avg = static_cast<float>(sum) /
                          static_cast<float>(SAMPLES_PER_CHANNEL);
        const float volts = (avg / ADC1_FULL_SCALE) * m_vcc;
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

    /* Motor temp (ADC3 continuous): average whatever completed this window. */
    if (m_adc3_count != 0U) {
        const float avg = static_cast<float>(m_adc3_accum) /
                          static_cast<float>(m_adc3_count);
        m_ch[3].voltage = (avg / ADC3_FULL_SCALE) * m_vcc;
        m_adc3_accum = 0;
        m_adc3_count = 0;
    }

    for (uint8_t ch = 0; ch < NUM_CHANNELS; ++ch) {
        evaluateChannel(ch, now_ms);
    }
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

    /* Harvest completed ADC3 conversions; never blocks.  Reading DR clears
     * EOC, and continuous mode immediately starts the next conversion. */
    while (LL_ADC_IsActiveFlag_EOC(hadc3.Instance)) {
        m_adc3_accum += LL_ADC_REG_ReadConversionData32(hadc3.Instance);
        ++m_adc3_count;
    }

    if ((now_ms - m_last_window_ms) >= WINDOW_MS) {
        computeWindow(now_ms);
        m_last_window_ms = now_ms;
    }
}

} // namespace Inverter
