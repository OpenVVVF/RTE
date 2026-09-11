/*
 * sil_phase_current_adc.cpp — SIL replacement for
 * Src/Inverter/Drivers/Sensors/PhaseCurrentADC.cpp.
 *
 * Reproduces the firmware-visible behavior of the PWM-synchronous
 * phase-current ADC against the simulated plant:
 *
 *  - The scheduler calls onInjectedConversionComplete() once per TIM1 TRGO
 *    (see sil_hooks.h); the function mirrors the hardware ISR order:
 *    LoopStats, domain dt, generated adc_isr step (which reads the PREVIOUS
 *    sample through platform_get_phase_currents(), exactly one tick stale —
 *    the emitter inserts app::AdcIsrStep at the same point in the real
 *    file), then the new burst conversion.
 *  - Plant phase currents are converted to raw 16-bit LA37S600 differential
 *    sig/ref counts.  The sensor wiring inversion documented in
 *    FocControlManager.cpp ("sensors wired with inverted polarity") is
 *    modeled here: sig = ref_mid - i * counts/A.  Downstream firmware
 *    (generated adc_isr graph, InvertPolarity=1) re-inverts, matching
 *    hardware.
 *  - start()/stop() mirror the hardware sequence (calibration, injected
 *    start, TIM1 base start) and perform the zero-offset capture directly
 *    against the (standstill) plant instead of spin-waiting on ISR state —
 *    the SIL runtime is cooperative, so the hardware's
 *    "while (!m_new_data)" pattern would deadlock.
 */
#include "Inverter/Drivers/Sensors/PhaseCurrentADC.h"
#include "Inverter/AppState.h"
#include "Inverter/LoopStats.h"
#include "Inverter/Drivers/PWM/pwm.h"
#include "Inverter/platform_api.h"
#include "Inverter/Calibration/EncoderCycleCalibrator.h"
#include "Inverter/Control/FaultManager.h"
#include "Inverter/Drivers/Sensors/EncoderADC.h"
#include "Inverter/Drivers/Sensors/PoleEstimator.h"
#include "Inverter/Drivers/Sensors/SpikeRecorder.h"
#include "Inverter/Drivers/Storage/RteParamStore.h"
#include "Inverter/Calibration/MotorCalibration.h"
#include "Inverter/Telemetry.h"

#include "main.h"
#include "adc.h"
#include "tim.h"

#include "domain_adc_isr_generated.h"

#include "sil_world.h"
#include "sil_hooks.h"

#include <cstdio>
#include <cmath>

namespace Inverter {

static PhaseCurrentADC s_instance;

PhaseCurrentADC& phaseCurrentADC() {
    return s_instance;
}

/* Physical LA37S600 model: differential output vs. 1.65 V reference, through
 * the 2/3 divider into the 16-bit ADC.  kCountsPerAmp matches
 * PhaseCurrentADC::countsToCurrent in reverse. */
namespace {
constexpr float kDivider      = 2.0f / 3.0f;
constexpr float kSensVa       = 1.042e-3f;
constexpr float kCountsFull   = 65535.0f;
constexpr float kCountsPerAmp =
    (kDivider * kSensVa * kCountsFull) / 3.3f;
constexpr float kRefMidCounts = kCountsFull * 0.5f;

uint32_t sigCountsForCurrent(float i_a) {
    float c = kRefMidCounts - i_a * kCountsPerAmp;   /* inverted wiring */
    if (c < 0.0f) c = 0.0f;
    if (c > kCountsFull) c = kCountsFull;
    return static_cast<uint32_t>(c + 0.5f);
}

/* One raw acquisition from the plant into the ADC data registers, in the
 * real hardware channel order (U sig/ref on rank 1/3, V sig/ref on 2/4).
 * Scenario overcurrent injection (faults.oc_inject_*) enters here, at the
 * conversion level: the fault is exactly what a saturated/glitched current
 * channel looks like — the plant itself stays physically consistent. */
void silAcquireFromPlant() {
    const auto& st = silWorld().plant.State();
    float ia = st.ia_a;
    float ib = st.ib_a;
    const SilWorld& w = silWorld();
    if (w.oc_fault_active) {
        /* iu+iv+iw=0 at the sensor: a W-channel spike is the negative
         * injection into both measured channels. */
        switch (w.oc_fault_phase) {
            case 1:  ib += w.oc_fault_a; break;
            case 2:  ia -= w.oc_fault_a; ib -= w.oc_fault_a; break;
            default: ia += w.oc_fault_a; break;
        }
    }
    const uint32_t u_sig = sigCountsForCurrent(ia);
    const uint32_t v_sig = sigCountsForCurrent(ib);
    const uint32_t ref   = static_cast<uint32_t>(kRefMidCounts + 0.5f);

    sil_adc1.JDR1 = u_sig;
    sil_adc1.JDR2 = v_sig;
    sil_adc1.JDR3 = u_sig;
    sil_adc1.JDR4 = v_sig;
    sil_adc2.JDR1 = ref;
    sil_adc2.JDR2 = ref;
    sil_adc2.JDR3 = ref;
    sil_adc2.JDR4 = ref;
}
} // namespace

/* Map measured physical U/V currents to the logical UVW frame, accounting
 * for Motor.PhaseSwap (same as the hardware driver). */
static void applyPhaseSwap(float iu_phys_a, float iv_phys_a,
                           float& iu_log_a, float& iv_log_a) {
    switch (motorCalibration().phase_swap) {
        case PhaseSwap::SwapUV:
            iu_log_a = iv_phys_a;
            iv_log_a = iu_phys_a;
            break;
        case PhaseSwap::SwapVW:
            iu_log_a = iv_phys_a;
            iv_log_a = -(iu_phys_a + iv_phys_a);
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

bool PhaseCurrentADC::init() {
    /* DWT cycle counter (burst timestamps); the SIL DWT always runs. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    if (!configureAnalogWatchdog()) return false;

    return true;
}

bool PhaseCurrentADC::configureAdcChannels() { return true; }
bool PhaseCurrentADC::initTrigger() { return true; }

bool PhaseCurrentADC::start() {
    if (m_running) return true;

    /* Arm the injected path and start the TIM1 counter (the SIL scheduler
     * starts firing TRGO/injected events once this returns). */
    if (HAL_ADCEx_InjectedStart_IT(&hadc2) != HAL_OK) return false;
    if (HAL_ADCEx_InjectedStart_IT(&hadc1) != HAL_OK) {
        HAL_ADCEx_InjectedStop_IT(&hadc2);
        return false;
    }
    if (HAL_TIM_Base_Start(&htim1) != HAL_OK) {
        HAL_ADCEx_InjectedStop_IT(&hadc1);
        HAL_ADCEx_InjectedStop_IT(&hadc2);
        return false;
    }

    m_running = true;

    if (!calibrateOffsets()) {
        HAL_TIM_Base_Stop(&htim1);
        HAL_ADCEx_InjectedStop_IT(&hadc1);
        HAL_ADCEx_InjectedStop_IT(&hadc2);
        m_running = false;
        return false;
    }

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

bool PhaseCurrentADC::configureAnalogWatchdog() {
    /* The hardware AWD is not modeled; software OC checks still run. */
    return true;
}

bool PhaseCurrentADC::setHardwareOvercurrentThreshold(float amps) {
    if (m_running) {
        return false;
    }
    if (amps < 0.0f) amps = 0.0f;
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
        m_fixed_ref_u = m_raw_u_ref;
        m_fixed_ref_v = m_raw_v_ref;
    }
    m_use_fixed_ref = use_fixed;
}

float PhaseCurrentADC::countsToCurrent(uint32_t sig, uint32_t ref) const {
    const float lsb   = ADC_VREF / static_cast<float>((1U << ADC_BITS) - 1U);
    const float scale = lsb / (DIVIDER * SENSITIVITY_VA);
    if (m_use_fixed_ref) {
        const float sampled = static_cast<float>(ref);
        const float fixed_u = static_cast<float>(m_fixed_ref_u);
        const float fixed_v = static_cast<float>(m_fixed_ref_v);
        const float fixed = (std::fabs(sampled - fixed_u) < std::fabs(sampled - fixed_v))
                                ? fixed_u : fixed_v;
        return (static_cast<float>(sig) - fixed) * scale;
    }
    return (static_cast<float>(sig) - static_cast<float>(ref)) * scale;
}

/* (raw acquisition helper lives in the file-static namespace above) */

bool PhaseCurrentADC::calibrateOffsets() {
    constexpr uint32_t DISCARD_SAMPLES = 500;
    constexpr uint32_t AVG_SAMPLES     = 1000;

    /* SIL: sample the (standstill) plant directly — the hardware spins on
     * ISR-fed m_new_data here; the cooperative SIL runtime would deadlock. */
    m_new_data = false;
    float sum_u = 0.0f;
    float sum_v = 0.0f;
    for (uint32_t i = 0; i < DISCARD_SAMPLES + AVG_SAMPLES; ++i) {
        silAcquireFromPlant();
        const float iu = countsToCurrent(sil_adc1.JDR1, sil_adc2.JDR1);
        const float iv = countsToCurrent(sil_adc1.JDR2, sil_adc2.JDR2);
        if (i >= DISCARD_SAMPLES) {
            sum_u += iu;
            sum_v += iv;
        }
    }

    m_offset_u = sum_u / static_cast<float>(AVG_SAMPLES);
    m_offset_v = sum_v / static_cast<float>(AVG_SAMPLES);

    constexpr float MAX_SANE_OFFSET_A = 50.0f;
    m_offset_valid = (std::fabs(m_offset_u) < MAX_SANE_OFFSET_A) &&
                     (std::fabs(m_offset_v) < MAX_SANE_OFFSET_A);
    if (!m_offset_valid) {
        Telemetry::printf("[CUR] WARNING: bad offset U=%.2f V=%.2f A; sensor not settled",
                          static_cast<double>(m_offset_u),
                          static_cast<double>(m_offset_v));
    }

    return m_offset_valid;
}

void PhaseCurrentADC::onInjectedConversionComplete() {
    ++LoopStats::adc_isr;
    platform_set_current_domain_dt(1.0f / PWM_GetUpdateFrequency());

    /* Equivalent of the emitter-filled `// RTE_EMIT: adc_isr step`:
     * the generated domain reads the PREVIOUS sample via
     * platform_get_phase_currents() before the new burst is decoded below —
     * one tick of latency, exactly like hardware. */
    app::AdcIsrStep(appState.adc_isr);

    /* New micro-burst from the plant. */
    silAcquireFromPlant();
    m_raw_burst_u_sig[0] = HAL_ADCEx_InjectedGetValue(&hadc1, ADC_INJECTED_RANK_1);
    m_raw_burst_v_sig[0] = HAL_ADCEx_InjectedGetValue(&hadc1, ADC_INJECTED_RANK_2);
    m_raw_burst_u_sig[1] = HAL_ADCEx_InjectedGetValue(&hadc1, ADC_INJECTED_RANK_3);
    m_raw_burst_v_sig[1] = HAL_ADCEx_InjectedGetValue(&hadc1, ADC_INJECTED_RANK_4);

    m_raw_burst_u_ref[0] = HAL_ADCEx_InjectedGetValue(&hadc2, ADC_INJECTED_RANK_1);
    m_raw_burst_v_ref[0] = HAL_ADCEx_InjectedGetValue(&hadc2, ADC_INJECTED_RANK_2);
    m_raw_burst_u_ref[1] = HAL_ADCEx_InjectedGetValue(&hadc2, ADC_INJECTED_RANK_3);
    m_raw_burst_v_ref[1] = HAL_ADCEx_InjectedGetValue(&hadc2, ADC_INJECTED_RANK_4);

    m_last_burst_us = DWT->CYCCNT / (SystemCoreClock / 1000000U);

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

    /* Spike event recorder: synchronized raw currents + encoder snapshot. */
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

    /* Software overcurrent protection. */
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

    /* Pole estimator and encoder-cycle counter feeds. */
    PoleEstimator::instance().onSample(
        m_current_u, encoderADC().lastRawSin(), encoderADC().lastRawCos());
    EncoderCycleCalibrator::instance().onSample(encoderADC().lastRawSin(),
                                                encoderADC().lastRawCos());

    m_new_data = true;
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
        m_ref_armed = true;
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
    if (!m_ref_fault_raised &&
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

/* Scheduler hooks (not firmware-visible). */
bool silPhaseCurrentAdcRunning() {
    /* Injected conversions run once the driver was started (TIM1 base + IT
     * armed inside start()). */
    extern ADC_HandleTypeDef hadc1;
    return hadc1.sil_injected_running != 0;
}

void silPhaseCurrentAdcTrigger() {
    Inverter::phaseCurrentADC().onInjectedConversionComplete();
}
