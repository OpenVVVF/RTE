/*
 * sil_encoder_adc.cpp — SIL replacement for
 * Src/Inverter/Drivers/Sensors/EncoderADC.cpp.
 *
 * Models the analog sin/cos encoder (ADC2 regular + DMA stream): the
 * plant's mechanical rotor angle is rendered as quantized 16-bit sin/cos
 * ADC counts (center 32768, amplitude 30000, one electrical-style cycle per
 * mechanical revolution — CyclesRev=1, well inside the driver's hard caps
 * 427..65388), and the decoding pipeline (bounds learning, atan2 decode,
 * extrapolation, RPM window estimate, signal-quality diagnostics) is copied
 * semantically from the hardware driver.
 *
 * The simulated sample stream is driven by the scheduler:
 *   - free-running TIM2 trigger (~10 kHz) while control is off,
 *   - TIM1 TRGO2-synchronized (one per update event) while control runs
 *     (useSynchronizedTrigger(true) mirrors the CFGR EXTSEL switch).
 *
 * Additionally, diagnose() is the firmware app-loop rendezvous point for the
 * cooperative SIL runtime (sil_rt_app_gate) — it is called exactly once per
 * InverterMain::loop() pass.
 *
 * The hardware start() spin-waits and DMA/LL plumbing are replaced by direct
 * sim hooks; fit capture/compute logic is preserved verbatim.
 */
#include "Inverter/Drivers/Sensors/EncoderADC.h"
#include "Inverter/Telemetry.h"
#include "Inverter/Control/FaultManager.h"

#include "main.h"
#include "adc.h"

#include "sil_rt.h"
#include "sil_world.h"

#include <cmath>

namespace Inverter {

static EncoderADC s_instance;

/* Raw sin/cos counts for the latest sample (the hardware DMA buffer). */
static uint16_t s_enc_dma_buffer[2];

EncoderADC::FitAccumulator EncoderADC::s_fit_acc;
EncoderADC::SinCosFit      EncoderADC::s_fit;
EncoderADC::TraceEntry     EncoderADC::m_trace[EncoderADC::TRACE_LEN];

EncoderADC& encoderADC() {
    return s_instance;
}

bool EncoderADC::configureAdcChannels() { return true; }

bool EncoderADC::initTimer() {
    /* TIM2 TRGO: 10 kHz free-running sample trigger (APB1 137.5 MHz /
     * 13750 ticks as in the hardware configuration). */
    m_sample_hz = 10000.0f;
    return true;
}

bool EncoderADC::initDma() { return true; }

bool EncoderADC::init() {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    initializeFitState();
    m_trace_head = 0;
    m_trace_decim = 0;

    if (!configureAdcChannels()) return false;
    if (!initTimer()) return false;
    if (!initDma()) return false;
    return true;
}

bool EncoderADC::start() {
    if (m_running) return true;
    m_running = true;
    silWorld().encoder_stream_running = true;
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

    if (csin < m_obs_sin_min) m_obs_sin_min = csin;
    if (csin > m_obs_sin_max) m_obs_sin_max = csin;
    if (ccos < m_obs_cos_min) m_obs_cos_min = ccos;
    if (ccos > m_obs_cos_max) m_obs_cos_max = ccos;

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
    const uint32_t age_cycles = DWT->CYCCNT - m_last_sample_cycles;
    const float age_s = static_cast<float>(age_cycles) /
                        static_cast<float>(SystemCoreClock);
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
    /* SIL: remember the trigger select; the scheduler drives the stream. */
    silWorld().encoder_sync_trigger = sync;
}

void EncoderADC::traceDump() {
    Telemetry::printf("[SHELL] enc trace: %d samples @ ~1 kHz (sin cos angle_deg), oldest first",
                      static_cast<int>(TRACE_LEN));
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
    const uint16_t raw_sin = s_enc_dma_buffer[0];
    const uint16_t raw_cos = s_enc_dma_buffer[1];

    const float angle = computeAngle(raw_sin, raw_cos);

    m_snapshot.angle = angle;
    m_snapshot.raw_sin = raw_sin;
    m_snapshot.raw_cos = raw_cos;
    m_new_data = true;
    m_last_sample_ms = HAL_GetTick();
    m_last_sample_cycles = DWT->CYCCNT;
    ++m_isr_count;

    if (m_fit_capture) {
        s_fit_acc.add(raw_sin, raw_cos);
    }

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

    const float sin_phi = static_cast<float>(
        -2.0 * cov_sincos / (static_cast<double>(amp_sin) * static_cast<double>(amp_cos)));

    float clamped_sin_phi = sin_phi;
    if (clamped_sin_phi > FIT_MAX_SIN_PHASE) clamped_sin_phi = FIT_MAX_SIN_PHASE;
    if (clamped_sin_phi < -FIT_MAX_SIN_PHASE) clamped_sin_phi = -FIT_MAX_SIN_PHASE;
    const float phi = asinf(clamped_sin_phi);

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
    /* Cooperative SIL runtime app-loop rendezvous: diagnose() runs exactly
     * once per InverterMain::loop() pass, so the scheduler releases one
     * app_loop iteration per call here. */
    sil_rt_app_gate();

    const uint32_t now_ms = HAL_GetTick();

    /* Mechanical speed: time-window estimate at main-loop cadence (copied
     * from the hardware driver). */
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

    /* Signal-quality faults: magnitude collapse and rail sticking. */
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

    /* Publish the measured trigger/ISR rate once a second; self-calibrate
     * the estimator's time base from it (identical to hardware behavior). */
    static uint32_t s_last_ms = 0;
    static uint32_t s_last_count = 0;
    if (s_last_ms != 0U && (now_ms - s_last_ms) >= 1000U) {
        const float hz = static_cast<float>(m_isr_count - s_last_count) *
                         (1000.0f / static_cast<float>(now_ms - s_last_ms));
        Telemetry::log("enc_isr_hz", hz);
        m_sample_hz = hz;
        s_last_count = m_isr_count;
        s_last_ms = now_ms;
    } else if (s_last_ms == 0U) {
        s_last_count = m_isr_count;
        s_last_ms = now_ms;
    }

    if (m_running && (now_ms - m_last_sample_ms) > SAMPLE_TIMEOUT_MS) {
        /* Same as hardware: timeout fault currently disabled (interferes
         * with calibration work). */
    }
}

} // namespace Inverter

/* --------------------------------------------------------------------------
 * Scheduler hooks (not firmware-visible)
 * ------------------------------------------------------------------------ */

bool silEncoderRunning() {
    return silWorld().encoder_stream_running;
}

bool silEncoderSyncTrigger() {
    return silWorld().encoder_sync_trigger;
}

void silEncoderSampleFromPlant() {
    /* Render the plant's mechanical rotor angle as quantized sin/cos counts:
     * center 32768, amplitude 30000, one sin/cos cycle per mechanical rev. */
    const auto& st = silWorld().plant.State();
    const float pp = static_cast<float>(silWorld().plant.Model().Params().pole_pairs);
    const float two_pi = 6.28318530718f;
    float theta_m = (pp > 0.0f) ? (st.theta_e_rad / pp) : 0.0f;
    theta_m = std::fmod(theta_m, two_pi);
    if (theta_m < 0.0f) theta_m += two_pi;

    const float s = 32768.0f + 30000.0f * sinf(theta_m);
    const float c = 32768.0f + 30000.0f * cosf(theta_m);
    auto q = [](float x) -> uint16_t {
        if (x < 0.0f) x = 0.0f;
        if (x > 65535.0f) x = 65535.0f;
        return static_cast<uint16_t>(x + 0.5f);
    };
    Inverter::s_enc_dma_buffer[0] = q(s);
    Inverter::s_enc_dma_buffer[1] = q(c);
    Inverter::encoderADC().onDmaComplete();
}
