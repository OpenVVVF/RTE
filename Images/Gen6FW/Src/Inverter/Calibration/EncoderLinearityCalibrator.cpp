#include "Inverter/Calibration/EncoderLinearityCalibrator.h"
#include "Inverter/Calibration/Common/CalScratchBuffer.h"
#include "Inverter/Control/OpenLoopController.h"
#include "Inverter/Drivers/PWM/pwm.h"
#include "Inverter/Drivers/Sensors/EncoderADC.h"
#include "Inverter/Drivers/Sensors/PhaseCurrentADC.h"
#include "Inverter/Telemetry.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>

namespace Inverter {

static constexpr float VOLTAGE_ANGLE_U_HIGH_RAD = 1.57079632679f;
static constexpr float RAD_TO_DEG = 57.2957795131f;
static constexpr float DEG_TO_RAD = 0.01745329252f;
static constexpr float PI = 3.14159265358979323846f;

/* Keep the calibrator state in AXISRAM (.dma_buffers) so its bulk does not
 * consume DTCM.  The section is NOLOAD; members are default-initialized at
 * startup by the implicit constructor. */
static EncoderLinearityCalibrator s_instance __attribute__((section(".dma_buffers")));

EncoderLinearityCalibrator& EncoderLinearityCalibrator::instance() {
    return s_instance;
}

const char* EncoderLinearityCalibrator::stateName() const {
    switch (m_state) {
        case State::IDLE:               return "IDLE";
        case State::HW_INIT:            return "HW_INIT";
        case State::FIND_VOLTAGE:       return "FIND_VOLTAGE";
        case State::BREAKAWAY_SETTLE:   return "BREAKAWAY_SETTLE";
        case State::STEP:               return "STEP";
        case State::WAIT_SETTLE:        return "WAIT_SETTLE";
        case State::MEASURE:            return "MEASURE";
        case State::ANALYZE:            return "ANALYZE";
        case State::DONE:               return "DONE";
        case State::FAIL:               return "FAIL";
    }
    return "?";
}

bool EncoderLinearityCalibrator::start(float pole_count, float encoder_cycles_per_rev,
                                       float rotate_mod, float revolutions,
                                       float step_size_mech_deg, bool dual_pass) {
    if (isActive()) {
        Telemetry::printf("[CAL] ENCLIN: already active");
        return false;
    }

    if (pole_count <= 0.0f || encoder_cycles_per_rev <= 0.0f) {
        Telemetry::printf("[CAL] ENCLIN: ERROR: invalid pole_count/encoder_cycles");
        return false;
    }

    m_pole_pairs = pole_count / 2.0f;
    m_encoder_cycles_per_rev = encoder_cycles_per_rev;
    m_rotate_mod = (rotate_mod > 0.0f) ? rotate_mod : DEFAULT_ROTATE_MOD;
    m_revolutions = (revolutions > 0.0f) ? revolutions : 3.0f;
    m_step_size_mech_deg = (step_size_mech_deg > 0.0f)
                               ? step_size_mech_deg
                               : DEFAULT_STEP_SIZE_MECH_DEG;
    m_dual_pass = dual_pass;
    m_pass = 0;

    /* Claim and zero the shared scratch buffer for both bin arrays. */
    CalScratch::zero();
    m_bins = CalScratch::as<Bin>();
    m_bins_rev = m_bins + BIN_COUNT;
    const size_t needed = 2U * BIN_COUNT * sizeof(Bin);
    if (needed > CalScratch::size()) {
        Telemetry::printf("[CAL] ENCLIN: ERROR: scratch buffer too small (%u needed, %u available)",
                          static_cast<unsigned>(needed),
                          static_cast<unsigned>(CalScratch::size()));
        return false;
    }
    resetBins();

    if (!openLoopController().isInitialized()) {
        openLoopController().init();
    }

    m_tracker.reset();

    m_hw.begin();
    enterState(State::HW_INIT);
    return true;
}

void EncoderLinearityCalibrator::enterState(State state) {
    m_state = state;
    m_state_start_ms = HAL_GetTick();
    Telemetry::printf("[CAL] ENCLIN DBG: enter %s", stateName());

    switch (state) {
        case State::HW_INIT:
            break;

        case State::FIND_VOLTAGE:
            PWM_ResetSPWMElectricalCycles();
            PWM_StartSPWM(ROTATE_FREQUENCY_HZ, 0.0f);
            m_tracker.reset();
            m_breakaway.start(0.005f, 200U, MAX_ROTATE_MOD,
                              0.15f, BREAKAWAY_MARGIN, 6000U);
            break;

        case State::BREAKAWAY_SETTLE:
            /* Switch from rotating field to a static holding vector while the
             * rotor settles at the start angle.  Seed unwrapped tracking from
             * the tracker so revolutions during breakaway are not lost, and
             * start the hold throttle at the breakaway-derived ceiling. */
            PWM_StopSPWM();
            m_hold_mod = m_rotate_mod;
            m_last_throttle_ms = 0;
            commandFieldAngle(0.0f);
            m_prev_encoder_deg = encoderADC().lastAngle();
            m_encoder_unwrapped = m_tracker.unwrappedDegrees();
            break;

        case State::STEP:
            m_settle_start_ms = 0;
            m_settled = false;
            m_target_field_mech = static_cast<float>(m_step_index) * m_step_size_mech_deg;
            commandFieldAngle(m_target_field_mech);
            break;

        case State::WAIT_SETTLE:
            m_settle_start_ms = 0;
            m_settled = false;
            m_settle_ref_encoder = m_encoder_unwrapped;
            break;

        case State::MEASURE:
            m_prev_encoder_deg = encoderADC().lastAngle();
            m_encoder_unwrapped = m_prev_encoder_deg;
            accumulateSample();
            break;

        case State::ANALYZE:
            analyze();
            break;

        case State::DONE:
        case State::FAIL:
        case State::IDLE:
        default:
            break;
    }
}

static float maxPhaseCurrentMagnitude() {
    const float iu = phaseCurrentADC().lastU();
    const float iv = phaseCurrentADC().lastV();
    const float iw = -(iu + iv);
    float max_i = std::fabs(iu);
    if (std::fabs(iv) > max_i) max_i = std::fabs(iv);
    if (std::fabs(iw) > max_i) max_i = std::fabs(iw);
    return max_i;
}

void EncoderLinearityCalibrator::commandFieldAngle(float field_mech_deg) {
    /* PWM_SetVoltageAngle convention: electrical rad, U peak at +90 deg.
     * Uses the throttled hold mod, not the raw ceiling. */
    const float field_elec_rad = field_mech_deg * m_pole_pairs * DEG_TO_RAD +
                                 VOLTAGE_ANGLE_U_HIGH_RAD;
    PWM_SetVoltageAngle(field_elec_rad, m_hold_mod);
}

void EncoderLinearityCalibrator::updateHoldThrottle(uint32_t now_ms) {
    /* Same throttle shape as CurrentLimitedRamp: back off fast when over the
     * current target, recover slowly toward the ceiling when under.  A static
     * holding vector draws V/R, so the breakaway-derived mod is only the
     * ceiling — the applied mod settles wherever the current target lands. */
    if (m_last_throttle_ms == 0U) {
        m_last_throttle_ms = now_ms;
        return;
    }
    const float dt = (now_ms - m_last_throttle_ms) * 1.0e-3f;
    if (dt <= 0.0f || dt >= 0.5f) {
        m_last_throttle_ms = now_ms;
        return;
    }
    m_last_throttle_ms = now_ms;

    if (m_last_current_a > HOLD_CURRENT_TARGET_A) {
        const float decay = 1.0f - std::exp(-dt / 0.020f);
        m_hold_mod -= m_hold_mod * decay;
        if (m_hold_mod < HOLD_MOD_FLOOR) m_hold_mod = HOLD_MOD_FLOOR;
    } else if (m_last_current_a <= 0.8f * HOLD_CURRENT_TARGET_A) {
        const float recover = 1.0f - std::exp(-dt / 0.5f);
        m_hold_mod += (m_rotate_mod - m_hold_mod) * recover;
        if (m_hold_mod > m_rotate_mod) m_hold_mod = m_rotate_mod;
    }
}

void EncoderLinearityCalibrator::restoreHardware() {
    CalibrationHardware::shutdown();
}

void EncoderLinearityCalibrator::stop() {
    if (!isActive()) {
        return;
    }
    PWM_StopSPWM();
    restoreHardware();
    m_state = State::IDLE;
    Telemetry::printf("[CAL] ENCLIN: stopped by user");
}

void EncoderLinearityCalibrator::fail(const char* reason_fmt, ...) {
    va_list args;
    va_start(args, reason_fmt);
    Telemetry::vprintf(reason_fmt, args);
    va_end(args);

    PWM_StopSPWM();
    restoreHardware();
    m_state = State::FAIL;
}

float EncoderLinearityCalibrator::fieldMechanicalAngle() const {
    const float spwm_angle = PWM_GetSPWMAngle();
    const uint32_t cycles = PWM_GetSPWMElectricalCycles();
    float field_elec_deg = static_cast<float>(cycles) * 360.0f + spwm_angle * RAD_TO_DEG;
    field_elec_deg -= VOLTAGE_ANGLE_U_HIGH_RAD * RAD_TO_DEG;
    return field_elec_deg / m_pole_pairs;
}

float EncoderLinearityCalibrator::wrap180(float angle_deg) {
    while (angle_deg > 180.0f) angle_deg -= 360.0f;
    while (angle_deg <= -180.0f) angle_deg += 360.0f;
    return angle_deg;
}

int EncoderLinearityCalibrator::encoderBin(float encoder_deg) {
    int bin = static_cast<int>(encoder_deg * static_cast<float>(BIN_COUNT) / 360.0f);
    if (bin < 0) bin = 0;
    if (bin >= BIN_COUNT) bin = BIN_COUNT - 1;
    return bin;
}

void EncoderLinearityCalibrator::resetBins() {
    if (m_bins == nullptr) return;
    std::fill(m_bins, m_bins + BIN_COUNT, Bin{});
    if (m_bins_rev != nullptr) {
        std::fill(m_bins_rev, m_bins_rev + BIN_COUNT, Bin{});
    }
}

void EncoderLinearityCalibrator::updateEncoderTracking() {
    const float encoder_deg = encoderADC().lastAngle();
    float delta = encoder_deg - m_prev_encoder_deg;
    if (delta > 180.0f) delta -= 360.0f;
    else if (delta < -180.0f) delta += 360.0f;
    m_encoder_unwrapped += delta;
    m_prev_encoder_deg = encoder_deg;
}

void EncoderLinearityCalibrator::accumulateSample() {
    Bin* bins = (m_pass == 0) ? m_bins : m_bins_rev;
    if (bins == nullptr) return;

    updateEncoderTracking();

    const float encoder_deg = m_prev_encoder_deg;  /* updated by tracking */
    const float field_deg = m_target_field_mech;   /* field angle we commanded */
    const float diff = wrap180(m_encoder_unwrapped - field_deg);

    const int bin = encoderBin(encoder_deg);
    bins[bin].sum += static_cast<double>(diff);
    ++bins[bin].count;
}

void EncoderLinearityCalibrator::analyze() {
    if (m_bins == nullptr) {
        m_residual_rms_deg = 0.0f;
        m_residual_pp_deg = 0.0f;
        for (int k = 1; k <= MAX_HARMONIC; ++k) m_harmonics[k] = 0.0f;
        return;
    }

    /* First pass: convert per-bin sums to per-bin means.  In dual-pass mode,
     * average the FWD and REV means where both exist: the direction-dependent
     * torque-lag term cancels, leaving encoder INL.  The direction-dependent
     * half-difference is the lag component and is reported separately.
     * The combined residual is written back into m_bins[].sum for the DFT. */
    double total_sum = 0.0;
    uint32_t total_count = 0;
    m_valid_bins = 0;
    m_paired_bins = 0;
    double lag_sum_sq = 0.0;

    for (int i = 0; i < BIN_COUNT; ++i) {
        const bool has_f = m_bins[i].count > 0;
        const bool has_r = (m_bins_rev != nullptr) && (m_bins_rev[i].count > 0);

        double combined = 0.0;
        uint32_t n = 0;
        if (has_f && has_r) {
            const double mean_f = m_bins[i].sum / static_cast<double>(m_bins[i].count);
            const double mean_r = m_bins_rev[i].sum / static_cast<double>(m_bins_rev[i].count);
            combined = 0.5 * (mean_f + mean_r);
            n = m_bins[i].count + m_bins_rev[i].count;
            const double lag = 0.5 * (mean_f - mean_r);
            lag_sum_sq += lag * lag;
            ++m_paired_bins;
        } else if (has_f) {
            combined = m_bins[i].sum / static_cast<double>(m_bins[i].count);
            n = m_bins[i].count;
        } else if (has_r) {
            combined = m_bins_rev[i].sum / static_cast<double>(m_bins_rev[i].count);
            n = m_bins_rev[i].count;
        }

        if (n > 0) {
            m_bins[i].sum = combined;
            m_bins[i].count = n;
            total_sum += combined * static_cast<double>(n);
            total_count += n;
            ++m_valid_bins;
        } else {
            m_bins[i].sum = 0.0;
            m_bins[i].count = 0;
        }
    }

    m_lag_rms_deg = (m_paired_bins > 0)
                        ? static_cast<float>(std::sqrt(lag_sum_sq / static_cast<double>(m_paired_bins)))
                        : 0.0f;

    const float mean = (total_count > 0)
                           ? static_cast<float>(total_sum / static_cast<double>(total_count))
                           : 0.0f;

    /* Second pass: RMS and peak-to-peak of the zero-mean residual, plus
     * outlier-bin diagnostics so a localized defect can be distinguished from
     * broadband noise. */
    double sum_sq = 0.0;
    float min_r = 1e6f;
    float max_r = -1e6f;
    int min_bin = -1;
    int max_bin = -1;
    int outlier_count = 0;  /* bins with |residual| > 1.0 deg */
    for (int i = 0; i < BIN_COUNT; ++i) {
        if (m_bins[i].count == 0) continue;
        const float residual = static_cast<float>(m_bins[i].sum) - mean;
        sum_sq += static_cast<double>(residual) * residual;
        if (residual < min_r) { min_r = residual; min_bin = i; }
        if (residual > max_r) { max_r = residual; max_bin = i; }
        if (std::fabs(residual) > 1.0f) ++outlier_count;
    }

    m_residual_rms_deg = static_cast<float>(std::sqrt(sum_sq / static_cast<double>(BIN_COUNT)));
    m_residual_pp_deg = max_r - min_r;

    /* DFT for harmonics 1..MAX_HARMONIC. */
    const float scale = 2.0f / static_cast<float>(BIN_COUNT);
    for (int k = 1; k <= MAX_HARMONIC; ++k) {
        double a = 0.0;
        double b = 0.0;
        for (int i = 0; i < BIN_COUNT; ++i) {
            const float residual = (m_bins[i].count > 0)
                                       ? static_cast<float>(m_bins[i].sum) - mean
                                       : 0.0f;
            const float theta = static_cast<float>(k * i) * 2.0f *
                                PI / static_cast<float>(BIN_COUNT);
            a += static_cast<double>(residual) * std::cos(theta);
            b += static_cast<double>(residual) * std::sin(theta);
        }
        a *= scale;
        b *= scale;
        m_harmonics[k] = static_cast<float>(std::sqrt(a * a + b * b));
    }

    Telemetry::printf("[CAL] ENCLIN: valid_bins=%d paired=%d lag_rms=%.4f deg rms=%.4f deg pp=%.4f deg outliers(>1deg)=%d",
                      m_valid_bins,
                      m_paired_bins,
                      static_cast<double>(m_lag_rms_deg),
                      static_cast<double>(m_residual_rms_deg),
                      static_cast<double>(m_residual_pp_deg),
                      outlier_count);
    if (min_bin >= 0) {
        Telemetry::printf("[CAL] ENCLIN: worst bins: bin %d (%+.2f deg, angle %.2f) bin %d (%+.2f deg, angle %.2f)",
                          min_bin,
                          static_cast<double>(min_r),
                          static_cast<double>((min_bin + 0.5f) * 360.0f / BIN_COUNT),
                          max_bin,
                          static_cast<double>(max_r),
                          static_cast<double>((max_bin + 0.5f) * 360.0f / BIN_COUNT));
    }
    Telemetry::printf("[CAL] ENCLIN: harmonics (deg): ");
    for (int k = 1; k <= MAX_HARMONIC; ++k) {
        Telemetry::printf("  H%02d=%.4f", k, static_cast<double>(m_harmonics[k]));
    }
}

float EncoderLinearityCalibrator::harmonicAmplitude(int harmonic) const {
    if (harmonic < 1 || harmonic > MAX_HARMONIC) return 0.0f;
    return m_harmonics[harmonic];
}

void EncoderLinearityCalibrator::update() {
    if (m_state == State::IDLE || m_state == State::DONE || m_state == State::FAIL) {
        return;
    }

    const uint32_t now_ms = HAL_GetTick();

    if ((now_ms - m_state_start_ms) > OVERALL_TIMEOUT_MS) {
        fail("[CAL] ENCLIN: FAIL: overall timeout");
        return;
    }

    /* Hard overcurrent abort, independent of the calibration logic.  PWM is
     * active in every state past HW_INIT, and a static holding vector draws
     * V/R continuously — a stuck rotor otherwise heats the stage until the
     * user reboots. */
    if (m_state != State::HW_INIT) {
        m_last_current_a = maxPhaseCurrentMagnitude();
        if (m_last_current_a > OVERCURRENT_ABORT_A) {
            if (m_oc_start_ms == 0U) {
                m_oc_start_ms = now_ms;
            } else if ((now_ms - m_oc_start_ms) > OVERCURRENT_SUSTAIN_MS) {
                fail("[CAL] ENCLIN: FAIL: overcurrent %.1f A sustained (limit %.1f A)",
                     static_cast<double>(m_last_current_a),
                     static_cast<double>(OVERCURRENT_ABORT_A));
                return;
            }
        } else {
            m_oc_start_ms = 0;
        }
    }

    /* Current-targeted hold throttle: active once the field is static. */
    if (m_state == State::BREAKAWAY_SETTLE || m_state == State::STEP ||
        m_state == State::WAIT_SETTLE || m_state == State::MEASURE) {
        updateHoldThrottle(now_ms);
    }

    switch (m_state) {
        case State::HW_INIT: {
            m_hw.update();
            if (m_hw.hasFailed()) {
                fail("[CAL] ENCLIN: ERROR: gate driver not ready or fault latched");
            } else if (m_hw.isReady()) {
                PWM_ClearFault();
                PWM_StartPhase(0);
                PWM_StartPhase(1);
                PWM_StartPhase(2);

                m_total_steps = static_cast<int>(
                    std::ceil(m_revolutions * 360.0f / m_step_size_mech_deg));
                Telemetry::printf("[CAL] ENCLIN: started for %.0f poles, encoder_cycles=%.2f, steps=%d",
                                  static_cast<double>(m_pole_pairs * 2.0f),
                                  static_cast<double>(m_encoder_cycles_per_rev),
                                  m_total_steps);

                if (m_rotate_mod > 0.0f) {
                    /* User supplied a modulation; skip breakaway search. */
                    Telemetry::printf("[CAL] ENCLIN: using fixed rotate_mod=%.3f", static_cast<double>(m_rotate_mod));
                    enterState(State::BREAKAWAY_SETTLE);
                } else {
                    enterState(State::FIND_VOLTAGE);
                }
            } else if ((now_ms - m_state_start_ms) > HW_INIT_TIMEOUT_MS) {
                fail("[CAL] ENCLIN: ERROR: hardware init timeout");
            }
            break;
        }

        case State::FIND_VOLTAGE: {
            m_tracker.update();
            float mod = 0.0f;
            const auto status = m_breakaway.update(now_ms,
                                                   std::fabs(m_tracker.mechanicalCycles()),
                                                   mod);
            PWM_SetSPWMParams(ROTATE_FREQUENCY_HZ, mod);

            if ((now_ms - m_last_dbg_ms) >= 500U) {
                m_last_dbg_ms = now_ms;
                Telemetry::printf("[CAL] ENCLIN DBG: find: mod=%.3f moved=%.3f I=%.1f A",
                                  static_cast<double>(mod),
                                  static_cast<double>(std::fabs(m_tracker.mechanicalCycles())),
                                  static_cast<double>(m_last_current_a));
            }

            if (m_breakaway.phantomMotionDetected()) {
                PWM_StopSPWM();
                fail("[CAL] ENCLIN: FAIL: phantom motion detected before voltage applied");
                return;
            }

            if (status == BreakawayFinder::Status::FOUND) {
                const float breakaway_mod = m_breakaway.breakawayMod();
                m_rotate_mod = breakaway_mod * BREAKAWAY_MARGIN;
                if (m_rotate_mod > MAX_ROTATE_MOD) m_rotate_mod = MAX_ROTATE_MOD;
                Telemetry::printf("[CAL] ENCLIN: breakaway at mod=%.3f -> rotate mod=%.3f",
                                  static_cast<double>(breakaway_mod),
                                  static_cast<double>(m_rotate_mod));
                enterState(State::BREAKAWAY_SETTLE);
            } else if (status == BreakawayFinder::Status::TIMEOUT) {
                fail("[CAL] ENCLIN: FAIL: breakaway not found");
            }
            break;
        }

        case State::BREAKAWAY_SETTLE: {
            commandFieldAngle(0.0f);
            updateEncoderTracking();
            if ((now_ms - m_state_start_ms) >= BREAKAWAY_SETTLE_MS) {
                m_step_index = 0;
                enterState(State::STEP);
            }
            break;
        }

        case State::STEP: {
            /* Re-command the same angle (paranoia) then wait for motion. */
            commandFieldAngle(m_target_field_mech);
            enterState(State::WAIT_SETTLE);
            break;
        }

        case State::WAIT_SETTLE: {
            commandFieldAngle(m_target_field_mech);
            updateEncoderTracking();

            /* Position-based settle: the encoder must stay within a small
             * window for the full settle duration.  This is far less noisy than
             * the RPM derivative at near-zero speed. */
            const float pos_error = std::fabs(m_encoder_unwrapped - m_settle_ref_encoder);
            const bool below_threshold = pos_error < SETTLE_POS_THRESHOLD_DEG;

            if (below_threshold) {
                if (m_settle_start_ms == 0U) {
                    m_settle_start_ms = now_ms;
                } else if ((now_ms - m_settle_start_ms) >= SETTLE_DURATION_MS) {
                    enterState(State::MEASURE);
                    break;
                }
            } else {
                m_settle_start_ms = 0;
                m_settle_ref_encoder = m_encoder_unwrapped;
            }

            if ((now_ms - m_state_start_ms) > STEP_TIMEOUT_MS) {
                fail("[CAL] ENCLIN: FAIL: step %d did not settle (pos_err=%.2f deg)",
                     m_step_index, static_cast<double>(pos_error));
                return;
            }

            if ((now_ms - m_last_dbg_ms) >= 500U) {
                m_last_dbg_ms = now_ms;
                Telemetry::printf("[CAL] ENCLIN DBG: step=%d/%d fld=%.1f err=%.3f deg settle=%u ms I=%.1f A hmod=%.3f",
                                  m_step_index, m_total_steps,
                                  static_cast<double>(m_target_field_mech),
                                  static_cast<double>(pos_error),
                                  below_threshold ? (now_ms - m_settle_start_ms) : 0U,
                                  static_cast<double>(m_last_current_a),
                                  static_cast<double>(m_hold_mod));
            }
            break;
        }

        case State::MEASURE: {
            commandFieldAngle(m_target_field_mech);
            accumulateSample();

            if (m_pass == 0) {
                ++m_step_index;
                if (m_step_index >= m_total_steps) {
                    if (m_dual_pass) {
                        /* Turn around: reverse approach flips the sign of the
                         * static torque-angle lag so averaging cancels it. */
                        m_pass = 1;
                        m_step_index = m_total_steps - 1;
                        Telemetry::printf("[CAL] ENCLIN: FWD pass done, starting REV pass");
                        enterState(State::STEP);
                        break;
                    }
                    PWM_StopSPWM();
                    restoreHardware();
                    Telemetry::printf("[CAL] ENCLIN: sweep complete (%d steps)", m_step_index);
                    enterState(State::ANALYZE);
                    m_state = State::DONE;
                    return;
                }
            } else {
                --m_step_index;
                if (m_step_index < 0) {
                    PWM_StopSPWM();
                    restoreHardware();
                    Telemetry::printf("[CAL] ENCLIN: sweep complete (%d steps, dual pass)",
                                      m_total_steps);
                    enterState(State::ANALYZE);
                    m_state = State::DONE;
                    return;
                }
            }

            enterState(State::STEP);
            break;
        }

        case State::ANALYZE:
            /* analyze() already ran in enterState(); transition forced above. */
            break;

        default:
            break;
    }
}

EncoderLinearityCalibrator& encoderLinearityCalibrator() {
    return EncoderLinearityCalibrator::instance();
}

} // namespace Inverter
