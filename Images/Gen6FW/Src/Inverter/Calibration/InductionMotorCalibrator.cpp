#include "Inverter/Calibration/InductionMotorCalibrator.h"
#include "Inverter/Calibration/MotorCalibration.h"
#include "Inverter/Calibration/PoleCalibrator.h"
#include "Inverter/Calibration/EncoderOffsetCalibrator.h"
#include "Inverter/Calibration/ResistanceCalibrator.h"
#include "Inverter/Calibration/InductanceCalibrator.h"
#include "Inverter/Control/FaultManager.h"
#include "Inverter/Control/FocControlManager.h"
#include "Inverter/Control/OpenLoopController.h"
#include "Inverter/Drivers/Sensors/DcLinkVoltageSensor.h"
#include "Inverter/Drivers/Sensors/EncoderADC.h"
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

/* Keep the large decay buffers out of scarce DTCMRAM; calibration objects can
 * live in AXI SRAM (RAM_D1) for the duration of a commissioning run. */
static InductionMotorCalibrator s_instance __attribute__((section(".dma_buffers")));

constexpr float PI_F = 3.14159265358979323846f;

float maxAbs3(float a, float b, float c) {
    float m = std::fabs(a);
    if (std::fabs(b) > m) m = std::fabs(b);
    if (std::fabs(c) > m) m = std::fabs(c);
    return m;
}

} // namespace

InductionMotorCalibrator& InductionMotorCalibrator::instance() {
    return s_instance;
}

InductionMotorCalibrator& inductionMotorCalibrator() {
    return InductionMotorCalibrator::instance();
}

bool InductionMotorCalibrator::isActive() const {
    return m_state != State::IDLE && m_state != State::DONE && m_state != State::FAIL;
}

const char* InductionMotorCalibrator::stateName() const {
    switch (m_state) {
        case State::IDLE:              return "IDLE";
        case State::ENABLE:            return "ENABLE";
        case State::WAIT_READY:        return "WAIT_READY";
        case State::SIGMA_LS_SETTLE:   return "SIGMA_LS_SETTLE";
        case State::SIGMA_LS_MEASURE:  return "SIGMA_LS_MEASURE";
        case State::TAU_R_FLUXING:     return "TAU_R_FLUXING";
        case State::TAU_R_SETTLE:      return "TAU_R_SETTLE";
        case State::TAU_R_DECAY:       return "TAU_R_DECAY";
        case State::COMPUTE:           return "COMPUTE";
        case State::DONE:              return "DONE";
        case State::FAIL:              return "FAIL";
    }
    return "?";
}

void InductionMotorCalibrator::enterState(State s) {
    m_state = s;
    m_state_enter_ms = HAL_GetTick();
}

void InductionMotorCalibrator::fail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(m_fail_reason, sizeof(m_fail_reason), fmt, ap);
    va_end(ap);
    Telemetry::printf("[CAL] INDUCTION: FAIL: %s", m_fail_reason);
    shutdown();
    enterState(State::FAIL);
}

void InductionMotorCalibrator::shutdown() {
    PWM_StopSPWM();
    CalibrationHardware::shutdown();
}

void InductionMotorCalibrator::sampleCurrents(float& iu, float& iv, float& iw) {
    if (!phaseCurrentADC().sample(iu, iv, iw)) {
        iu = phaseCurrentADC().lastU();
        iv = phaseCurrentADC().lastV();
        iw = -(iu + iv);
    }
}

bool InductionMotorCalibrator::checkOvercurrent(float iu, float iv, float iw) {
    if (maxAbs3(iu, iv, iw) > OVERCURRENT_A) {
        fail("overcurrent %.1f A > %.1f A",
             static_cast<double>(maxAbs3(iu, iv, iw)),
             static_cast<double>(OVERCURRENT_A));
        return true;
    }
    return false;
}

bool InductionMotorCalibrator::checkEncoderMovement() {
    if (!m_encoder_guard) return false;
    if (!encoderADC().boundsValid()) return false;
    const float ang = encoderADC().lastAngle();
    float diff = std::fabs(ang - m_encoder_start_angle);
    while (diff > 360.0f) diff -= 360.0f;
    if (diff > ENCODER_MOVE_DEG) {
        fail("rotor moved %.1f deg during DC phase", static_cast<double>(diff));
        return true;
    }
    return false;
}

void InductionMotorCalibrator::updateFluxPi(float i_meas_a) {
    const uint32_t now = HAL_GetTick();
    const float dt_s = (m_flux_pi_last_ms == 0U) ? 0.01f :
                       static_cast<float>(now - m_flux_pi_last_ms) * 1.0e-3f;
    m_flux_pi_last_ms = now;

    const float error = m_max_flux_current_a - i_meas_a;
    m_flux_integral += FLUX_PI_KI * error * dt_s;
    if (m_flux_integral > FLUX_PI_MAX_MOD) m_flux_integral = FLUX_PI_MAX_MOD;
    if (m_flux_integral < FLUX_PI_MIN_MOD) m_flux_integral = FLUX_PI_MIN_MOD;

    float mod = FLUX_PI_KP * error + m_flux_integral;
    if (mod > FLUX_PI_MAX_MOD) mod = FLUX_PI_MAX_MOD;
    if (mod < FLUX_PI_MIN_MOD) mod = FLUX_PI_MIN_MOD;
    m_flux_mod_index = mod;

    /* Park a stationary vector aligned with the U axis for consistent fluxing. */
    PWM_SetVoltageAngle(PI_F * 0.5f, m_flux_mod_index);
}

bool InductionMotorCalibrator::start(float max_flux_current_a,
                                     float ac_voltage_pct,
                                     float ac_freq_hz,
                                     uint32_t fluxing_time_ms,
                                     uint32_t decay_sample_time_ms,
                                     float lm_estimate_henry) {
    if (isActive()) {
        Telemetry::printf("[CAL] INDUCTION: already running");
        return false;
    }
    if (openLoopController().isRunning() || focControlManager().isRunning()) {
        Telemetry::printf("[CAL] INDUCTION: stop the motor first");
        return false;
    }
    if (poleCalibrator().isActive() || encoderOffsetCalibrator().isActive() ||
        resistanceCalibrator().isActive() || inductanceCalibrator().isActive()) {
        Telemetry::printf("[CAL] INDUCTION: another calibration is active");
        return false;
    }

    const MotorCalibration& mc = motorCalibration();
    if (!mc.valid || mc.r_phase_avg <= 0.0f) {
        Telemetry::printf("[CAL] INDUCTION: need a valid resistance calibration first");
        return false;
    }
    if (FaultManager::instance().isSeverityActive(FaultSeverity::Critical) ||
        FaultManager::instance().isSeverityActive(FaultSeverity::High)) {
        Telemetry::printf("[CAL] INDUCTION: active Critical/High faults");
        return false;
    }
    if (dcLinkVoltageSensor().voltage() < 10.0f) {
        Telemetry::printf("[CAL] INDUCTION: DC bus too low (%.1f V)",
                          static_cast<double>(dcLinkVoltageSensor().voltage()));
        return false;
    }
    if (max_flux_current_a < 1.0f || max_flux_current_a > 200.0f ||
        ac_voltage_pct <= 0.0f || ac_voltage_pct > 50.0f ||
        ac_freq_hz < 10.0f || ac_freq_hz > 400.0f ||
        fluxing_time_ms < 500U || fluxing_time_ms > 20000U ||
        decay_sample_time_ms < 500U || decay_sample_time_ms > 2000U) {
        Telemetry::printf("[CAL] INDUCTION: bad parameters");
        return false;
    }

    m_max_flux_current_a = max_flux_current_a;
    m_ac_voltage_pct = ac_voltage_pct;
    m_ac_freq_hz = ac_freq_hz;
    m_fluxing_time_ms = fluxing_time_ms;
    m_decay_sample_time_ms = decay_sample_time_ms;
    m_lm_estimate_h = lm_estimate_henry;

    m_ac_mod_index = ac_voltage_pct * 0.01f;
    m_spwm_start_cycles = 0;
    m_sigma_i_peak = 0.0f;
    m_sigma_samples = 0;
    m_flux_mod_index = 0.0f;
    m_flux_integral = 0.0f;
    m_flux_pi_last_ms = 0;
    m_decay_n = 0;
    m_last_decay_ms = 0;
    m_fail_reason[0] = '\0';

    m_sigma_ls_h = 0.0f;
    m_rotor_tau_ms = 0.0f;
    m_lm_h = 0.0f;
    m_lr_h = 0.0f;
    m_rr_ohm = 0.0f;
    m_l_leak_h = 0.0f;

    m_encoder_guard = encoderADC().boundsValid();
    m_encoder_start_angle = encoderADC().lastAngle();

    /* Use the same switching frequency as the open-loop controller so the
     * SPWM ISR has a known, stable rate. */
    PWM_SetFrequency(2500U);
    PWM_SetDeadTime(1000U);
    PWM_SetThreePhaseDuty(50.0f, 50.0f, 50.0f);
    PWM_DisableFocMode();

    Telemetry::printf("[CAL] INDUCTION: start flux_I=%.1f A ac=%.1f%% f=%.1f Hz "
                      "flux_ms=%lu decay_ms=%lu R=%.4f ohm",
                      static_cast<double>(m_max_flux_current_a),
                      static_cast<double>(m_ac_voltage_pct),
                      static_cast<double>(m_ac_freq_hz),
                      static_cast<unsigned long>(m_fluxing_time_ms),
                      static_cast<unsigned long>(m_decay_sample_time_ms),
                      static_cast<double>(mc.r_phase_avg));

    m_hw.begin();
    enterState(State::ENABLE);
    return true;
}

void InductionMotorCalibrator::stop() {
    if (!isActive()) return;
    shutdown();
    Telemetry::printf("[CAL] INDUCTION: stopped");
    enterState(State::IDLE);
}

bool InductionMotorCalibrator::runSigmaLsSettle() {
    if ((HAL_GetTick() - m_state_enter_ms) < SETTLE_MS) return false;
    PWM_StartSPWM(m_ac_freq_hz, m_ac_mod_index);
    m_spwm_start_cycles = PWM_GetSPWMElectricalCycles();
    m_sigma_i_peak = 0.0f;
    m_sigma_samples = 0;
    enterState(State::SIGMA_LS_MEASURE);
    return true;
}

bool InductionMotorCalibrator::runSigmaLsMeasure() {
    float iu, iv, iw;
    sampleCurrents(iu, iv, iw);
    if (checkOvercurrent(iu, iv, iw)) return true;

    const float i_peak = maxAbs3(iu, iv, iw);
    if (i_peak > m_sigma_i_peak) m_sigma_i_peak = i_peak;
    ++m_sigma_samples;

    const uint32_t cycles = PWM_GetSPWMElectricalCycles() - m_spwm_start_cycles;
    if (cycles < AC_MEASURE_CYCLES) return false;

    PWM_StopSPWM();

    const float vdc = dcLinkVoltageSensor().voltage();
    const float vph_peak = m_ac_mod_index * 0.5f * vdc;
    const MotorCalibration& mc = motorCalibration();

    Telemetry::printf("[CAL] INDUCTION: sigma_Ls raw Vph=%.3f V Ipeak=%.3f A samples=%lu",
                      static_cast<double>(vph_peak),
                      static_cast<double>(m_sigma_i_peak),
                      static_cast<unsigned long>(m_sigma_samples));

    if (m_sigma_i_peak < 0.1f) {
        fail("AC current too small (%.2f A); raise ac_voltage_pct",
             static_cast<double>(m_sigma_i_peak));
        return true;
    }

    const float z_mag = vph_peak / m_sigma_i_peak;
    const float r_s = mc.r_phase_avg;
    const float omega = 2.0f * PI_F * m_ac_freq_hz;
    const float z2r2 = z_mag * z_mag - r_s * r_s;

    if (z2r2 <= 0.0f || !std::isfinite(z2r2)) {
        fail("nonphysical impedance |Z|=%.4f ohm <= Rs=%.4f ohm",
             static_cast<double>(z_mag), static_cast<double>(r_s));
        return true;
    }

    m_sigma_ls_h = std::sqrt(z2r2) / omega;
    Telemetry::printf("[CAL] INDUCTION: sigma_Ls = %.1f uH  (|Z|=%.4f ohm)",
                      static_cast<double>(m_sigma_ls_h * 1.0e6f),
                      static_cast<double>(z_mag));

    /* Prepare for DC fluxing: park at zero vector, then let PI ramp up. */
    PWM_SetThreePhaseDuty(50.0f, 50.0f, 50.0f);
    m_flux_mod_index = 0.0f;
    m_flux_integral = 0.0f;
    m_flux_pi_last_ms = 0;
    enterState(State::TAU_R_FLUXING);
    return true;
}

bool InductionMotorCalibrator::runTauRFluxing() {
    float iu, iv, iw;
    sampleCurrents(iu, iv, iw);
    if (checkOvercurrent(iu, iv, iw)) return true;
    if (checkEncoderMovement()) return true;

    const float i_meas = maxAbs3(iu, iv, iw);
    updateFluxPi(i_meas);

    /* Wait until the current has reached the target and settled. */
    const uint32_t elapsed = HAL_GetTick() - m_state_enter_ms;
    if (elapsed < SETTLE_MS) return false;
    if (i_meas < 0.5f * m_max_flux_current_a) {
        if (elapsed > 5000U) {
            fail("fluxing current did not reach target (%.1f A < %.1f A)",
                 static_cast<double>(i_meas),
                 static_cast<double>(0.5f * m_max_flux_current_a));
            return true;
        }
        return false;
    }

    Telemetry::printf("[CAL] INDUCTION: fluxing at %.1f A (mod=%.3f)",
                      static_cast<double>(i_meas),
                      static_cast<double>(m_flux_mod_index));
    enterState(State::TAU_R_SETTLE);
    return true;
}

bool InductionMotorCalibrator::runTauRSettle() {
    float iu, iv, iw;
    sampleCurrents(iu, iv, iw);
    if (checkOvercurrent(iu, iv, iw)) return true;
    if (checkEncoderMovement()) return true;

    /* Keep the PI loop alive so the current stays at the target. */
    updateFluxPi(maxAbs3(iu, iv, iw));

    if ((HAL_GetTick() - m_state_enter_ms) < m_fluxing_time_ms) return false;

    /* Short all three phases (low-side on) to start the current decay. */
    PWM_SetThreePhaseDuty(0.0f, 0.0f, 0.0f);
    m_decay_n = 0;
    m_last_decay_ms = 0;
    enterState(State::TAU_R_DECAY);
    return true;
}

bool InductionMotorCalibrator::runTauRDecay() {
    const uint32_t now = HAL_GetTick();
    if (m_last_decay_ms != 0U && (now - m_last_decay_ms) < 10U) return false;

    float iu, iv, iw;
    sampleCurrents(iu, iv, iw);
    if (checkOvercurrent(iu, iv, iw)) return true;
    if (checkEncoderMovement()) return true;

    const float i_meas = maxAbs3(iu, iv, iw);
    if (m_decay_n < MAX_DECAY_SAMPLES) {
        m_decay_t_ms[m_decay_n] = static_cast<float>(now - m_state_enter_ms);
        m_decay_i_a[m_decay_n] = i_meas;
        ++m_decay_n;
    }
    m_last_decay_ms = now;

    if ((now - m_state_enter_ms) >= m_decay_sample_time_ms) {
        enterState(State::COMPUTE);
        return true;
    }
    return false;
}

bool InductionMotorCalibrator::runCompute() {
    if (m_decay_n < 10) {
        fail("not enough decay samples (%d)", m_decay_n);
        return true;
    }

    /* Estimate the current offset from the tail of the recording. */
    float offset = 0.0f;
    const int tail = std::min(10, m_decay_n);
    for (int i = m_decay_n - tail; i < m_decay_n; ++i) offset += m_decay_i_a[i];
    offset /= static_cast<float>(tail);

    /* Log-linear fit over the decay samples that are well above the offset. */
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    int n_fit = 0;
    for (int i = 0; i < m_decay_n; ++i) {
        const float y = m_decay_i_a[i] - offset;
        if (y <= 0.01f) continue;
        const double t = static_cast<double>(m_decay_t_ms[i]);
        const double ln_y = std::log(static_cast<double>(y));
        sx += t; sy += ln_y; sxx += t * t; sxy += t * ln_y;
        ++n_fit;
    }

    if (n_fit < 5) {
        fail("decay did not have enough usable samples for fit");
        return true;
    }

    const double n = static_cast<double>(n_fit);
    const double denom = n * sxx - sx * sx;
    if (std::fabs(denom) < 1.0e-9) {
        fail("degenerate decay fit");
        return true;
    }

    const double slope = (n * sxy - sx * sy) / denom;
    if (slope >= 0.0) {
        fail("decay fit slope non-negative (%.4f)", static_cast<double>(slope));
        return true;
    }

    m_rotor_tau_ms = -1.0f / static_cast<float>(slope);
    if (m_rotor_tau_ms < MIN_DECAY_FIT_TAU_MS || !std::isfinite(m_rotor_tau_ms)) {
        fail("rotor time constant out of range (%.2f ms)",
             static_cast<double>(m_rotor_tau_ms));
        return true;
    }

    Telemetry::printf("[CAL] INDUCTION: tau_r = %.1f ms  (fit samples=%d)",
                      static_cast<double>(m_rotor_tau_ms), n_fit);

    /* Derive Lr, Rr', Lm and leakage from sigma_Ls, tau_r and optional Lm. */
    if (m_lm_estimate_h > 0.0f) {
        /* y = Ls = Lr under the Lls=Llr' assumption.
         * sigma_Ls = y - Lm^2 / y  =>  y^2 - sigma_Ls*y - Lm^2 = 0 */
        const float s = m_sigma_ls_h;
        const float l = m_lm_estimate_h;
        const float disc = std::sqrt(s * s + 4.0f * l * l);
        const float y = 0.5f * (s + disc);
        m_lr_h = y;
        m_lm_h = l;
        m_l_leak_h = y - l;
        m_rr_ohm = y / (m_rotor_tau_ms * 1.0e-3f);
        Telemetry::printf("[CAL] INDUCTION: derived Lm=%.1f uH Lr=%.1f uH "
                          "Ll=%.1f uH Rr'=%.4f ohm",
                          static_cast<double>(m_lm_h * 1.0e6f),
                          static_cast<double>(m_lr_h * 1.0e6f),
                          static_cast<double>(m_l_leak_h * 1.0e6f),
                          static_cast<double>(m_rr_ohm));
    }

    /* Publish into the runtime calibration. */
    MotorCalibration& mc = motorCalibration();
    mc.motor_type = MotorType::Induction;
    mc.sigma_ls_henry = m_sigma_ls_h;
    mc.rotor_time_constant_ms = m_rotor_tau_ms;
    if (m_lm_h > 0.0f) mc.lm_henry = m_lm_h;
    if (m_lr_h > 0.0f) mc.lr_henry = m_lr_h;
    if (m_rr_ohm > 0.0f) mc.rr_ohm = m_rr_ohm;
    if (m_l_leak_h > 0.0f) mc.l_leak_henry = m_l_leak_h;

    shutdown();

    Telemetry::printf("[CAL] INDUCTION: ========================================");
    Telemetry::printf("[CAL] INDUCTION: INDUCTION CALIBRATION COMPLETE");
    Telemetry::printf("[CAL] INDUCTION:   sigma_Ls = %.1f uH",
                      static_cast<double>(m_sigma_ls_h * 1.0e6f));
    Telemetry::printf("[CAL] INDUCTION:   tau_r    = %.1f ms",
                      static_cast<double>(m_rotor_tau_ms));
    if (m_lm_h > 0.0f) {
        Telemetry::printf("[CAL] INDUCTION:   Lm       = %.1f uH",
                          static_cast<double>(m_lm_h * 1.0e6f));
        Telemetry::printf("[CAL] INDUCTION:   Lr       = %.1f uH",
                          static_cast<double>(m_lr_h * 1.0e6f));
        Telemetry::printf("[CAL] INDUCTION:   Rr'      = %.4f ohm",
                          static_cast<double>(m_rr_ohm));
    }
    Telemetry::printf("[CAL] INDUCTION: ========================================");

    Telemetry::log("ind_sigma_ls_uh", m_sigma_ls_h * 1.0e6f);
    Telemetry::log("ind_tau_r_ms", m_rotor_tau_ms);
    enterState(State::DONE);
    return true;
}

void InductionMotorCalibrator::update() {
    if (!isActive()) return;

    if (FaultManager::instance().isSeverityActive(FaultSeverity::Critical) ||
        FaultManager::instance().isSeverityActive(FaultSeverity::High)) {
        fail("Critical/High fault detected");
        return;
    }

    switch (m_state) {
        case State::ENABLE: {
            m_hw.update();
            if (m_hw.hasFailed()) {
                fail("calibration hardware failed (gate driver not ready)");
                return;
            }
            if (!m_hw.isReady()) return;
            Telemetry::printf("[CAL] INDUCTION: hardware ready");
            enterState(State::SIGMA_LS_SETTLE);
            return;
        }

        case State::SIGMA_LS_SETTLE:
            runSigmaLsSettle();
            return;

        case State::SIGMA_LS_MEASURE:
            runSigmaLsMeasure();
            return;

        case State::TAU_R_FLUXING:
            runTauRFluxing();
            return;

        case State::TAU_R_SETTLE:
            runTauRSettle();
            return;

        case State::TAU_R_DECAY:
            runTauRDecay();
            return;

        case State::COMPUTE:
            runCompute();
            return;

        default:
            return;
    }
}

} // namespace Inverter
