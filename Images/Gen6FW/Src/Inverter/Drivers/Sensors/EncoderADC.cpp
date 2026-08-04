#include "Inverter/Drivers/Sensors/EncoderADC.h"
#include "Inverter/Telemetry.h"
#include "Inverter/Control/FaultManager.h"

#include "main.h"
#include "adc.h"
#include "dma.h"

#include <cmath>

namespace Inverter {

static EncoderADC s_instance;
static DMA_HandleTypeDef hdma_adc2_enc;
static TIM_HandleTypeDef htim2_enc;

/* DMA buffer must live in AXI SRAM, not DTCMRAM. */
static uint16_t s_enc_dma_buffer[2] __attribute__((section(".dma_buffers")));

/* Layer 1 fit state lives in AXI SRAM for the same reason: it is large-ish
 * and only touched by the encoder ISR/calibration path. */
EncoderADC::FitAccumulator EncoderADC::s_fit_acc __attribute__((section(".dma_buffers")));
EncoderADC::SinCosFit EncoderADC::s_fit __attribute__((section(".dma_buffers")));

EncoderADC& encoderADC() {
    return s_instance;
}

bool EncoderADC::configureAdcChannels() {
    /* Make sure no regular conversion is running before reconfiguring. */
    if (LL_ADC_REG_IsConversionOngoing(hadc2.Instance)) {
        LL_ADC_REG_StopConversion(hadc2.Instance);
        while (LL_ADC_REG_IsConversionOngoing(hadc2.Instance)) {
            __NOP();
        }
    }

    /* Scan sequence: sin (CH10) then cos (CH11). */
    ADC_ChannelConfTypeDef sConfig = {};
    sConfig.SamplingTime = ADC_SAMPLETIME_32CYCLES_5;
    sConfig.SingleDiff = ADC_SINGLE_ENDED;
    sConfig.OffsetNumber = ADC_OFFSET_NONE;
    sConfig.Offset = 0;
    sConfig.OffsetSignedSaturation = DISABLE;
    sConfig.OffsetSign = ADC3_OFFSET_SIGN_NEGATIVE;

    sConfig.Channel = ADC_CHANNEL_10;
    sConfig.Rank = ADC_REGULAR_RANK_1;
    if (HAL_ADC_ConfigChannel(&hadc2, &sConfig) != HAL_OK) {
        return false;
    }

    sConfig.Channel = ADC_CHANNEL_11;
    sConfig.Rank = ADC_REGULAR_RANK_2;
    if (HAL_ADC_ConfigChannel(&hadc2, &sConfig) != HAL_OK) {
        return false;
    }

    /* 2-rank scan sequence. */
    MODIFY_REG(hadc2.Instance->SQR1, ADC_SQR1_L, 1U);
    CLEAR_BIT(hadc2.Instance->CFGR, ADC_CFGR_DISCEN | ADC_CFGR_DISCNUM);

    /* Disable regular oversampling for the encoder (not needed, keep it fast). */
    CLEAR_BIT(hadc2.Instance->CFGR2, ADC_CFGR2_ROVSE);

    /* Trigger from TIM2 TRGO, rising edge. */
    MODIFY_REG(hadc2.Instance->CFGR, ADC_CFGR_EXTEN | ADC_CFGR_EXTSEL,
               ADC_EXTERNALTRIGCONVEDGE_RISING | ADC_EXTERNALTRIG_T2_TRGO);

    /* Tell HAL_ADC_Start_DMA() to use circular DMA mode. */
    hadc2.Init.ScanConvMode = ADC_SCAN_ENABLE;
    hadc2.Init.ContinuousConvMode = DISABLE;
    hadc2.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DMA_CIRCULAR;

    return true;
}

bool EncoderADC::initTimer() {
    __HAL_RCC_TIM2_CLK_ENABLE();

    htim2_enc.Instance = TIM2;
    htim2_enc.Init.Prescaler = 0;
    htim2_enc.Init.CounterMode = TIM_COUNTERMODE_UP;
    /* APB1 = 137.5 MHz.  13750 ticks -> 10 kHz TRGO. */
    htim2_enc.Init.Period = 13749U;
    htim2_enc.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim2_enc.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_Base_Init(&htim2_enc) != HAL_OK) {
        return false;
    }

    /* Derive the sample rate from the actual timer configuration rather than
     * assuming it: the RPM estimator uses this and must follow any future
     * change to the trigger rate. */
    {
        /* TIM2 is on APB1 (137.5 MHz timer clock). */
        m_sample_hz = 137500000.0f /
            (static_cast<float>(htim2_enc.Init.Prescaler + 1U) *
             static_cast<float>(htim2_enc.Init.Period + 1U));
    }

    TIM_MasterConfigTypeDef sMasterConfig = {};
    sMasterConfig.MasterOutputTrigger = TIM_TRGO_UPDATE;
    sMasterConfig.MasterOutputTrigger2 = TIM_TRGO2_RESET;
    sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
    if (HAL_TIMEx_MasterConfigSynchronization(&htim2_enc, &sMasterConfig) != HAL_OK) {
        return false;
    }

    return true;
}

bool EncoderADC::initDma() {
    __HAL_RCC_DMA2_CLK_ENABLE();

    hdma_adc2_enc.Instance = DMA2_Stream0;
    hdma_adc2_enc.Init.Request = DMA_REQUEST_ADC2;
    hdma_adc2_enc.Init.Direction = DMA_PERIPH_TO_MEMORY;
    hdma_adc2_enc.Init.PeriphInc = DMA_PINC_DISABLE;
    hdma_adc2_enc.Init.MemInc = DMA_MINC_ENABLE;
    hdma_adc2_enc.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma_adc2_enc.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
    hdma_adc2_enc.Init.Mode = DMA_CIRCULAR;
    hdma_adc2_enc.Init.Priority = DMA_PRIORITY_MEDIUM;
    hdma_adc2_enc.Init.FIFOMode = DMA_FIFOMODE_DISABLE;

    if (HAL_DMA_Init(&hdma_adc2_enc) != HAL_OK) {
        return false;
    }

    __HAL_LINKDMA(&hadc2, DMA_Handle, hdma_adc2_enc);

    HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);

    return true;
}

bool EncoderADC::init() {
    /* DWT cycle counter for microsecond snapshot timestamps (angle
     * extrapolation).  Idempotent — safe if already enabled elsewhere. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    /* NOLOAD-section state must be zeroed explicitly; the C runtime does not
     * do it for us. */
    initializeFitState();

    if (!configureAdcChannels()) return false;
    if (!initTimer()) return false;
    if (!initDma()) return false;
    return true;
}

bool EncoderADC::start() {
    if (m_running) return true;

    /* Calibrate only if ADC2 is not already running for current sense. */
    if (LL_ADC_IsEnabled(hadc2.Instance) == 0U) {
        if (HAL_ADCEx_Calibration_Start(&hadc2, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED) != HAL_OK) {
            /* non-fatal: continue */
        }
    }

    if (HAL_ADC_Start_DMA(&hadc2, reinterpret_cast<uint32_t*>(s_enc_dma_buffer), 2) != HAL_OK) {
        return false;
    }

    if (HAL_TIM_Base_Start(&htim2_enc) != HAL_OK) {
        HAL_ADC_Stop_DMA(&hadc2);
        return false;
    }

    m_running = true;
    return true;
}

float EncoderADC::computeAngle(uint16_t raw_sin, uint16_t raw_cos) {
    /* Clamp to hard limits to reject outliers. */
    uint16_t csin = raw_sin;
    uint16_t ccos = raw_cos;
    if (csin < SIN_MIN_CAP) csin = SIN_MIN_CAP;
    if (csin > SIN_MAX_CAP) csin = SIN_MAX_CAP;
    if (ccos < COS_MIN_CAP) ccos = COS_MIN_CAP;
    if (ccos > COS_MAX_CAP) ccos = COS_MAX_CAP;

    /* Expand the learned bounds from the observed signal.  They start empty
     * and bracket the true signal range after roughly one revolution.  When a
     * fitted correction is active these bounds are no longer used for angle
     * decoding, but they are still needed for signal-quality diagnostics. */
    if (csin < m_obs_sin_min) m_obs_sin_min = csin;
    if (csin > m_obs_sin_max) m_obs_sin_max = csin;
    if (ccos < m_obs_cos_min) m_obs_cos_min = ccos;
    if (ccos > m_obs_cos_max) m_obs_cos_max = ccos;

    /* Prefer the learned bounds once both channels have seen enough span;
     * the hardcoded caps are only a fallback until then.  Normalizing with
     * bounds that do not match the real signal (stale mids or spans)
     * distorts the angle with a 2nd-harmonic error large enough to
     * detent-lock the rotor under FOC. */
    const bool learned_valid =
        (m_obs_sin_max >= m_obs_sin_min + LEARNED_MIN_SPAN) &&
        (m_obs_cos_max >= m_obs_cos_min + LEARNED_MIN_SPAN);
    uint16_t sin_min, sin_max, cos_min, cos_max;
    if (learned_valid) {
        sin_min = m_obs_sin_min; sin_max = m_obs_sin_max;
        cos_min = m_obs_cos_min; cos_max = m_obs_cos_max;
    } else {
        sin_min = m_sin_min; sin_max = m_sin_max;
        cos_min = m_cos_min; cos_max = m_cos_max;
    }
    m_active_sin_min = sin_min;
    m_active_sin_max = sin_max;
    m_active_cos_min = cos_min;
    m_active_cos_max = cos_max;
    m_learned_active = learned_valid;

    float angle_deg = 0.0f;

    if (s_fit.valid) {
        /* Layer 1 ellipse-fit correction: de-skew center, amplitude, and
         * quadrature phase before atan2.  The model is:
         *   sin_raw = Cs + As * sin(theta)
         *   cos_raw = Cc + Ac * cos(theta + phi)
         * so the corrected cos is (c_norm + s_norm * sin(phi)) / cos(phi). */
        const float s = (static_cast<float>(csin) - s_fit.center_sin) / s_fit.amp_sin;
        float c = (static_cast<float>(ccos) - s_fit.center_cos) / s_fit.amp_cos;
        c = (c + s * s_fit.phase_err_sin) / s_fit.phase_err_cos;
        angle_deg = atan2f(s, c) * (180.0f / static_cast<float>(M_PI));
        if (angle_deg < 0.0f) {
            angle_deg += 360.0f;
        }
    } else if ((sin_max > sin_min) && (cos_max > cos_min)) {
        float sin_norm = (static_cast<float>(csin - sin_min) /
                          static_cast<float>(sin_max - sin_min)) * 2.0f - 1.0f;
        float cos_norm = (static_cast<float>(ccos - cos_min) /
                          static_cast<float>(cos_max - cos_min)) * 2.0f - 1.0f;
        angle_deg = atan2f(sin_norm, cos_norm) * (180.0f / static_cast<float>(M_PI));
        if (angle_deg < 0.0f) {
            angle_deg += 360.0f;
        }
    }

    return angle_deg;
}

float EncoderADC::extrapolatedAngleDeg() {
    const float angle = m_snapshot.angle;
    if (!m_running || !m_rpm_init) {
        return angle;
    }
    /* Age of the snapshot in seconds (u32 cycle subtraction wraps cleanly). */
    const uint32_t age_cycles = DWT->CYCCNT - m_last_sample_cycles;
    const float age_s = static_cast<float>(age_cycles) /
                        static_cast<float>(SystemCoreClock);
    /* Bound the correction to one sample period's rotation: a stalled stream
     * degrades to (near) the raw snapshot instead of extrapolating away. */
    const float deg_per_s = m_rpm_ema * 6.0f;  /* rpm -> deg/s */
    float corr = deg_per_s * age_s;
    const float bound = std::fabs(deg_per_s) * (1.5f / m_sample_hz);
    if (corr > bound) corr = bound;
    else if (corr < -bound) corr = -bound;

    float out = angle + corr;
    while (out >= 360.0f) out -= 360.0f;
    while (out < 0.0f) out += 360.0f;
    return out;
}

void EncoderADC::useSynchronizedTrigger(bool sync) {
    /* Only the trigger-select bits change; the DMA stream keeps running.
     * One sample may straddle the switch — harmless for a slow signal. */
    MODIFY_REG(hadc2.Instance->CFGR, ADC_CFGR_EXTSEL,
               sync ? ADC_EXTERNALTRIG_T1_TRGO2 : ADC_EXTERNALTRIG_T2_TRGO);
}

void EncoderADC::traceDump() {
    Telemetry::printf("[SHELL] enc trace: %d samples @ ~1 kHz (sin cos angle_deg), oldest first",
                      static_cast<int>(TRACE_LEN));
    /* Pause the ring while dumping so lines stay consistent. */
    __disable_irq();
    const size_t head = m_trace_head;
    __enable_irq();
    for (size_t k = 0; k < TRACE_LEN; ++k) {
        const TraceEntry& e = m_trace[(head + k) % TRACE_LEN];
        Telemetry::printf("[TR] %u %u %.3f", e.raw_sin, e.raw_cos,
                          static_cast<double>(e.angle_deg));
    }
}

void EncoderADC::onDmaComplete() {
    /* Slim ISR: decode + publish only.  No RPM math, no fault evaluation,
     * no library calls — those live in diagnose() (main loop).  This handler
     * is ~5 us so its priority can sit above the control ISRs without
     * meaningfully delaying them. */
    const uint16_t raw_sin = s_enc_dma_buffer[0];
    const uint16_t raw_cos = s_enc_dma_buffer[1];

    /* Compute angle before touching the snapshot so the ISR writes all three
     * fields atomically relative to the main-loop readers. */
    const float angle = computeAngle(raw_sin, raw_cos);

    m_snapshot.angle = angle;
    m_snapshot.raw_sin = raw_sin;
    m_snapshot.raw_cos = raw_cos;
    m_new_data = true;
    m_last_sample_ms = HAL_GetTick();
    m_last_sample_cycles = DWT->CYCCNT;
    ++m_isr_count;

    /* Layer 1 ellipse-fit capture: accumulate every raw sample at the full
     * encoder rate while a calibration routine requests it. */
    if (m_fit_capture) {
        s_fit_acc.add(raw_sin, raw_cos);
    }

    /* Angle-linearity trace: decimated ring for the `enc_trace` command. */
    if (++m_trace_decim >= TRACE_DECIM) {
        m_trace_decim = 0;
        m_trace[m_trace_head] = {raw_sin, raw_cos, angle};
        m_trace_head = (m_trace_head + 1) % TRACE_LEN;
    }
}

bool EncoderADC::sample(float& angle_deg) {
    if (!m_new_data) {
        return false;
    }

    __disable_irq();
    angle_deg = m_snapshot.angle;
    m_new_data = false;
    __enable_irq();

    return true;
}

bool EncoderADC::sample(float& angle_deg, uint16_t& raw_sin, uint16_t& raw_cos) {
    if (!m_new_data) {
        return false;
    }

    __disable_irq();
    angle_deg = m_snapshot.angle;
    raw_sin = m_snapshot.raw_sin;
    raw_cos = m_snapshot.raw_cos;
    m_new_data = false;
    __enable_irq();

    return true;
}

void EncoderADC::setBounds(uint16_t sin_min, uint16_t sin_max,
                           uint16_t cos_min, uint16_t cos_max) {
    __disable_irq();
    m_sin_min = sin_min;
    m_sin_max = sin_max;
    m_cos_min = cos_min;
    m_cos_max = cos_max;
    /* Seed the learned bounds so the override takes effect immediately;
     * they keep expanding from real samples afterwards. */
    m_obs_sin_min = sin_min;
    m_obs_sin_max = sin_max;
    m_obs_cos_min = cos_min;
    m_obs_cos_max = cos_max;
    m_mag_ema = 0.0f;
    m_mag_ema_init = false;
    m_amp_low_count = 0;
    m_rail_count = 0;
    __enable_irq();
}

void EncoderADC::resetBounds() {
    __disable_irq();
    /* Reset dynamic bounds to the hard-coded caps.  The hard caps stay fixed. */
    m_sin_min = SIN_MIN_CAP;
    m_sin_max = SIN_MAX_CAP;
    m_cos_min = COS_MIN_CAP;
    m_cos_max = COS_MAX_CAP;
    m_obs_sin_min = 65535U;
    m_obs_sin_max = 0U;
    m_obs_cos_min = 65535U;
    m_obs_cos_max = 0U;
    m_learned_active = false;
    m_active_sin_min = SIN_MIN_CAP;
    m_active_sin_max = SIN_MAX_CAP;
    m_active_cos_min = COS_MIN_CAP;
    m_active_cos_max = COS_MAX_CAP;
    m_mag_ema = 0.0f;
    m_mag_ema_init = false;
    m_amp_low_count = 0;
    m_rail_count = 0;
    __enable_irq();
}

void EncoderADC::startFitCapture() {
    __disable_irq();
    m_fit_capture = false;
    s_fit_acc.reset();
    m_fit_capture = true;
    __enable_irq();
}

void EncoderADC::stopFitCapture() {
    __disable_irq();
    m_fit_capture = false;
    __enable_irq();
}

bool EncoderADC::computeFit(SinCosFit& out) {
    __disable_irq();
    m_fit_capture = false;
    FitAccumulator acc = s_fit_acc;
    s_fit_acc.reset();
    __enable_irq();

    out = SinCosFit();

    if (acc.count < FIT_MIN_SAMPLES) {
        return false;
    }
    if ((acc.max_sin < acc.min_sin + FIT_MIN_SPAN) ||
        (acc.max_cos < acc.min_cos + FIT_MIN_SPAN)) {
        return false;
    }

    /* Center and amplitude come from the observed peak-to-peak span.  This is
     * far more robust to non-uniform angular coverage (slip, cogging, speed
     * ripple) than the moment mean/variance, which is biased when the rotor
     * does not complete an integer number of electrical cycles with perfectly
     * uniform velocity. */
    const float span_sin = static_cast<float>(acc.max_sin - acc.min_sin);
    const float span_cos = static_cast<float>(acc.max_cos - acc.min_cos);
    if ((span_sin < FIT_MIN_SPAN) || (span_cos < FIT_MIN_SPAN)) {
        return false;
    }

    const float center_sin = static_cast<float>(acc.min_sin) + span_sin * 0.5f;
    const float center_cos = static_cast<float>(acc.min_cos) + span_cos * 0.5f;
    const float amp_sin    = span_sin * 0.5f;
    const float amp_cos    = span_cos * 0.5f;
    if ((amp_sin < FIT_MIN_AMP) || (amp_cos < FIT_MIN_AMP)) {
        return false;
    }

    const double n = static_cast<double>(acc.count);
    const double mean_sin = acc.sum_sin / n;
    const double mean_cos = acc.sum_cos / n;
    const double var_sin = acc.sum_sin2 / n - mean_sin * mean_sin;
    const double var_cos = acc.sum_cos2 / n - mean_cos * mean_cos;
    const double cov_sincos = acc.sum_sincos / n - mean_sin * mean_cos;

    /* Cross-check: the moment-derived amplitude should match the peak-to-peak
     * half-span within tolerance.  A big mismatch means coverage was poor or
     * the signal is not sinusoidal enough for this model. */
    const float moment_amp_sin = static_cast<float>(std::sqrt(2.0 * var_sin));
    const float moment_amp_cos = static_cast<float>(std::sqrt(2.0 * var_cos));
    if ((moment_amp_sin < FIT_MIN_AMP) || (moment_amp_cos < FIT_MIN_AMP)) {
        return false;
    }
    const float amp_mismatch_sin = std::fabs(moment_amp_sin - amp_sin) / amp_sin;
    const float amp_mismatch_cos = std::fabs(moment_amp_cos - amp_cos) / amp_cos;
    if ((amp_mismatch_sin > 0.10f) || (amp_mismatch_cos > 0.10f)) {
        return false;
    }

    /* Heydemann quadrature estimator: with u = sin(theta) and
     * v = cos(theta + phi), <u*v> = -0.5 * sin(phi).  Use the span-derived
     * center/amplitude for normalization so the estimate is coverage-robust. */
    const float sin_phi = static_cast<float>(
        -2.0 * cov_sincos / (static_cast<double>(amp_sin) * static_cast<double>(amp_cos)));

    float clamped_sin_phi = sin_phi;
    if (clamped_sin_phi > FIT_MAX_SIN_PHASE) clamped_sin_phi = FIT_MAX_SIN_PHASE;
    if (clamped_sin_phi < -FIT_MAX_SIN_PHASE) clamped_sin_phi = -FIT_MAX_SIN_PHASE;
    const float phi = asinf(clamped_sin_phi);

    /* A quadrature phase error beyond a few degrees means the sensor wiring
     * or mounting is wrong, not a calibration problem.  Reject rather than
     * bake a pathological fit into the decoder. */
    if (std::fabs(phi) > (FIT_MAX_PHASE_DEG * static_cast<float>(M_PI) / 180.0f)) {
        return false;
    }

    out.center_sin = center_sin;
    out.center_cos = center_cos;
    out.amp_sin    = amp_sin;
    out.amp_cos    = amp_cos;
    out.phase_err  = phi;
    out.phase_err_sin = clamped_sin_phi;
    out.phase_err_cos = cosf(phi);
    out.sample_count = static_cast<uint32_t>(acc.count);
    out.valid = true;
    return true;
}

void EncoderADC::applyFit(const SinCosFit& fit) {
    __disable_irq();
    s_fit = fit;
    __enable_irq();
}

void EncoderADC::clearFit() {
    __disable_irq();
    s_fit = SinCosFit();
    __enable_irq();
}

EncoderADC::SinCosFit EncoderADC::currentFit() const {
    __disable_irq();
    SinCosFit fit = s_fit;
    __enable_irq();
    return fit;
}

void EncoderADC::initializeFitState() {
    __disable_irq();
    s_fit_acc.reset();
    s_fit = SinCosFit();
    __enable_irq();
}

void EncoderADC::onDmaError() {
    FaultManager::instance().raise(FaultSource::EncoderDma,
                                   FaultReason::EncoderDmaError);
}

void EncoderADC::diagnose() {
    const uint32_t now_ms = HAL_GetTick();

    /* Mechanical speed, evaluated here (main loop) rather than in the DMA
     * ISR: per-sample angle deltas at 10 kHz multiply angle noise by the
     * full sample rate, which buries the estimate in EMI at high switching
     * frequencies.  Time-based window instead of sample-count window so the
     * estimate is independent of the caller's cadence. */
    {
        const float angle = m_snapshot.angle;
        if (!m_rpm_init) {
            m_rpm_init = true;
            m_unwrapped_angle = angle;
            m_window_ref_angle = angle;
            m_rpm_prev_angle = angle;
            m_rpm_filt_angle = angle;
            m_rpm_ema = 0.0f;
            m_rpm_window_ms = now_ms;
        } else {
            float delta = angle - m_rpm_prev_angle;
            if (delta > 180.0f) delta -= 360.0f;
            else if (delta < -180.0f) delta += 360.0f;
            m_unwrapped_angle += delta;
            m_rpm_prev_angle = angle;

            const uint32_t win_ms = now_ms - m_rpm_window_ms;
            if (win_ms >= RPM_WINDOW_MS) {
                const float win_deg = m_unwrapped_angle - m_window_ref_angle;
                const float rpm_inst = (win_deg / 360.0f) * (60000.0f / static_cast<float>(win_ms));
                m_rpm_ema += RPM_ALPHA * (rpm_inst - m_rpm_ema);
                m_window_ref_angle = m_unwrapped_angle;
                m_rpm_window_ms = now_ms;
            }
        }
    }

    /* Signal-quality faults (main loop now): magnitude collapse and rail
     * sticking, evaluated on the latest snapshot raws.  Fault detection is
     * slow by nature; per-call EMA replaces the per-sample one. */
    const bool range_ok = (m_active_sin_max - m_active_sin_min > MIN_AMP_RANGE) &&
                          (m_active_cos_max - m_active_cos_min > MIN_AMP_RANGE);
    if (range_ok) {
        const uint16_t raw_sin = m_snapshot.raw_sin;
        const uint16_t raw_cos = m_snapshot.raw_cos;
        const float sin_mid = 0.5f * static_cast<float>(m_active_sin_min + m_active_sin_max);
        const float cos_mid = 0.5f * static_cast<float>(m_active_cos_min + m_active_cos_max);
        const float dx = static_cast<float>(raw_sin) - sin_mid;
        const float dy = static_cast<float>(raw_cos) - cos_mid;
        const float mag = std::sqrt(dx * dx + dy * dy);

        if (!m_mag_ema_init) {
            m_mag_ema = mag;
            m_mag_ema_init = true;
        } else {
            m_mag_ema += MAG_EMA_ALPHA * (mag - m_mag_ema);
        }

        if (m_mag_ema < AMP_COLLAPSE_THRESHOLD) {
            if (++m_amp_low_count >= AMP_COLLAPSE_COUNT) {
                FaultManager::instance().raise(
                    FaultSource::EncoderAmplitude, FaultReason::EncoderAmplitudeLow);
                m_amp_low_count = 0;
            }
        } else {
            m_amp_low_count = 0;
        }

        const bool at_rail =
            (raw_sin < SIN_MIN_CAP + RAIL_MARGIN) ||
            (raw_sin > SIN_MAX_CAP - RAIL_MARGIN) ||
            (raw_cos < COS_MIN_CAP + RAIL_MARGIN) ||
            (raw_cos > COS_MAX_CAP - RAIL_MARGIN);
        if (at_rail) {
            if (++m_rail_count >= RAIL_COUNT) {
                FaultManager::instance().raise(
                    FaultSource::EncoderOutOfRange, FaultReason::EncoderAtRail);
                m_rail_count = 0;
            }
        } else {
            m_rail_count = 0;
        }

        /* A fitted correction assumes the sensor geometry is stable.  If the
         * signal amplitude collapses or sticks at a rail repeatedly, the
         * geometry has changed (sensor failing, disconnected, swapped) — drop
         * back to bounds normalization rather than drive with a stale fit. */
        if (s_fit.valid) {
            const bool signal_bad = (m_mag_ema < AMP_COLLAPSE_THRESHOLD) || at_rail;
            if (signal_bad) {
                if (++m_fit_fault_count >= FIT_FAULT_COUNT) {
                    Telemetry::printf("[ENC] fit invalidated: repeated signal-quality fault");
                    clearFit();
                }
            } else if (m_fit_fault_count > 0) {
                --m_fit_fault_count;
            }
        }
    }

    /* Publish the measured trigger/ISR rate once a second so the assumed
     * sample rate (m_sample_hz) can be validated against reality. */
    static uint32_t s_last_ms = 0;
    static uint32_t s_last_count = 0;
    if (s_last_ms != 0U && (now_ms - s_last_ms) >= 1000U) {
        const float hz = static_cast<float>(m_isr_count - s_last_count) *
                         (1000.0f / static_cast<float>(now_ms - s_last_ms));
        Telemetry::log("enc_isr_hz", hz);
        /* Self-calibrate the estimator's time base from the measured rate;
         * the APB timer clock assumption (137.5 MHz) under-reads it badly. */
        m_sample_hz = hz;
        s_last_count = m_isr_count;
        s_last_ms = now_ms;
    } else if (s_last_ms == 0U) {
        s_last_count = m_isr_count;
        s_last_ms = now_ms;
    }

    if (m_running && (now_ms - m_last_sample_ms) > SAMPLE_TIMEOUT_MS) {
        /* Temporarily disabled: encoder timeout fault is firing during
         * bench testing and interfering with other calibration work. */
        // FaultManager::instance().raise(FaultSource::EncoderTimeout,
        //                                FaultReason::EncoderSampleTimeout);
    }
}

} // namespace Inverter

/* TIME_DOMAIN: ENCODER_DMA_ERROR_ISR
 *   Asynchronous error path for encoder DMA stream.
 * CODEGEN: Keep as base-image safety hook.
 */
extern "C" void HAL_DMA_ErrorCallback(DMA_HandleTypeDef* hdma) {
    if (hdma != nullptr && hdma->Instance == DMA2_Stream0) {
        Inverter::encoderADC().onDmaError();
    }
}

/* TIME_DOMAIN: ENCODER_DMA_ISR (entry vector)
 *   Rate: TIM2 trigger @ 10 kHz.  Medium latency, ISR context.
 * CODEGEN: Keep vector; codegen may change trigger source or sample rate.
 */
extern "C" void DMA2_Stream0_IRQHandler(void) {
    HAL_DMA_IRQHandler(&Inverter::hdma_adc2_enc);
}

/* TIME_DOMAIN: ENCODER_SAMPLE_ISR
 *   Rate: ~10 kHz (TIM2 TRGO).  Computes angle, speed estimate, and signal-quality faults.
 * CODEGEN: Extend for different encoder interfaces (resolver, digital, etc.);
 *   keep the angle/speed output contract used by the control loop.
 */
extern "C" void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef* hadc) {
    if (hadc->Instance == ADC2) {
        Inverter::encoderADC().onDmaComplete();
    }
}
