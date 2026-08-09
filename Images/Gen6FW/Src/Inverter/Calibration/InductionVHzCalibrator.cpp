#include "Inverter/Calibration/InductionVHzCalibrator.h"
#include "Inverter/Calibration/MotorCalibration.h"
#include "Inverter/Calibration/ResistanceCalibrator.h"
#include "Inverter/Calibration/InductanceCalibrator.h"
#include "Inverter/Calibration/InductionMotorCalibrator.h"
#include "Inverter/Calibration/CalKvStore.h"
#include "Inverter/Control/FaultManager.h"
#include "Inverter/Control/FocControlManager.h"
#include "Inverter/Control/OpenLoopController.h"
#include "Inverter/Drivers/Sensors/DcLinkVoltageSensor.h"
#include "Inverter/Drivers/Sensors/PhaseCurrentADC.h"
#include "Inverter/Drivers/PWM/pwm.h"
#include "Inverter/Telemetry.h"

#include "main.h"

#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <algorithm>

namespace Inverter {

namespace {

static InductionVHzCalibrator s_instance;

} // namespace

InductionVHzCalibrator& InductionVHzCalibrator::instance() {
    return s_instance;
}

InductionVHzCalibrator& inductionVHzCalibrator() {
    return InductionVHzCalibrator::instance();
}

bool InductionVHzCalibrator::isActive() const {
    return m_state != State::IDLE && m_state != State::DONE && m_state != State::FAIL;
}

const char* InductionVHzCalibrator::stateName() const {
    switch (m_state) {
        case State::IDLE:    return "IDLE";
        case State::START:   return "START";
        case State::RAMP:    return "RAMP";
        case State::SETTLE:  return "SETTLE";
        case State::MEASURE: return "MEASURE";
        case State::NEXT:    return "NEXT";
        case State::FINISH:  return "FINISH";
        case State::DONE:    return "DONE";
        case State::FAIL:    return "FAIL";
    }
    return "?";
}

void InductionVHzCalibrator::enterState(State s) {
    m_state = s;
    m_state_enter_ms = HAL_GetTick();
}

void InductionVHzCalibrator::fail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(m_fail_reason, sizeof(m_fail_reason), fmt, ap);
    va_end(ap);
    Telemetry::printf("[CAL] INDVHZ: FAIL: %s", m_fail_reason);
    shutdown();
    enterState(State::FAIL);
}

void InductionVHzCalibrator::shutdown() {
    if (openLoopController().isRunning()) {
        openLoopController().stop();
    }
}

bool InductionVHzCalibrator::sampleCurrents(float& iu, float& iv, float& iw) {
    if (!phaseCurrentADC().sample(iu, iv, iw)) {
        iu = phaseCurrentADC().lastU();
        iv = phaseCurrentADC().lastV();
        iw = -(iu + iv);
        return false;
    }
    return true;
}

float InductionVHzCalibrator::maxAbs3(float a, float b, float c) const {
    float m = std::fabs(a);
    if (std::fabs(b) > m) m = std::fabs(b);
    if (std::fabs(c) > m) m = std::fabs(c);
    return m;
}

void InductionVHzCalibrator::resetAccumulators() {
    m_samples = 0;
    m_sum_iu_sin = 0.0f;
    m_sum_iu_cos = 0.0f;
    m_sum_iv_sin = 0.0f;
    m_sum_iv_cos = 0.0f;
    m_sum_iw_sin = 0.0f;
    m_sum_iw_cos = 0.0f;
    m_peak_i = 0.0f;
}

void InductionVHzCalibrator::accumulateLockIn(float iu, float iv, float iw, float theta) {
    const float s = std::sin(theta);
    const float c = std::cos(theta);
    m_sum_iu_sin += iu * s;
    m_sum_iu_cos += iu * c;
    m_sum_iv_sin += iv * s;
    m_sum_iv_cos += iv * c;
    m_sum_iw_sin += iw * s;
    m_sum_iw_cos += iw * c;
    ++m_samples;

    const float peak = maxAbs3(iu, iv, iw);
    if (peak > m_peak_i) m_peak_i = peak;
}

bool InductionVHzCalibrator::computePoint(float vdc, float& out_i_a, float& out_ls_h,
                                           float& out_phi_deg) const {
    if (m_samples < 10) return false;

    out_phi_deg = 0.0f;

    const float inv = 2.0f / static_cast<float>(m_samples);
    /* For a sinusoid x(θ) = X_pk * sin(θ - φ):
     *   (2/N) * sum(x * sin(θ)) = X_pk * cos(φ)
     *   (2/N) * sum(x * cos(θ)) = -X_pk * sin(φ)
     *
     * Use the magnitude of all three phases for a noise-resistant current
     * amplitude, and the phase of phase-U only for the current angle.  This
     * avoids needing to know the exact phase sequence (V/W swap tolerant) and
     * works for both delta and wye because the U line-current phase lag equals
     * the phase-winding current lag in both connections.
     */
    const float iu_re = inv * m_sum_iu_sin;
    const float iu_im = -inv * m_sum_iu_cos;
    const float iv_re = inv * m_sum_iv_sin;
    const float iv_im = -inv * m_sum_iv_cos;
    const float iw_re = inv * m_sum_iw_sin;
    const float iw_im = -inv * m_sum_iw_cos;

    const float iu_amp = std::sqrt(iu_re * iu_re + iu_im * iu_im);
    const float iv_amp = std::sqrt(iv_re * iv_re + iv_im * iv_im);
    const float iw_amp = std::sqrt(iw_re * iw_re + iw_im * iw_im);
    float i_amp = (iu_amp + iv_amp + iw_amp) / 3.0f * m_i_scale;

    /* Phase lag of the U line current relative to the U voltage angle.
     * For an induction motor near no-load this is close to 90° (magnetizing).
     * It collapses toward 0° when the rotor is slipping. */
    const float phi_u = std::atan2(iu_im, iu_re);  /* sign depends on sensor polarity */
    const float phi_mag = std::fabs(phi_u);
    out_phi_deg = phi_mag * 180.0f / PI_F;

    /* Phase voltage at the winding.  SPWM generates peak line-neutral voltage
     * m*Vdc/2.  For delta the winding sees line-line voltage (sqrt(3) larger). */
    const float vph_peak = m_target_mod * 0.5f * vdc * (m_delta ? 1.73205080757f : 1.0f);
    const float z_mag = vph_peak / i_amp;

    /* Always report current/phi so REJECTED telemetry is useful. */
    out_i_a = i_amp;

    if (i_amp < 2.0f) {
        return false;  /* below useful ADC noise floor */
    }
    if (out_phi_deg < 50.0f || out_phi_deg > 130.0f) {
        return false;  /* invalid operating point: slipping or bad measurement */
    }

    /* Use the measured phase angle magnitude to separate reactive and resistive
     * parts.  Sign of phi_u depends on current-sensor polarity; magnitude is
     * the power-factor angle. */
    const float x_l = z_mag * std::sin(phi_mag);

    out_ls_h = x_l / (2.0f * PI_F * m_freq_hz);
    return std::isfinite(out_ls_h) && out_ls_h > 0.0f;
}

float InductionVHzCalibrator::currentPoint(int i) const {
    return (i >= 0 && i < m_n_results && i < MAX_POINTS) ? m_i_a[i] : 0.0f;
}

float InductionVHzCalibrator::modulationPoint(int i) const {
    return (i >= 0 && i < m_n_results && i < MAX_POINTS) ? m_mod[i] : 0.0f;
}

float InductionVHzCalibrator::lsPoint(int i) const {
    return (i >= 0 && i < m_n_results && i < MAX_POINTS) ? m_ls_h[i] : 0.0f;
}

bool InductionVHzCalibrator::start(float freq_hz,
                                   float max_modulation,
                                   int n_points,
                                   uint32_t settle_ms,
                                   uint32_t measure_ms,
                                   float current_limit_a,
                                   bool delta_connected) {
    if (isActive()) {
        Telemetry::printf("[CAL] INDVHZ: already running");
        return false;
    }
    if (openLoopController().isRunning() || focControlManager().isRunning()) {
        Telemetry::printf("[CAL] INDVHZ: stop the motor first");
        return false;
    }
    if (resistanceCalibrator().isActive() || inductanceCalibrator().isActive() ||
        inductionMotorCalibrator().isActive()) {
        Telemetry::printf("[CAL] INDVHZ: another calibration is active");
        return false;
    }

    const MotorCalibration& mc = motorCalibration();
    if (!mc.valid || mc.r_phase_avg <= 0.0f) {
        Telemetry::printf("[CAL] INDVHZ: need a valid resistance calibration first");
        return false;
    }
    if (FaultManager::instance().isSeverityActive(FaultSeverity::Critical) ||
        FaultManager::instance().isSeverityActive(FaultSeverity::High)) {
        Telemetry::printf("[CAL] INDVHZ: active Critical/High faults");
        return false;
    }
    if (dcLinkVoltageSensor().voltage() < 10.0f) {
        Telemetry::printf("[CAL] INDVHZ: DC bus too low");
        return false;
    }
    if (freq_hz < 1.0f || freq_hz > 100.0f ||
        max_modulation <= 0.0f || max_modulation > 1.15f ||
        n_points < 3 || n_points > MAX_POINTS ||
        settle_ms < 100U || measure_ms < 500U ||
        current_limit_a <= 0.0f || current_limit_a > 500.0f) {
        Telemetry::printf("[CAL] INDVHZ: bad parameters");
        return false;
    }

    m_freq_hz = freq_hz;
    m_max_mod = max_modulation;
    m_n_points = n_points;
    m_settle_ms = settle_ms;
    measure_ms_ = measure_ms;
    m_current_limit_a = current_limit_a;
    m_delta = delta_connected;
    m_i_scale = delta_connected ? (1.0f / std::sqrt(3.0f)) : 1.0f;

    m_point = 0;
    m_mod_step = max_modulation / static_cast<float>(n_points - 1);
    m_target_mod = 0.0f;

    m_n_results = 0;
    for (int i = 0; i < MAX_POINTS; ++i) {
        m_i_a[i] = m_mod[i] = m_ls_h[i] = 0.0f;
    }
    m_fail_reason[0] = '\0';

    Telemetry::printf("[CAL] INDVHZ: start f=%.1f Hz max_mod=%.3f points=%d "
                      "%s R=%.4f ohm",
                      static_cast<double>(m_freq_hz),
                      static_cast<double>(m_max_mod),
                      m_n_points,
                      m_delta ? "delta" : "wye",
                      static_cast<double>(mc.r_phase_avg));

    enterState(State::START);
    return true;
}

void InductionVHzCalibrator::stop() {
    if (!isActive()) return;
    shutdown();
    Telemetry::printf("[CAL] INDVHZ: stopped");
    enterState(State::IDLE);
}

void InductionVHzCalibrator::update() {
    if (!isActive()) return;

    if (FaultManager::instance().isSeverityActive(FaultSeverity::Critical) ||
        FaultManager::instance().isSeverityActive(FaultSeverity::High)) {
        fail("Critical/High fault detected");
        return;
    }

    const uint32_t now = HAL_GetTick();

    switch (m_state) {
        case State::START: {
            openLoopController().setRampCurrentLimit(m_current_limit_a);
            if (!openLoopController().start(m_freq_hz, 0.0f)) {
                fail("failed to start open-loop V/Hz");
                return;
            }
            Telemetry::printf("[CAL] INDVHZ: open-loop running at %.1f Hz",
                              static_cast<double>(m_freq_hz));
            enterState(State::RAMP);
            return;
        }

        case State::RAMP: {
            m_target_mod = static_cast<float>(m_point) * m_mod_step;
            if (m_target_mod > m_max_mod) m_target_mod = m_max_mod;
            openLoopController().setModulationIndexDirect(m_target_mod);
            Telemetry::printf("[CAL] INDVHZ: point %d/%d mod=%.3f",
                              m_point + 1, m_n_points,
                              static_cast<double>(m_target_mod));
            resetAccumulators();
            enterState(State::SETTLE);
            return;
        }

        case State::SETTLE: {
            float iu, iv, iw;
            sampleCurrents(iu, iv, iw);
            if (maxAbs3(iu, iv, iw) > m_current_limit_a) {
                fail("overcurrent %.1f A > %.1f A",
                     static_cast<double>(maxAbs3(iu, iv, iw)),
                     static_cast<double>(m_current_limit_a));
                return;
            }
            if ((now - m_state_enter_ms) < m_settle_ms) return;
            resetAccumulators();
            enterState(State::MEASURE);
            return;
        }

        case State::MEASURE: {
            float iu, iv, iw;
            sampleCurrents(iu, iv, iw);
            const float ipeak = maxAbs3(iu, iv, iw);
            if (ipeak > m_current_limit_a) {
                fail("overcurrent %.1f A > %.1f A",
                     static_cast<double>(ipeak),
                     static_cast<double>(m_current_limit_a));
                return;
            }

            const float theta = PWM_GetSPWMAngle();
            accumulateLockIn(iu, iv, iw, theta);

            if ((now - m_state_enter_ms) < measure_ms_) return;

            float i_a = 0.0f, ls_h = 0.0f, phi_deg = 0.0f;
            const bool ok = computePoint(dcLinkVoltageSensor().voltage(), i_a, ls_h,
                                         phi_deg);

            if (ok && m_n_results < MAX_POINTS) {
                m_mod[m_n_results] = m_target_mod;
                m_i_a[m_n_results] = i_a;
                m_ls_h[m_n_results] = ls_h;
                ++m_n_results;
            }

            if (ok) {
                Telemetry::printf("[CAL] INDVHZ: point %d mod=%.3f I=%.2f A phi=%.1fdeg Ls=%.1f uH "
                                  "(samples=%lu peak=%.2f A)",
                                  m_point + 1,
                                  static_cast<double>(m_target_mod),
                                  static_cast<double>(i_a),
                                  static_cast<double>(phi_deg),
                                  static_cast<double>(ls_h * 1.0e6f),
                                  static_cast<unsigned long>(m_samples),
                                  static_cast<double>(m_peak_i));
            } else {
                Telemetry::printf("[CAL] INDVHZ: point %d mod=%.3f REJECTED I=%.2f A phi=%.1fdeg "
                                  "(samples=%lu peak=%.2f A)",
                                  m_point + 1,
                                  static_cast<double>(m_target_mod),
                                  static_cast<double>(i_a),
                                  static_cast<double>(phi_deg),
                                  static_cast<unsigned long>(m_samples),
                                  static_cast<double>(m_peak_i));
            }

            ++m_point;
            if (m_point >= m_n_points) {
                enterState(State::FINISH);
            } else {
                enterState(State::RAMP);
            }
            return;
        }

        case State::FINISH: {
            shutdown();

            /* Publish the lowest-current valid point as the small-signal Ls. */
            MotorCalibration& mc = motorCalibration();
            float best_ls = 0.0f;
            float best_i = 1e9f;
            for (int i = 0; i < m_n_results; ++i) {
                if (m_ls_h[i] > 0.0f && m_i_a[i] < best_i) {
                    best_i = m_i_a[i];
                    best_ls = m_ls_h[i];
                }
            }
            if (best_ls > 0.0f) {
                mc.ld_henry = best_ls;
                mc.lq_henry = best_ls;
            }

            CalKvStore::saveInductanceResults(best_ls, best_ls);
            CalKvStore::flush();

            Telemetry::printf("[CAL] INDVHZ: ========================================");
            Telemetry::printf("[CAL] INDVHZ: V/Hz INDUCTANCE SWEEP COMPLETE");
            for (int i = 0; i < m_n_results; ++i) {
                Telemetry::printf("[CAL] INDVHZ:   mod=%.3f I=%5.2f A Ls=%7.1f uH",
                                  static_cast<double>(m_mod[i]),
                                  static_cast<double>(m_i_a[i]),
                                  static_cast<double>(m_ls_h[i] * 1.0e6f));
            }
            Telemetry::printf("[CAL] INDVHZ: ========================================");
            enterState(State::DONE);
            return;
        }

        case State::IDLE:
        case State::DONE:
        case State::FAIL:
        default:
            return;
    }
}

} // namespace Inverter
