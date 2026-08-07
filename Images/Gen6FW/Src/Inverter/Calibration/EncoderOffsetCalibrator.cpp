#include "Inverter/Calibration/EncoderOffsetCalibrator.h"
#include "Inverter/Control/OpenLoopController.h"
#include "Inverter/Control/FaultManager.h"
#include "Inverter/Drivers/PWM/pwm.h"
#include "Inverter/Drivers/Sensors/EncoderADC.h"
#include "Inverter/Drivers/Sensors/DcLinkVoltageSensor.h"
#include "Inverter/Telemetry.h"

#include "main.h"
#include <cmath>
#include <cstdarg>

namespace Inverter {

static constexpr float CAL_MAX_MOD = 0.50f;
static constexpr float HOLD_MAX_MOD = 0.50f;

/* Voltage-aware breakaway/rotation limits.  Modulation index is peak phase
 * voltage divided by (Vdc/2), so m = 2*V/Vdc.  Using volts instead of a fixed
 * modulation step makes the ramp behave similarly at 20 V and 120 V. */
static constexpr float BREAKAWAY_STEP_V = 0.50f;        /* V per ramp period */
static constexpr float BREAKAWAY_MAX_V = 10.0f;         /* peak phase voltage */
static constexpr float ROTATE_HEADROOM_V = 3.0f;        /* V above breakaway */
static constexpr float CAL_MAX_VOLTAGE_V = 25.0f;       /* hard voltage ceiling */

static constexpr uint32_t WARMUP_MS = 5000U;
static constexpr float WARMUP_FREQUENCY_HZ = 1.0f;
static constexpr float OFFSET_ROTATION_FREQUENCY_HZ = 1.0f;
static constexpr float OFFSET_ROTATE_REVS = 3.0f;
static constexpr float OFFSET_ACQUIRE_START_DEG = 180.0f;
static constexpr float FIT_GIVE_UP_DEG = 360.0f;
static constexpr uint32_t OFFSET_SAMPLE_PERIOD_MS = 10U;

static constexpr float NOISE_THRESHOLD_CYCLES = 0.02f;
static constexpr float BREAKAWAY_DETECT_CYCLES = 0.15f;
static constexpr uint32_t BREAKAWAY_DETECT_DWELL_MS = 100U;
static constexpr uint32_t RAMP_PERIOD_MS = 200U;
static constexpr uint32_t MOVE_TIMEOUT_MS = 3000U;
static constexpr uint32_t FIND_VOLTAGE_TIMEOUT_MS = 15000U;
static constexpr uint32_t OFFSET_ROTATE_TIMEOUT_MS = 60000U;
static constexpr uint32_t RAMP_PAUSE_TIMEOUT_MS = 200U;

static constexpr float VOLTAGE_ANGLE_U_HIGH_RAD = 1.57079632679f;
static constexpr float RAD_TO_DEG = 57.2957795131f;

static EncoderOffsetCalibrator s_instance;

static float voltageToMod(float v_v, float vdc_v) {
    return (vdc_v > 1.0f) ? (v_v * 2.0f / vdc_v) : 0.0f;
}

static float modToVoltage(float m, float vdc_v) {
    return m * vdc_v * 0.5f;
}

EncoderOffsetCalibrator& EncoderOffsetCalibrator::instance() {
    return s_instance;
}

static const char* stateName(EncoderOffsetCalibrator::State state) {
    switch (state) {
        case EncoderOffsetCalibrator::State::IDLE:          return "IDLE";
        case EncoderOffsetCalibrator::State::HW_INIT:       return "HW_INIT";
        case EncoderOffsetCalibrator::State::WAIT_READY:    return "WAIT_READY";
        case EncoderOffsetCalibrator::State::WARMUP:        return "WARMUP";
        case EncoderOffsetCalibrator::State::FIND_VOLTAGE:  return "FIND_VOLTAGE";
        case EncoderOffsetCalibrator::State::OFFSET_ROTATE: return "OFFSET_ROTATE";
        case EncoderOffsetCalibrator::State::DONE:          return "DONE";
        case EncoderOffsetCalibrator::State::FAIL:          return "FAIL";
    }
    return "?";
}

bool EncoderOffsetCalibrator::start(float pole_count, float encoder_cycles_per_rev,
                                     float breakaway_mod, float max_mod,
                                     float torque_margin) {
    if (isActive()) {
        Telemetry::printf("[CAL] ENC: already active");
        return false;
    }

    if (pole_count <= 0.0f || encoder_cycles_per_rev <= 0.0f) {
        Telemetry::printf("[CAL] ENC: ERROR: invalid pole_count/encoder_cycles");
        return false;
    }

    m_poles = pole_count;
    m_pole_pairs = pole_count / 2.0f;
    m_encoder_cycles_per_rev = encoder_cycles_per_rev;
    m_mech_deg_per_motor_elec_cycle = 360.0f / m_pole_pairs;
    m_breakaway_mod = breakaway_mod;
    m_max_mod = (max_mod > 0.0f) ? max_mod : 0.50f;
    m_torque_margin = (torque_margin > 0.0f) ? torque_margin : 0.40f;

    if (m_pole_pairs <= 0.0f) {
        Telemetry::printf("[CAL] ENC: ERROR: pole_pairs must be > 0");
        return false;
    }

    if (!openLoopController().isInitialized()) {
        openLoopController().init();
    }

    encoderADC().resetBounds();
    encoderADC().learnBounds(true);
    encoderADC().stopFitCapture();
    m_tracker.reset();
    m_hw.begin();

    enterState(State::HW_INIT);
    return true;
}

void EncoderOffsetCalibrator::enterState(State state) {
    m_state = state;
    m_state_start_ms = HAL_GetTick();

    Telemetry::printf("[CAL] ENC DBG: enter %s", stateName(state));

    switch (state) {
        case State::HW_INIT:
            /* Hardware startup is driven by CalibrationHardware; nothing else
             * to set up here. */
            break;

        case State::FIND_VOLTAGE: {
            PWM_ResetSPWMElectricalCycles();
            const float vdc = dcLinkVoltageSensor().voltage();
            const float step_mod = voltageToMod(BREAKAWAY_STEP_V, vdc);
            const float max_v = std::min(BREAKAWAY_MAX_V, vdc * 0.5f);
            const float max_mod_search = voltageToMod(max_v, vdc);
            m_breakaway.start(step_mod, RAMP_PERIOD_MS, max_mod_search,
                              BREAKAWAY_DETECT_CYCLES, 1.0f, MOVE_TIMEOUT_MS,
                              BREAKAWAY_DETECT_DWELL_MS);
            PWM_StartSPWM(OFFSET_ROTATION_FREQUENCY_HZ, 0.0f);
            break;
        }

        case State::WARMUP: {
            m_tracker.reset();
            const float vdc = dcLinkVoltageSensor().voltage();
            const float v_breakaway = modToVoltage(m_breakaway_mod, vdc);
            float target = m_mod;
            const float floor_mod = voltageToMod(v_breakaway, vdc);
            /* Never warm up at a voltage below breakaway; keep a small headroom
             * so the rotor stays locked. */
            const float min_target_mod = voltageToMod(v_breakaway + 0.5f, vdc);
            if (target < min_target_mod) {
                target = min_target_mod;
            }
            const float max_mod = effectiveMaxMod(vdc);
            if (target > max_mod) {
                target = max_mod;
            }
            m_ramp.start(0.0f, target, 4000U, openLoopController().rampCurrentLimit(), floor_mod);
            PWM_ResetSPWMElectricalCycles();
            PWM_StartSPWM(WARMUP_FREQUENCY_HZ, 0.0f);

            m_warmup_sign = 0;
            m_warmup_enc_start = m_tracker.unwrappedDegrees() / m_encoder_cycles_per_rev;
            m_warmup_fld_start = fieldMechanicalAngle();
            break;
        }

        case State::OFFSET_ROTATE:
            PWM_ResetSPWMElectricalCycles();
            PWM_StartSPWM(OFFSET_ROTATION_FREQUENCY_HZ, 0.0f);
            /* Reset the tracker here so the offset measurement starts from the
             * same instant as the field angle.  This removes any scale/offset
             * error accumulated during the warmup phase. */
            m_tracker.reset();
            m_rotate_start_encoder = 0.0f;
            m_rotate_start_field = fieldMechanicalAngle();
            m_offset_acquisition_active = false;
            m_acquire_start_encoder_elec_deg = 0.0f;
            m_acquire_start_field_mech_deg = 0.0f;
            m_sign = 0;
            m_first_encoder_mech = 0.0f;
            m_first_field_mech = 0.0f;
            m_last_encoder_mech = 0.0f;
            m_last_field_mech = 0.0f;
            m_sum_offset = 0.0;
            m_sample_count = 0;
            m_last_offset = 0.0f;
            m_last_offset_sample_ms = 0;
            m_fit = EncoderADC::SinCosFit();
            m_fit_checked_at_start = false;
            m_fit_checked_at_giveup = false;
            /* Start collecting raw sin/cos for the Layer 1 ellipse fit
             * immediately.  The fit must be computed and applied *before*
             * offset acquisition so that the stored offset matches the same
             * angle decoding the runtime will use. */
            encoderADC().startFitCapture();
            {
                const float vdc = dcLinkVoltageSensor().voltage();
                const float v_breakaway = modToVoltage(m_breakaway_mod, vdc);
                float target = m_mod;
                const float floor_mod = voltageToMod(v_breakaway, vdc);
                const float min_target_mod = voltageToMod(v_breakaway + 0.5f, vdc);
                if (target < min_target_mod) {
                    target = min_target_mod;
                }
                const float max_mod = effectiveMaxMod(vdc);
                if (target > max_mod) {
                    target = max_mod;
                }
                m_ramp.start(0.0f, target, 4000U, openLoopController().rampCurrentLimit(), floor_mod);
            }
            break;

        case State::DONE:
        case State::FAIL:
        case State::WAIT_READY:
        default:
            break;
    }
}

void EncoderOffsetCalibrator::restoreHardware() {
    CalibrationHardware::shutdown();
}

void EncoderOffsetCalibrator::stop() {
    if (m_state != State::IDLE && m_state != State::DONE && m_state != State::FAIL) {
        PWM_StopSPWM();
        restoreHardware();
        encoderADC().learnBounds(false);
        encoderADC().stopFitCapture();
        m_state = State::IDLE;
        Telemetry::printf("[CAL] ENC: stopped by user");
    }
}

void EncoderOffsetCalibrator::fail(const char* reason_fmt, ...) {
    va_list args;
    va_start(args, reason_fmt);
    Telemetry::vprintf(reason_fmt, args);
    va_end(args);

    restoreHardware();
    encoderADC().learnBounds(false);
    encoderADC().stopFitCapture();
    m_state = State::FAIL;
}

void EncoderOffsetCalibrator::sample() {
    uint16_t raw_sin = 0U;
    uint16_t raw_cos = 0U;
    float angle = 0.0f;
    if (encoderADC().sample(angle, raw_sin, raw_cos)) {
        m_tracker.update();
        m_last_raw_sin = raw_sin;
        m_last_raw_cos = raw_cos;
    }
}

float EncoderOffsetCalibrator::encoderMechanicalAngle() const {
    return m_tracker.unwrappedDegrees() / m_encoder_cycles_per_rev;
}

float EncoderOffsetCalibrator::fieldMechanicalAngle() const {
    const float spwm_angle = PWM_GetSPWMAngle();
    const uint32_t cycles = PWM_GetSPWMElectricalCycles();

    /* Field mechanical angle relative to the U-high vector. */
    float field_elec_deg = static_cast<float>(cycles) * 360.0f + spwm_angle * RAD_TO_DEG;
    field_elec_deg -= VOLTAGE_ANGLE_U_HIGH_RAD * RAD_TO_DEG;
    return field_elec_deg / m_pole_pairs;
}

float EncoderOffsetCalibrator::wrapOffset(float offset, float period) {
    float wrapped = offset - period * std::floor(offset / period);
    if (wrapped < 0.0f) {
        wrapped += period;
    }
    if (wrapped >= period) {
        wrapped -= period;
    }
    return wrapped;
}

float EncoderOffsetCalibrator::effectiveMaxMod(float vdc_v) const {
    if (vdc_v <= 1.0f) {
        return m_max_mod;
    }
    /* Clamp to a hard peak-phase-voltage ceiling so high bus voltages do not
     * translate into huge modulation/current spikes.  At low bus voltages the
     * same ceiling allows the full inverter range (m up to 1.0). */
    const float max_mod_from_voltage = CAL_MAX_VOLTAGE_V * 2.0f / vdc_v;
    return std::min(m_max_mod, max_mod_from_voltage);
}

static bool encoderAngleReliable() {
    const auto& enc = encoderADC();
    return enc.boundsValid() &&
           (enc.sinMax() - enc.sinMin() > 10000U) &&
           (enc.cosMax() - enc.cosMin() > 10000U);
}

void EncoderOffsetCalibrator::accumulateOffsetSample() {
    if (!encoderAngleReliable()) {
        return;
    }

    const float encoder_raw_deg = encoderADC().lastAngle();
    const float field_mech_deg = fieldMechanicalAngle();

    /* The physical offset is the encoder angle when the stator field points
     * at U-high.  Use the raw absolute encoder angle (0..360) and unwrap it
     * against the known continuous field angle; this avoids any drift from
     * the movement tracker starting at an arbitrary field angle.
     *
     * FOC computes theta_elec = offset + sign * encoder_angle and requires
     * theta_elec == field angle when the rotor is locked to the field, so the
     * stored offset must be (field - sign * encoder):
     *   sign = -1 -> encoder + field
     *   sign = +1 -> field - encoder */
    float offset = (m_sign < 0)
                       ? (encoder_raw_deg + field_mech_deg)
                       : (field_mech_deg - encoder_raw_deg);

    /* The offset is only unique modulo (360° / pole_pairs) because the
     * motor has multiple pole pairs.  Keep successive samples in the same
     * branch so a boundary does not corrupt the average. */
    if (m_sample_count > 0) {
        const float half_period = 0.5f * m_mech_deg_per_motor_elec_cycle;
        while ((offset - m_last_offset) > half_period) {
            offset -= m_mech_deg_per_motor_elec_cycle;
        }
        while ((m_last_offset - offset) > half_period) {
            offset += m_mech_deg_per_motor_elec_cycle;
        }
    }

    m_sum_offset += static_cast<double>(offset);
    m_last_offset = offset;
    ++m_sample_count;
}

void EncoderOffsetCalibrator::update() {
    if (m_state == State::IDLE || m_state == State::DONE || m_state == State::FAIL) {
        return;
    }

    sample();
    const uint32_t now_ms = HAL_GetTick();

    switch (m_state) {
        case State::HW_INIT: {
            m_hw.update();
            if (m_hw.hasFailed()) {
                fail("[CAL] ENC: ERROR: gate driver not ready or fault latched");
            } else if (m_hw.isReady()) {
                PWM_ClearFault();
                PWM_StartPhase(0);
                PWM_StartPhase(1);
                PWM_StartPhase(2);

                Telemetry::printf("[CAL] ENC: started for %.0f poles (%.0f pole pairs), encoder_cycles=%.2f",
                                  static_cast<double>(m_poles),
                                  static_cast<double>(m_pole_pairs),
                                  static_cast<double>(m_encoder_cycles_per_rev));

                if (m_breakaway_mod > 0.0f) {
                    m_mod = m_breakaway_mod * m_torque_margin;
                    if (m_mod > m_max_mod) {
                        m_mod = m_max_mod;
                    }
                    Telemetry::printf("[CAL] ENC: using provided breakaway mod %.3f -> rotate mod %.3f",
                                      static_cast<double>(m_breakaway_mod),
                                      static_cast<double>(m_mod));
                    enterState(State::WARMUP);
                } else {
                    m_mod = 0.0f;
                    enterState(State::FIND_VOLTAGE);
                }
            }
            break;
        }

        case State::WARMUP: {
            const auto ramp_status = m_ramp.update(now_ms);
            if (ramp_status == CurrentLimitedRamp::Status::ABORTED) {
                fail("[CAL] ENC: FAIL: warmup current limit hit");
                break;
            }

            PWM_SetSPWMParams(WARMUP_FREQUENCY_HZ, m_ramp.applied());

            const float enc_mech = m_tracker.unwrappedDegrees() / m_encoder_cycles_per_rev;
            const float moved_enc = std::fabs(enc_mech - m_warmup_enc_start);
            const float moved_field = std::fabs(fieldMechanicalAngle() - m_warmup_fld_start);

            const float warmup_lock_ratio =
                (moved_field > 5.0f) ? (std::fabs(moved_enc) / moved_field) : 0.0f;
            if ((now_ms - m_last_dbg_ms) >= 500U) {
                m_last_dbg_ms = now_ms;
                Telemetry::printf("[CAL] ENC DBG: warmup: enc_mech=%.3f mod=%.3f enc_moved=%.1f fld_moved=%.1f lock=%.2f ms=%lu",
                                  static_cast<double>(enc_mech),
                                  static_cast<double>(m_ramp.applied()),
                                  static_cast<double>(moved_enc),
                                  static_cast<double>(moved_field),
                                  static_cast<double>(warmup_lock_ratio),
                                  static_cast<unsigned long>(now_ms - m_state_start_ms));
            }

            const bool time_done = (now_ms - m_state_start_ms) >= WARMUP_MS;
            const bool rev_done = moved_field >= 360.0f;
            if (time_done && rev_done) {
                /* Capture final field angle before stopping SPWM, because
                 * PWM_GetSPWMAngle() returns 0 while SPWM is stopped. */
                const float warmup_enc_end = m_tracker.unwrappedDegrees() / m_encoder_cycles_per_rev;
                const float warmup_fld_end = fieldMechanicalAngle();

                PWM_StopSPWM();
                Telemetry::printf("[CAL] ENC DBG: warmup done: enc_moved=%.1f fld_moved=%.1f deg",
                                  static_cast<double>(moved_enc),
                                  static_cast<double>(moved_field));

                /* Determine the encoder/field direction from the whole warmup
                 * revolution; this is much more robust than the first few
                 * degrees after the PWM restart in OFFSET_ROTATE. */
                const float d_enc = warmup_enc_end - m_warmup_enc_start;
                const float d_fld = warmup_fld_end - m_warmup_fld_start;
                if (std::fabs(d_enc) >= 180.0f && std::fabs(d_fld) >= 180.0f) {
                    m_warmup_sign = (d_enc * d_fld > 0.0f) ? 1 : -1;
                    Telemetry::printf("[CAL] ENC DBG: warmup direction d_enc=%.1f d_fld=%.1f sign=%d",
                                      static_cast<double>(d_enc),
                                      static_cast<double>(d_fld),
                                      m_warmup_sign);
                }

                enterState(State::OFFSET_ROTATE);
            } else if (time_done && !rev_done) {
                if ((now_ms - m_state_start_ms) > (WARMUP_MS + 5000U)) {
                    PWM_StopSPWM();
                    fail("[CAL] ENC: FAIL: warmup did not produce a full revolution (enc_moved %.1f fld_moved %.1f deg)",
                         static_cast<double>(moved_enc),
                         static_cast<double>(moved_field));
                }
            }
            break;
        }

        case State::FIND_VOLTAGE: {
            const float angle_cycles = m_tracker.mechanicalCycles();
            float mod = 0.0f;
            const auto status = m_breakaway.update(now_ms, std::fabs(angle_cycles), mod);

            if (status == BreakawayFinder::Status::RUNNING) {
                PWM_SetSPWMParams(OFFSET_ROTATION_FREQUENCY_HZ, mod);

                /* Floating/disconnected encoder inputs often produce smooth
                 * phantom rotation while the rotor is stationary.  If we see
                 * significant encoder movement before the applied voltage is
                 * high enough to cause real motion, abort with a clear message
                 * instead of ramping to max_mod and timing out. */
                if (m_breakaway.phantomMotionDetected()) {
                    PWM_StopSPWM();
                    fail("[CAL] ENC: FAIL: encoder shows motion with no applied voltage (floating inputs?)");
                    return;
                }
            } else if (status == BreakawayFinder::Status::TIMEOUT) {
                PWM_StopSPWM();
                fail("[CAL] ENC: FAIL: rotor did not move during voltage ramp");
                return;
            } else {
                /* FOUND */
                m_breakaway_mod = m_breakaway.breakawayMod();
                const float vdc = dcLinkVoltageSensor().voltage();
                const float v_breakaway = modToVoltage(m_breakaway_mod, vdc);
                const float v_rotate = v_breakaway + ROTATE_HEADROOM_V;
                m_mod = voltageToMod(v_rotate, vdc);
                const float max_mod = effectiveMaxMod(vdc);
                if (m_mod > max_mod) m_mod = max_mod;
                if (m_mod < 0.0f) m_mod = 0.0f;
                PWM_StopSPWM();
                Telemetry::printf("[CAL] ENC: breakaway at mod=%.3f (%.2f V) -> rotate mod=%.3f (%.2f V)",
                                  static_cast<double>(m_breakaway_mod),
                                  static_cast<double>(v_breakaway),
                                  static_cast<double>(m_mod),
                                  static_cast<double>(v_rotate));
                enterState(State::WARMUP);
                return;
            }

            if ((now_ms - m_state_start_ms) > FIND_VOLTAGE_TIMEOUT_MS) {
                PWM_StopSPWM();
                fail("[CAL] ENC: FAIL: voltage ramp timed out");
                return;
            }
            break;
        }

        case State::OFFSET_ROTATE: {
            const auto ramp_status = m_ramp.update(now_ms);
            if (ramp_status == CurrentLimitedRamp::Status::ABORTED) {
                fail("[CAL] ENC: FAIL: rotation current limit hit");
                break;
            }

            PWM_SetSPWMParams(OFFSET_ROTATION_FREQUENCY_HZ, m_ramp.applied());

            const float field_mech = fieldMechanicalAngle();
            const float moved_field = std::fabs(field_mech - m_rotate_start_field);
            const float encoder_mech = m_tracker.unwrappedDegrees() / m_encoder_cycles_per_rev;
            const bool ramp_done = (ramp_status == CurrentLimitedRamp::Status::DONE);

            if ((now_ms - m_last_dbg_ms) >= 500U) {
                m_last_dbg_ms = now_ms;
                Telemetry::printf("[CAL] ENC DBG: rotate: enc_mech=%.3f fld_mech=%.3f moved=%.1f samples=%d",
                                  static_cast<double>(encoder_mech),
                                  static_cast<double>(field_mech),
                                  static_cast<double>(moved_field),
                                  m_sample_count);
            }

            if (!m_offset_acquisition_active) {
                if (m_sign == 0) {
                    /* Use the robust warmup direction if available; otherwise
                     * fall back to observing a larger initial motion in this
                     * state so noise/transients do not flip the sign. */
                    if (m_warmup_sign != 0) {
                        m_sign = m_warmup_sign;
                        Telemetry::printf("[CAL] ENC DBG: using warmup sign=%d", m_sign);
                    } else if (m_first_encoder_mech == 0.0f && m_first_field_mech == 0.0f) {
                        m_first_encoder_mech = encoder_mech;
                        m_first_field_mech = field_mech;
                    } else {
                        const float d_enc = encoder_mech - m_first_encoder_mech;
                        const float d_fld = field_mech - m_first_field_mech;
                        if (std::fabs(d_enc) >= 45.0f && std::fabs(d_fld) >= 45.0f) {
                            m_sign = (d_enc * d_fld > 0.0f) ? 1 : -1;
                            Telemetry::printf("[CAL] ENC DBG: detected sign=%d (d_enc=%.2f d_fld=%.2f)",
                                              m_sign,
                                              static_cast<double>(d_enc),
                                              static_cast<double>(d_fld));
                        }
                    }
                }

                /* Slip monitor: when locked, encoder tracks field movement 1:1
                 * (both are rotor mechanical degrees).  Persistent low ratio
                 * means the stator field is spinning without pulling the rotor
                 * along — usually too little torque for the load/inertia. */
                const float rotate_lock_ratio =
                    (moved_field > 5.0f) ? (std::fabs(encoder_mech) / moved_field) : 0.0f;
                if (moved_field > 90.0f && rotate_lock_ratio < 0.5f && m_sign == 0) {
                    Telemetry::printf("[CAL] ENC DBG: slip detected (lock_ratio=%.2f, enc=%.1f fld=%.1f)",
                                      static_cast<double>(rotate_lock_ratio),
                                      static_cast<double>(encoder_mech),
                                      static_cast<double>(moved_field));
                }
                if (moved_field > 450.0f && !m_offset_acquisition_active) {
                    PWM_StopSPWM();
                    fail("[CAL] ENC: FAIL: rotor did not lock after %.1f deg field rotation (insufficient torque?)",
                         static_cast<double>(moved_field));
                    return;
                }

                if (ramp_done && moved_field >= OFFSET_ACQUIRE_START_DEG && m_sign != 0) {
                    /* Compute and apply the Layer 1 ellipse fit *before* measuring
                     * the offset.  The runtime decoder uses the fit (if valid), so
                     * the stored offset must refer to the fitted angle, not the
                     * uncorrected angle used during the first part of this routine.
                     * We attempt the fit once after a full locked revolution; if it
                     * still fails after an additional sweep we proceed without it. */
                    if (!m_fit.valid && !m_fit_checked_at_start) {
                        m_fit_checked_at_start = true;
                        EncoderADC::SinCosFit fit;
                        if (encoderADC().computeFit(fit)) {
                            encoderADC().applyFit(fit);
                            m_fit = fit;
                            const float phi_deg = std::fabs(fit.phase_err) * RAD_TO_DEG;
                            Telemetry::printf("[CAL] ENC: fit applied before offset acquisition (phi=%.3f deg, samples=%lu)",
                                              static_cast<double>(phi_deg),
                                              static_cast<unsigned long>(fit.sample_count));
                        } else {
                            /* Not enough good data yet; keep collecting for the
                             * second attempt closer to the give-up threshold. */
                            encoderADC().startFitCapture();
                        }
                    }
                    if (!m_fit.valid && !m_fit_checked_at_giveup && moved_field >= FIT_GIVE_UP_DEG) {
                        m_fit_checked_at_giveup = true;
                        EncoderADC::SinCosFit fit;
                        if (encoderADC().computeFit(fit)) {
                            encoderADC().applyFit(fit);
                            m_fit = fit;
                            const float phi_deg = std::fabs(fit.phase_err) * RAD_TO_DEG;
                            Telemetry::printf("[CAL] ENC: fit applied before offset acquisition (late, phi=%.3f deg, samples=%lu)",
                                              static_cast<double>(phi_deg),
                                              static_cast<unsigned long>(fit.sample_count));
                        } else {
                            encoderADC().stopFitCapture();
                            Telemetry::printf("[CAL] ENC: fit not usable, proceeding without Layer 1 correction");
                        }
                    }

                    if (m_fit.valid || (m_fit_checked_at_giveup && !m_fit.valid)) {
                        m_offset_acquisition_active = true;
                        m_last_encoder_mech = encoder_mech;
                        m_last_field_mech = field_mech;
                        /* Record the start of the locked region so cycles/rev can
                         * be measured only over verified-locked motion. */
                        m_acquire_start_encoder_elec_deg = m_tracker.unwrappedDegrees();
                        m_acquire_start_field_mech_deg = field_mech;
                        /* Reset the offset accumulator now that the decoder is
                         * in its final configuration. */
                        m_sum_offset = 0.0;
                        m_sample_count = 0;
                        m_last_offset = 0.0f;
                        m_last_offset_sample_ms = now_ms;
                        Telemetry::printf("[CAL] ENC DBG: acquisition started (fit=%s)",
                                          m_fit.valid ? "active" : "none");
                    }
                }
            } else {
                if ((now_ms - m_last_offset_sample_ms) >= OFFSET_SAMPLE_PERIOD_MS) {
                    m_last_offset_sample_ms = now_ms;
                    accumulateOffsetSample();
                }
            }

            const float target_mech = OFFSET_ROTATE_REVS * 360.0f;
            if (moved_field >= target_mech) {
                PWM_StopSPWM();
                if (m_sample_count == 0) {
                    fail("[CAL] ENC: FAIL: rotation finished with no samples");
                    return;
                }
                const float avg_offset =
                    wrapOffset(static_cast<float>(m_sum_offset / static_cast<double>(m_sample_count)),
                               m_mech_deg_per_motor_elec_cycle);
                m_average_offset = avg_offset;

                /* Measure encoder cycles per mechanical revolution only over the
                 * locked-rotor region (acquisition start → rotation end).  The
                 * initial slip segment would otherwise corrupt the result. */
                const float locked_encoder_elec_deg =
                    m_tracker.unwrappedDegrees() - m_acquire_start_encoder_elec_deg;
                const float locked_field_mech_deg =
                    field_mech - m_acquire_start_field_mech_deg;
                if (std::fabs(locked_field_mech_deg) > 30.0f) {
                    m_measured_encoder_cycles_per_rev =
                        std::fabs(locked_encoder_elec_deg) /
                        std::fabs(locked_field_mech_deg);
                } else {
                    /* Not enough locked motion to improve on the pole-cal
                     * estimate; leave the measured value at 0 so the caller
                     * can fall back. */
                    m_measured_encoder_cycles_per_rev = 0.0f;
                }

                restoreHardware();
                encoderADC().learnBounds(false);

                /* The Layer 1 ellipse fit (if any) was already computed and
                 * applied before offset acquisition so the stored offset matches
                 * the angle decoding the runtime will use. */
                if (m_fit.valid) {
                    const float phi_deg = std::fabs(m_fit.phase_err) * RAD_TO_DEG;
                    const float amp_mismatch_pct =
                        100.0f * std::fabs(m_fit.amp_sin - m_fit.amp_cos) /
                        (0.5f * (m_fit.amp_sin + m_fit.amp_cos));
                    Telemetry::printf("[CAL] ENC: fit: Cs=%.1f Cc=%.1f As=%.1f Ac=%.1f phi=%.3f deg samples=%lu",
                                      static_cast<double>(m_fit.center_sin),
                                      static_cast<double>(m_fit.center_cos),
                                      static_cast<double>(m_fit.amp_sin),
                                      static_cast<double>(m_fit.amp_cos),
                                      static_cast<double>(phi_deg),
                                      static_cast<unsigned long>(m_fit.sample_count));
                    if (phi_deg > 3.0f || amp_mismatch_pct > 10.0f) {
                        Telemetry::printf("[CAL] ENC: fit WARNING: phi=%.2f deg gain_mismatch=%.1f%% (check wiring/mounting)",
                                          static_cast<double>(phi_deg),
                                          static_cast<double>(amp_mismatch_pct));
                    }
                } else {
                    Telemetry::printf("[CAL] ENC: fit not active");
                }

                Telemetry::printf("[CAL] ENC: DONE: avg=%.3f deg samples=%d revs=%.1f",
                                  static_cast<double>(m_average_offset),
                                  m_sample_count,
                                  static_cast<double>(moved_field / 360.0f));
                Telemetry::printf("[CAL] ENC: bounds: sin=[%u,%u] cos=[%u,%u]",
                                  static_cast<unsigned int>(encoderADC().sinMin()),
                                  static_cast<unsigned int>(encoderADC().sinMax()),
                                  static_cast<unsigned int>(encoderADC().cosMin()),
                                  static_cast<unsigned int>(encoderADC().cosMax()));
                m_state = State::DONE;
                return;
            }

            if ((now_ms - m_state_start_ms) > OFFSET_ROTATE_TIMEOUT_MS) {
                PWM_StopSPWM();
                fail("[CAL] ENC: FAIL: rotation timed out (moved %.1f deg)",
                     static_cast<double>(moved_field));
                return;
            }
            break;
        }

        default:
            break;
    }
}

EncoderOffsetCalibrator& encoderOffsetCalibrator() {
    return EncoderOffsetCalibrator::instance();
}

} // namespace Inverter
