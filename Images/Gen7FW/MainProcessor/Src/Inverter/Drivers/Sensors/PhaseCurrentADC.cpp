#include "Inverter/Drivers/Sensors/PhaseCurrentADC.h"
#include "Inverter/AppState.h"
#include "Inverter/LoopStats.h"
#include "Inverter/Drivers/PWM/pwm.h"
#include "Inverter/platform_api.h"
#include "Inverter/Calibration/EncoderCycleCalibrator.h"
#include "Inverter/Control/FaultManager.h"
#include "Inverter/Control/CurrentObserver.h"
#include "Inverter/Control/MotorParameterEstimator.h"
#include "Inverter/Control/VoltageVectorSchedule.h"
#include "Inverter/Drivers/Sensors/EncoderADC.h"
#include "Inverter/Drivers/Sensors/PoleEstimator.h"
#include "Inverter/Drivers/Sensors/SpikeRecorder.h"
#include "Inverter/Drivers/Storage/RteParamStore.h"
#include "Inverter/Calibration/MotorCalibration.h"
#include "Inverter/Telemetry.h"

#include "main.h"
#include "adc.h"
#include "tim.h"

#include "../../../generated/domain_tim_isr_generated.h"

#include <cstdio>
#include <cmath>

namespace Inverter {

static PhaseCurrentADC s_instance;

PhaseCurrentADC& phaseCurrentADC() {
    return s_instance;
}

/**
 * @brief Remap measured physical U/V currents to the logical UVW frame
 *        requested by the control algorithm, accounting for Motor.PhaseSwap.
 *
 * W is reconstructed from U+V by the caller; this helper only produces the
 * logical U and V that preserve the zero-sequence constraint.
 */
static void applyPhaseSwap(float iu_phys_a, float iv_phys_a,
                           float& iu_log_a, float& iv_log_a) {
    switch (motorCalibration().phase_swap) {
        case PhaseSwap::SwapUV:
            iu_log_a = iv_phys_a;
            iv_log_a = iu_phys_a;
            break;
        case PhaseSwap::SwapVW:
            iu_log_a = iu_phys_a;
            iv_log_a = -(iu_phys_a + iv_phys_a);
            break;
        case PhaseSwap::CurrentSwapUW:
            /* Gen7 sensor harness: the transducer on the physical V wire feeds
             * the ADC "U" channel (iu_phys) and the transducer on the W wire
             * feeds the ADC "V" channel (iv_phys); the U wire is unsensed.
             * Verified by open-loop rotation: without this, the measured
             * current vector rotates BACKWARD vs the applied voltage vector
             * (mirrored Clarke) and FOC goes unstable. */
            iu_log_a = -(iu_phys_a + iv_phys_a);
            iv_log_a = iu_phys_a;
            break;
        case PhaseSwap::SwapUW:
            iu_log_a = -(iu_phys_a + iv_phys_a);
            iv_log_a = iv_phys_a;
            break;
        default:
            iu_log_a = iu_phys_a;
            iv_log_a = iv_phys_a;
            break;
    }
}

static ADC_InjectionConfTypeDef makeInjectedConfig(uint32_t channel, uint32_t rank,
                                                    uint32_t trigger, uint32_t edge,
                                                    uint32_t nbr_of_conv = 4U) {
    ADC_InjectionConfTypeDef cfg = {};
    cfg.InjectedChannel = channel;
    cfg.InjectedRank = rank;
    cfg.InjectedSamplingTime = ADC_SAMPLETIME_8CYCLES_5;
    cfg.InjectedSingleDiff = ADC_SINGLE_ENDED;
    cfg.InjectedOffsetNumber = ADC_OFFSET_NONE;
    cfg.InjectedOffset = 0;
    cfg.InjectedOffsetSignedSaturation = DISABLE;
    cfg.InjectedNbrOfConversion = nbr_of_conv;
    cfg.InjectedDiscontinuousConvMode = DISABLE;
    cfg.AutoInjectedConv = DISABLE;
    cfg.QueueInjectedContext = DISABLE;
    cfg.ExternalTrigInjecConv = trigger;
    cfg.ExternalTrigInjecConvEdge = edge;
    /* Hardware oversampling is disabled so the CPU sees individual raw
     * samples in JDR1..4.  Oversampling would hide the local di/dt needed by
     * the current observer and RLS estimator. */
    cfg.InjecOversamplingMode = DISABLE;
    cfg.InjecOversampling.Ratio = 1;
    cfg.InjecOversampling.RightBitShift = ADC_RIGHTBITSHIFT_NONE;
    return cfg;
}

bool PhaseCurrentADC::configureAdcChannels() {
    HAL_ADC_Stop(&hadc1);
    HAL_ADC_Stop(&hadc2);

    /* Scan mode must be enabled for the injected sequencer to use ranks 1 and 2. */
    hadc1.Init.ScanConvMode = ADC_SCAN_ENABLE;
    hadc2.Init.ScanConvMode = ADC_SCAN_ENABLE;

    /* We want one interrupt per completed injected sequence (both ranks). */
    hadc1.Init.EOCSelection = ADC_EOC_SEQ_CONV;
    hadc2.Init.EOCSelection = ADC_EOC_SEQ_CONV;

    /* Disable injected context queue so software-trigger (slave) start works. */
    if (HAL_ADCEx_DisableInjectedQueue(&hadc1) != HAL_OK) {
        return false;
    }
    if (HAL_ADCEx_DisableInjectedQueue(&hadc2) != HAL_OK) {
        return false;
    }

    /* ADC1 injected master: [U_sig, V_sig, U_sig, V_sig], triggered at both TIM1_TRGO2 update extrema during control.
     * The reference channels are on ADC2 so each signal/reference pair is
     * sampled simultaneously (true differential measurement).  The 4-rank
     * sequence captures a two-point micro-burst for both phases. */
    ADC_InjectionConfTypeDef inj1_r1 = makeInjectedConfig(
        ADC_CHANNEL_4, ADC_INJECTED_RANK_1,
        ADC_EXTERNALTRIGINJEC_T1_TRGO2, ADC_EXTERNALTRIGINJECCONV_EDGE_RISING);
    ADC_InjectionConfTypeDef inj1_r2 = makeInjectedConfig(
        ADC_CHANNEL_3, ADC_INJECTED_RANK_2,
        ADC_EXTERNALTRIGINJEC_T1_TRGO2, ADC_EXTERNALTRIGINJECCONV_EDGE_RISING);
    ADC_InjectionConfTypeDef inj1_r3 = makeInjectedConfig(
        ADC_CHANNEL_4, ADC_INJECTED_RANK_3,
        ADC_EXTERNALTRIGINJEC_T1_TRGO2, ADC_EXTERNALTRIGINJECCONV_EDGE_RISING);
    ADC_InjectionConfTypeDef inj1_r4 = makeInjectedConfig(
        ADC_CHANNEL_3, ADC_INJECTED_RANK_4,
        ADC_EXTERNALTRIGINJEC_T1_TRGO2, ADC_EXTERNALTRIGINJECCONV_EDGE_RISING);

    if (HAL_ADCEx_InjectedConfigChannel(&hadc1, &inj1_r1) != HAL_OK) {
        return false;
    }
    if (HAL_ADCEx_InjectedConfigChannel(&hadc1, &inj1_r2) != HAL_OK) {
        return false;
    }
    if (HAL_ADCEx_InjectedConfigChannel(&hadc1, &inj1_r3) != HAL_OK) {
        return false;
    }
    if (HAL_ADCEx_InjectedConfigChannel(&hadc1, &inj1_r4) != HAL_OK) {
        return false;
    }

    /* ADC2 injected slave: [U_ref, V_ref, U_ref, V_ref], no external trigger.
     * It is hardware-slaved to ADC1 in injected-simultaneous mode. */
    ADC_InjectionConfTypeDef inj2_r1 = makeInjectedConfig(
        ADC_CHANNEL_8, ADC_INJECTED_RANK_1,
        ADC_INJECTED_SOFTWARE_START, ADC_EXTERNALTRIGINJECCONV_EDGE_NONE);
    ADC_InjectionConfTypeDef inj2_r2 = makeInjectedConfig(
        ADC_CHANNEL_7, ADC_INJECTED_RANK_2,
        ADC_INJECTED_SOFTWARE_START, ADC_EXTERNALTRIGINJECCONV_EDGE_NONE);
    ADC_InjectionConfTypeDef inj2_r3 = makeInjectedConfig(
        ADC_CHANNEL_8, ADC_INJECTED_RANK_3,
        ADC_INJECTED_SOFTWARE_START, ADC_EXTERNALTRIGINJECCONV_EDGE_NONE);
    ADC_InjectionConfTypeDef inj2_r4 = makeInjectedConfig(
        ADC_CHANNEL_7, ADC_INJECTED_RANK_4,
        ADC_INJECTED_SOFTWARE_START, ADC_EXTERNALTRIGINJECCONV_EDGE_NONE);

    if (HAL_ADCEx_InjectedConfigChannel(&hadc2, &inj2_r1) != HAL_OK) {
        return false;
    }
    if (HAL_ADCEx_InjectedConfigChannel(&hadc2, &inj2_r2) != HAL_OK) {
        return false;
    }
    if (HAL_ADCEx_InjectedConfigChannel(&hadc2, &inj2_r3) != HAL_OK) {
        return false;
    }
    if (HAL_ADCEx_InjectedConfigChannel(&hadc2, &inj2_r4) != HAL_OK) {
        return false;
    }

    /* Use injected-simultaneous dual mode only.  Regular groups remain independent,
     * so ADC2 regular can run the encoder DMA. */
    ADC_MultiModeTypeDef multimode = {};
    multimode.Mode = ADC_DUALMODE_INJECSIMULT;
    multimode.DualModeData = ADC_DUALMODEDATAFORMAT_DISABLED;
    multimode.TwoSamplingDelay = ADC_TWOSAMPLINGDELAY_5CYCLES;
    if (HAL_ADCEx_MultiModeConfigChannel(&hadc1, &multimode) != HAL_OK) {
        return false;
    }

    return true;
}

bool PhaseCurrentADC::initTrigger() {
    /* Keep channel 4 / TRGO available for the legacy sampling scheduler.
     * Current acquisition now uses TRGO2 update events: both zero-vector
     * midpoints during graph control, with ADC2 injected following ADC1. */
    static constexpr uint32_t PULSE_TICKS = 10U;

    TIM_OC_InitTypeDef sConfigOC = {};
    sConfigOC.OCMode = TIM_OCMODE_PWM1;
    sConfigOC.Pulse = PULSE_TICKS;
    sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
    sConfigOC.OCNPolarity = TIM_OCNPOLARITY_HIGH;
    sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
    sConfigOC.OCIdleState = TIM_OCIDLESTATE_RESET;
    sConfigOC.OCNIdleState = TIM_OCNIDLESTATE_RESET;
    if (HAL_TIM_OC_ConfigChannel(&htim1, &sConfigOC, TIM_CHANNEL_4) != HAL_OK) {
        return false;
    }

    MODIFY_REG(htim1.Instance->CR2, TIM_CR2_MMS, TIM_TRGO_OC4REF);

    /* TRGO2 = update event: triggers the encoder's ADC2 regular scan so the
     * angle stream is synchronous with the control timebase while running
     * (ControlSupervisor switches the encoder between TIM2 at idle and
     * TIM1 TRGO2 in control). */
    MODIFY_REG(htim1.Instance->CR2, TIM_CR2_MMS2, TIM_TRGO2_UPDATE);
    return true;
}

bool PhaseCurrentADC::init() {
    /* DWT cycle counter for microsecond burst timestamps.  Idempotent — safe
     * if already enabled elsewhere. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    if (!configureAdcChannels()) return false;
    if (!initTrigger()) return false;
    if (!configureAnalogWatchdog()) return false;

    HAL_NVIC_SetPriority(ADC_IRQn, 4, 0);
    HAL_NVIC_EnableIRQ(ADC_IRQn);

    return true;
}

bool PhaseCurrentADC::start() {
    if (m_running) return true;

    if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED) != HAL_OK) {
        /* non-fatal: continue */
    }
    if (HAL_ADCEx_Calibration_Start(&hadc2, ADC_CALIB_OFFSET, ADC_SINGLE_ENDED) != HAL_OK) {
        /* non-fatal: continue */
    }

    /* Start injected conversions: slave first, then master. */
    if (HAL_ADCEx_InjectedStart_IT(&hadc2) != HAL_OK) {
        return false;
    }
    if (HAL_ADCEx_InjectedStart_IT(&hadc1) != HAL_OK) {
        HAL_ADCEx_InjectedStop_IT(&hadc2);
        return false;
    }

    /* Start the TIM1 counter so its TRGO events trigger the injected group.
     * This does not enable PWM outputs; it just runs the timer. */
    if (HAL_TIM_Base_Start(&htim1) != HAL_OK) {
        HAL_ADCEx_InjectedStop_IT(&hadc1);
        HAL_ADCEx_InjectedStop_IT(&hadc2);
        return false;
    }

    /* Sanity check: the TIM1 counter must be moving. */
    const uint32_t cnt_before = htim1.Instance->CNT;
    for (volatile uint32_t i = 0; i < 10000U; ++i) {
        __NOP();
    }
    if (htim1.Instance->CNT == cnt_before) {
        HAL_TIM_Base_Stop(&htim1);
        HAL_ADCEx_InjectedStop_IT(&hadc1);
        HAL_ADCEx_InjectedStop_IT(&hadc2);
        return false;
    }

    if (!calibrateOffsets()) {
        HAL_TIM_Base_Stop(&htim1);
        HAL_ADCEx_InjectedStop_IT(&hadc1);
        HAL_ADCEx_InjectedStop_IT(&hadc2);
        return false;
    }

    m_running = true;
    Telemetry::printf("[CUR] start cal done U=%.3f V=%.3f",
                      static_cast<double>(m_offset_u),
                      static_cast<double>(m_offset_v));
    return true;
}

bool PhaseCurrentADC::stop() {
    if (!m_running) return true;
    HAL_TIM_Base_Stop(&htim1);
    HAL_ADCEx_InjectedStop_IT(&hadc1);
    HAL_ADCEx_InjectedStop_IT(&hadc2);
    m_running = false;
    return true;
}

uint32_t PhaseCurrentADC::awdHalfWindowCounts() const {
    /* Raw ADC codes per amp: the LA37S600 output is ratiometric around its
     * reference at 0 A. */
    constexpr float COUNTS_FULL = static_cast<float>((1U << ADC_BITS) - 1U);
    constexpr float COUNTS_PER_AMP = (DIVIDER * SENSITIVITY_VA * COUNTS_FULL) / ADC_VREF;
    return static_cast<uint32_t>(m_hw_oc_threshold_a * COUNTS_PER_AMP);
}

void PhaseCurrentADC::awdWindowFromRefs(uint32_t rmin, uint32_t rmax,
                                        uint32_t& low, uint32_t& high) const {
    /* The hardware watchdog compares ABSOLUTE codes with one window for both
     * phase channels, but each LA37S600 rides its own isolated reference and
     * the references bounce by up to ~200 counts (~14 A equivalent) at idle
     * on this bench.  Frame the window on the reference extremes so each
     * channel's sig stays inside as long as its differential sig-ref
     * excursion is below the requested threshold.  AWD_TRACK_GUARD absorbs
     * single-sample differential glitches (measured up to ~115 counts =
     * ~8 A at idle with the bridge off). */
    constexpr uint32_t COUNTS_FULL = (1U << ADC_BITS) - 1U;

    if (rmin == 0U || rmax == 0U) {
        /* Stream not live yet: center on the ideal mid-scale code. */
        rmin = rmax = COUNTS_FULL / 2U;
    }

    const uint32_t pad = awdHalfWindowCounts() + AWD_TRACK_GUARD;
    low  = (rmin > pad) ? rmin - pad : 0U;
    high = (rmax + pad > COUNTS_FULL) ? COUNTS_FULL : rmax + pad;
}

void PhaseCurrentADC::writeAwdWindow(uint32_t low, uint32_t high) {
    /* Widen first: the intermediate window is then a superset of both the old
     * and the new window, so a conversion that compares against the registers
     * mid-update can never see a window that excludes a value both the old
     * and the new window would accept. */
    const uint32_t cur_low  = READ_BIT(hadc1.Instance->LTR1_TR1, ADC_LTR_LT);
    const uint32_t cur_high = READ_BIT(hadc1.Instance->HTR1_TR2, ADC_HTR_HT);
    if (high > cur_high) {
        MODIFY_REG(hadc1.Instance->HTR1_TR2, ADC_HTR_HT, high);
    }
    if (low < cur_low) {
        MODIFY_REG(hadc1.Instance->LTR1_TR1, ADC_LTR_LT, low);
    }
    MODIFY_REG(hadc1.Instance->LTR1_TR1, ADC_LTR_LT, low);
    MODIFY_REG(hadc1.Instance->HTR1_TR2, ADC_HTR_HT, high);
}

bool PhaseCurrentADC::configureAnalogWatchdog() {
    /* A threshold of 0 maps to the full-range window, which can never fire:
     * AWD1 stays armed and "disabled" is purely a threshold choice (so
     * runtime changes only ever rewrite the threshold registers).
     *
     * The window must track the SAMPLED reference codes, not the ideal
     * VREF/2 mid-scale: the isolated sensor supplies bounce the absolute
     * codes by up to +/-200 counts (~+/-14 A equivalent) at idle on this
     * bench while the differential sig-ref stays within +/-5 A.  Any fixed
     * absolute window narrow enough to trip at a real 8 A false-trips on
     * that bounce.  The armed window is therefore framed from the live
     * per-burst reference extremes and re-tracked in the ISR (see
     * onInjectedConversionComplete). */
    constexpr uint32_t COUNTS_FULL = (1U << ADC_BITS) - 1U;

    uint32_t low = 0U, high = COUNTS_FULL;
    if (m_hw_oc_threshold_a > 0.0f) {
        uint32_t rmin = m_raw_burst_u_ref[0], rmax = rmin;
        const uint32_t refs[3] = {m_raw_burst_v_ref[0], m_raw_burst_u_ref[1],
                                  m_raw_burst_v_ref[1]};
        for (uint32_t r : refs) {
            if (r < rmin) rmin = r;
            if (r > rmax) rmax = r;
        }
        awdWindowFromRefs(rmin, rmax, low, high);
        m_ref_floor = rmin;
        m_ref_ceil  = rmax;
    }

    if (!m_running) {
        /* Init-time full config: conversions are stopped, so the HAL may
         * touch the mode/channel-select bits (it refuses while any
         * conversion is ongoing). */
        ADC_AnalogWDGConfTypeDef awd = {};
        awd.WatchdogNumber = ADC_ANALOGWATCHDOG_1;
        awd.WatchdogMode   = ADC_ANALOGWATCHDOG_ALL_INJEC;
        awd.ITMode         = ENABLE;
        awd.HighThreshold  = high;
        awd.LowThreshold   = low;
        return HAL_ADC_AnalogWDGConfig(&hadc1, &awd) == HAL_OK;
    }

    /* Runtime: the TIM1/ADC streams run permanently on Gen7, and stopping
     * them to satisfy the HAL guard corrupts the dual injected-simultaneous
     * acquisition (measured on the bench: railed currents, encoder
     * out-of-range).  The AWD1 thresholds are plain data registers
     * (LTR1_TR1/HTR1_TR2, 16-bit resolution = no shift), written directly
     * while conversions run; the caller (hwocset) guarantees the power stage
     * is not actuating. */
    m_awd_armed = false;
    writeAwdWindow(low, high);
    if (m_hw_oc_threshold_a > 0.0f) {
        m_awd_low   = low;
        m_awd_high  = high;
        m_awd_armed = true;
    }

    return READ_BIT(hadc1.Instance->LTR1_TR1, ADC_LTR_LT) == low &&
           READ_BIT(hadc1.Instance->HTR1_TR2, ADC_HTR_HT) == high;
}

bool PhaseCurrentADC::setHardwareOvercurrentThreshold(float amps) {
    /* The 600 A IGBT module must not be left without the independent ADC
     * window by a shell command or an invalid calibration. Lower thresholds
     * are allowed for a more restrictive power stage. */
    if (!std::isfinite(amps) || amps <= 0.0f || amps > 500.0f) return false;

    /* The ADC watchdog is a safety-critical window: changing it while the
     * power stage is actuating could create a glitch or a blind spot.  The
     * TIM1/ADC ISR runs permanently for measurement, so "running" here no
     * longer implies actuation; the hwocset command refuses while the power
     * stage is active. */
    m_hw_oc_threshold_a = amps;
    return configureAnalogWatchdog();
}

bool PhaseCurrentADC::recalibrateOffsets() {
    if (!m_running) {
        return false;
    }
    const bool ok = calibrateOffsets();
    if (ok) {
        Telemetry::printf("[CUR] recal done U=%.3f V=%.3f",
                          static_cast<double>(m_offset_u),
                          static_cast<double>(m_offset_v));
    }
    return ok;
}

void PhaseCurrentADC::setUseFixedReference(bool use_fixed) {
    if (use_fixed && !m_use_fixed_ref) {
        /* Capture the current reference values when enabling fixed mode. */
        m_fixed_ref_u = m_raw_u_ref;
        m_fixed_ref_v = m_raw_v_ref;
    }
    m_use_fixed_ref = use_fixed;
}

float PhaseCurrentADC::countsToCurrent(uint32_t sig, uint32_t ref) const {
    const float lsb   = ADC_VREF / static_cast<float>((1U << ADC_BITS) - 1U);
    const float scale = lsb / (DIVIDER * SENSITIVITY_VA);
    if (m_use_fixed_ref) {
        /* Use the captured reference for this phase.  The caller passes the
         * sampled ref; we match it to the captured fixed ref by proximity. */
        const float sampled = static_cast<float>(ref);
        const float fixed_u = static_cast<float>(m_fixed_ref_u);
        const float fixed_v = static_cast<float>(m_fixed_ref_v);
        const float fixed = (std::fabs(sampled - fixed_u) < std::fabs(sampled - fixed_v))
                                ? fixed_u : fixed_v;
        return (static_cast<float>(sig) - fixed) * scale;
    }
    return (static_cast<float>(sig) - static_cast<float>(ref)) * scale;
}

bool PhaseCurrentADC::calibrateOffsets() {
    constexpr uint32_t DISCARD_SAMPLES = 500;
    constexpr uint32_t AVG_SAMPLES     = 1000;

    m_new_data = false;
    for (uint32_t i = 0; i < DISCARD_SAMPLES; ++i) {
        while (!m_new_data) {
            __NOP();
        }
        m_new_data = false;
    }

    float sum_u = 0.0f;
    float sum_v = 0.0f;
    for (uint32_t i = 0; i < AVG_SAMPLES; ++i) {
        while (!m_new_data) {
            __NOP();
        }
        sum_u += m_iu;
        sum_v += m_iv;
        m_new_data = false;
    }

    m_offset_u = sum_u / static_cast<float>(AVG_SAMPLES);
    m_offset_v = sum_v / static_cast<float>(AVG_SAMPLES);

    /* Sanity-check the captured zero-current offset.  A residual this large
     * means the sensor/reference was still drifting during the capture. */
    constexpr float MAX_SANE_OFFSET_A = 50.0f;
    m_offset_valid = (std::fabs(m_offset_u) < MAX_SANE_OFFSET_A) &&
                     (std::fabs(m_offset_v) < MAX_SANE_OFFSET_A);
    if (!m_offset_valid) {
        Telemetry::printf("[CUR] WARNING: bad offset U=%.2f V=%.2f A; sensor not settled",
                          static_cast<double>(m_offset_u),
                          static_cast<double>(m_offset_v));
    }

    /* The synchronous output values are recomputed each ISR from (m_iu - offset),
     * so no separate filter state needs to be reset here. */
    return m_offset_valid;
}

void PhaseCurrentADC::onInjectedConversionComplete() {
    platform_current_sample_begin(DWT->CYCCNT);
    ++LoopStats::adc_isr;
    /* Read the 4-rank micro-burst:
     *   ADC1 ranks: U_sig, V_sig, U_sig, V_sig
     *   ADC2 ranks: U_ref, V_ref, U_ref, V_ref
     * ADC1/ADC2 run simultaneously rank-by-rank in dual injected mode. */
    m_raw_burst_u_sig[0] = HAL_ADCEx_InjectedGetValue(&hadc1, ADC_INJECTED_RANK_1);
    m_raw_burst_v_sig[0] = HAL_ADCEx_InjectedGetValue(&hadc1, ADC_INJECTED_RANK_2);
    m_raw_burst_u_sig[1] = HAL_ADCEx_InjectedGetValue(&hadc1, ADC_INJECTED_RANK_3);
    m_raw_burst_v_sig[1] = HAL_ADCEx_InjectedGetValue(&hadc1, ADC_INJECTED_RANK_4);

    m_raw_burst_u_ref[0] = HAL_ADCEx_InjectedGetValue(&hadc2, ADC_INJECTED_RANK_1);
    m_raw_burst_v_ref[0] = HAL_ADCEx_InjectedGetValue(&hadc2, ADC_INJECTED_RANK_2);
    m_raw_burst_u_ref[1] = HAL_ADCEx_InjectedGetValue(&hadc2, ADC_INJECTED_RANK_3);
    m_raw_burst_v_ref[1] = HAL_ADCEx_InjectedGetValue(&hadc2, ADC_INJECTED_RANK_4);

    m_last_burst_us = DWT->CYCCNT / (SystemCoreClock / 1000000U);

    /* Keep the armed hardware watchdog window framed on the reference
     * extremes with a leak tracker: the floor/ceiling snap to new reference
     * extremes instantly and relax one count per burst, so the window hugs
     * the rolling reference envelope without per-burst lag (the isolated
     * sensor supplies bounce the absolute codes common-mode far beyond any
     * usable overcurrent half-window; a fixed or one-burst-lagged window
     * false-trips at idle).  A genuine overcurrent moves sig away from its
     * ref and still trips.  Deadband keeps the register write rate low;
     * widen-first ordering in writeAwdWindow() makes updates safe mid-stream. */
    if (m_awd_armed) {
        uint32_t rmin = m_raw_burst_u_ref[0], rmax = rmin;
        const uint32_t refs[3] = {m_raw_burst_v_ref[0], m_raw_burst_u_ref[1],
                                  m_raw_burst_v_ref[1]};
        for (uint32_t r : refs) {
            if (r < rmin) rmin = r;
            if (r > rmax) rmax = r;
        }
        if (rmin != 0U && rmax != 0U) {
            uint32_t floor = m_ref_floor;
            uint32_t ceil  = m_ref_ceil;
            floor = (rmin < floor) ? rmin : floor + 1U;
            ceil  = (rmax > ceil) ? rmax : ceil - 1U;
            m_ref_floor = floor;
            m_ref_ceil  = ceil;

            uint32_t low = 0U, high = 0U;
            awdWindowFromRefs(floor, ceil, low, high);
            int32_t dl = static_cast<int32_t>(low) - static_cast<int32_t>(m_awd_low);
            int32_t dh = static_cast<int32_t>(high) - static_cast<int32_t>(m_awd_high);
            if (dl < 0) dl = -dl;
            if (dh < 0) dh = -dh;
            if (dl > static_cast<int32_t>(AWD_TRACK_DEADBAND) ||
                dh > static_cast<int32_t>(AWD_TRACK_DEADBAND)) {
                m_awd_low  = low;
                m_awd_high = high;
                writeAwdWindow(low, high);
            }
        }
    }

    /* Keep the single-point legacy interface working: use the first burst
     * point as the canonical raw/current values. */
    m_raw_u_sig = m_raw_burst_u_sig[0];
    m_raw_v_sig = m_raw_burst_v_sig[0];
    m_raw_u_ref = m_raw_burst_u_ref[0];
    m_raw_v_ref = m_raw_burst_v_ref[0];

    m_iu = countsToCurrent(m_raw_u_sig, m_raw_u_ref);
    m_iv = countsToCurrent(m_raw_v_sig, m_raw_v_ref);

    const float iu_phys = m_iu - m_offset_u;
    const float iv_phys = m_iv - m_offset_v;
    float iu_log = 0.0f, iv_log = 0.0f;
    applyPhaseSwap(iu_phys, iv_phys, iu_log, iv_log);
    m_current_u = iu_log;
    m_current_v = iv_log;

    /* The generated adc_isr domain (hw.phase_currents) handles observer
     * correction and RLS estimation from the micro-burst.  The base image
     * only computes the legacy single-point currents for compatibility. */

    /* Spike event recorder: synchronized raw currents + encoder snapshot for
     * glitch forensics (see `spikes` shell command). */
    spikeRecorder().onSample(HAL_GetTick(),
                             static_cast<uint16_t>(m_raw_u_sig),
                             static_cast<uint16_t>(m_raw_v_sig),
                             static_cast<uint16_t>(m_raw_u_ref),
                             static_cast<uint16_t>(m_raw_v_ref),
                             m_current_u, m_current_v,
                             encoderADC().extrapolatedAngleDeg(),
                             static_cast<uint16_t>(encoderADC().lastRawSin()),
                             static_cast<uint16_t>(encoderADC().lastRawCos()),
                             appState.tim_isr.Svpwm.Duty_A,
                             appState.tim_isr.Svpwm.Duty_B,
                             appState.tim_isr.Svpwm.Duty_C);

    /* Software overcurrent protection.  Requires several consecutive
     * samples over threshold: at high bus voltage a single switching
     * transient can glitch one sample far beyond any real current, and a
     * one-sample trip would false-fault the whole drive. */
    if (m_oc_threshold_a > 0.0f) {
        if (std::fabs(m_current_u) > m_oc_threshold_a ||
            std::fabs(m_current_v) > m_oc_threshold_a) {
            if (++m_oc_count >= OC_CONSEC_SAMPLES) {
                m_oc_count = 0;
                FaultManager::instance().raise(FaultSource::PhaseOvercurrent,
                                               FaultReason::PhaseOvercurrentSoftware);
            }
        } else {
            m_oc_count = 0;
        }
    }

    /* Feed the pole estimator at the ADC sample rate.  Pass raw encoder
     * sin/cos so the estimate does not depend on the encoder angle bounds. */
    PoleEstimator::instance().onSample(
        m_current_u, encoderADC().lastRawSin(), encoderADC().lastRawCos());

    /* Feed the encoder-cycle counter (used by motor profiling) at the same
     * rate. */
    EncoderCycleCalibrator::instance().onSample(encoderADC().lastRawSin(),
                                                encoderADC().lastRawCos());

    m_new_data = true;

    /* Publish this completed conversion before the generated graph reads it.
     * Calling the graph above the JDR reads made latestBurst() return the
     * previous PWM period's currents, adding a full sample of feedback delay.
     * Keep base-image overcurrent protection ahead of graph execution. */
    const float interrupted_domain_dt = platform_get_current_domain_dt();
    platform_set_current_domain_dt(1.0f / PWM_GetUpdateFrequency());
    // RTE_EMIT: adc_isr step
    /* ADC IRQ can preempt TIM1: do not change the interrupted PI time step. */
    platform_set_current_domain_dt(interrupted_domain_dt);
}

bool PhaseCurrentADC::sample(float& iu, float& iv, float& iw) {
    if (!m_new_data) {
        return false;
    }

    __disable_irq();
    iu = m_current_u;
    iv = m_current_v;
    m_new_data = false;
    __enable_irq();

    iw = -(iu + iv);
    return true;
}

bool PhaseCurrentADC::latest(float& iu, float& iv, float& iw) const {
    if (!m_running) {
        return false;
    }

    __disable_irq();
    iu = m_current_u;
    iv = m_current_v;
    __enable_irq();

    iw = -(iu + iv);
    return true;
}

bool PhaseCurrentADC::sampleBurst(BurstSample& out) {
    if (!m_new_data) {
        return false;
    }

    __disable_irq();
    const float iu0_phys = countsToCurrent(m_raw_burst_u_sig[0], m_raw_burst_u_ref[0]) - m_offset_u;
    const float iv0_phys = countsToCurrent(m_raw_burst_v_sig[0], m_raw_burst_v_ref[0]) - m_offset_v;
    const float iu1_phys = countsToCurrent(m_raw_burst_u_sig[1], m_raw_burst_u_ref[1]) - m_offset_u;
    const float iv1_phys = countsToCurrent(m_raw_burst_v_sig[1], m_raw_burst_v_ref[1]) - m_offset_v;
    applyPhaseSwap(iu0_phys, iv0_phys, out.point[0].iu_a, out.point[0].iv_a);
    applyPhaseSwap(iu1_phys, iv1_phys, out.point[1].iu_a, out.point[1].iv_a);
    /* The two points in one burst are one rank apart; at 48 MHz ADC clock
     * with 8.5-cycle sampling this is roughly 0.46 us.  We store a nominal
     * midpoint timestamp for both points. */
    out.point[0].time_us = m_last_burst_us;
    out.point[1].time_us = m_last_burst_us;
    out.valid = true;
    m_new_data = false;
    __enable_irq();

    return true;
}

bool PhaseCurrentADC::latestBurst(BurstSample& out) const {
    if (!m_running) {
        return false;
    }

    __disable_irq();
    const float iu0_phys = countsToCurrent(m_raw_burst_u_sig[0], m_raw_burst_u_ref[0]) - m_offset_u;
    const float iv0_phys = countsToCurrent(m_raw_burst_v_sig[0], m_raw_burst_v_ref[0]) - m_offset_v;
    const float iu1_phys = countsToCurrent(m_raw_burst_u_sig[1], m_raw_burst_u_ref[1]) - m_offset_u;
    const float iv1_phys = countsToCurrent(m_raw_burst_v_sig[1], m_raw_burst_v_ref[1]) - m_offset_v;
    applyPhaseSwap(iu0_phys, iv0_phys, out.point[0].iu_a, out.point[0].iv_a);
    applyPhaseSwap(iu1_phys, iv1_phys, out.point[1].iu_a, out.point[1].iv_a);
    out.point[0].time_us = m_last_burst_us;
    out.point[1].time_us = m_last_burst_us;
    out.valid = true;
    __enable_irq();

    return true;
}

} // namespace Inverter

namespace Inverter {

void PhaseCurrentADC::diagnose() {
    constexpr float COUNTS_TO_V = 3.3f / 65535.0f;
    const float u_ref_v = static_cast<float>(m_raw_u_ref) * COUNTS_TO_V;
    const float v_ref_v = static_cast<float>(m_raw_v_ref) * COUNTS_TO_V;

    float lo = 1.4f, hi = 1.9f;
    if (RteParamStore::isReady()) {
        RteParamStore::get("Hw.PhCur.RefMinV", &lo);
        RteParamStore::get("Hw.PhCur.RefMaxV", &hi);
    }

    const bool plausible = (u_ref_v >= lo && u_ref_v <= hi &&
                            v_ref_v >= lo && v_ref_v <= hi);
    if (plausible) {
        m_ref_armed = true;  /* boot-time rail settle can't false-trip */
        m_ref_implausible_since_ms = 0;
        m_ref_fault_raised = false;
        return;
    }
    if (!m_ref_armed) {
        return;
    }
    if (m_ref_implausible_since_ms == 0) {
        m_ref_implausible_since_ms = HAL_GetTick();
        return;
    }
    if ((!m_ref_fault_raised ||
         !FaultManager::instance().isActive(FaultSource::CurrentSensorRef)) &&
        (HAL_GetTick() - m_ref_implausible_since_ms) >= 500U) {
        m_ref_fault_raised = true;
        FaultManager::instance().raise(FaultSource::CurrentSensorRef,
                                       FaultReason::SensorRefOutOfRange);
        Telemetry::printf("[CUR] sensor ref implausible: U=%.2f V V=%.2f V (window %.1f..%.1f)",
                          static_cast<double>(u_ref_v),
                          static_cast<double>(v_ref_v),
                          static_cast<double>(lo),
                          static_cast<double>(hi));
    }
}

} // namespace Inverter

/* TIME_DOMAIN: ADC_PHASE_CURRENT_ISR (entry vector)
 *   Triggered by TIM1 TRGO at PWM bottom.  Hard real-time current sampling.
 * CODEGEN: Keep this vector; codegen may add additional ADC ISR dispatch if
 *   current-sensor hardware or channel assignment changes.
 */
extern "C" void ADC_IRQHandler(void) {
    HAL_ADC_IRQHandler(&hadc1);
    HAL_ADC_IRQHandler(&hadc2);
}

/* TIME_DOMAIN: PWM_SYNCHRONOUS_CURRENT_SAMPLE_ISR
 *   Rate: one completion per PWM period (RCR=1) or per half period (RCR=0).
 *   Converts raw U/V signal+reference pairs to amperes and feeds safety.
 * CODEGEN: Extend onInjectedConversionComplete for additional current channels
 *   or different sensor topologies; keep overcurrent protection and fault raising.
 */
extern "C" void HAL_ADCEx_InjectedConvCpltCallback(ADC_HandleTypeDef* hadc) {
    if (hadc->Instance == ADC1) {
        Inverter::phaseCurrentADC().onInjectedConversionComplete();
    }
}

/* TIME_DOMAIN: ADC_ANALOG_WATCHDOG_ISR
 *   Asynchronous hardware overcurrent trip.  Highest safety priority.
 *   Asserts the TIM1 break immediately: the 100 Hz safety loop would leave
 *   the bridge driving for up to 10 ms otherwise, which at full bus voltage
 *   is enough to run a real current ramp far past the trip level (measured
 *   >300 A phase on this bench).  The break is hardware-asynchronous
 *   six-switch-open; the main loop then completes the shutdown
 *   (gate-driver reset, rail power off) via FaultManager.
 * CODEGEN: Keep as-is; this is a base-image safety hook.
 */
extern "C" void HAL_ADC_LevelOutOfWindowCallback(ADC_HandleTypeDef* hadc) {
    if (hadc != nullptr && hadc->Instance == ADC1) {
        TIM1->EGR |= TIM_EGR_BG;
        Inverter::FaultManager::instance().raise(
            Inverter::FaultSource::AdcWatchdog,
            Inverter::FaultReason::AdcWatchdogTrip);
    }
}
