#include "Inverter/Calibration/ResistanceCalibrator.h"

#include "Inverter/Control/OpenLoopController.h"
#include "Inverter/Control/FaultManager.h"
#include "Inverter/Drivers/Sensors/PhaseCurrentADC.h"
#include "Inverter/Drivers/Sensors/DcLinkVoltageSensor.h"
#include "Inverter/Drivers/GateDriver/gate_driver.h"
#include "Inverter/Drivers/PWM/pwm.h"
#include "Inverter/Telemetry.h"

#include "main.h"
#include "tim.h"
#include <cstdarg>
#include <cmath>

namespace Inverter {

static ResistanceCalibrator s_instance;

ResistanceCalibrator& ResistanceCalibrator::instance() {
    return s_instance;
}

ResistanceCalibrator& resistanceCalibrator() {
    return s_instance;
}

namespace {

/* Pair semantics with the standard 3-phase complementary PWM driver (no true
 * high-Z is possible): pair XY drives phase X at the commanded duty while the
 * other two phases sit at 0 % (low side on) and share the return current.
 * Current enters the motor at the driven phase, so the driven-phase current
 * is the active measurement. */
float pairCurrentActive(float iu, float iv, float iw, ResistanceCalibrator::Pair pair) {
    (void)iw;
    /* Keep the historical sign convention (negated driven-phase current) so
     * the V/I fit behaves as originally designed. */
    switch (pair) {
        case ResistanceCalibrator::Pair::UV:
            return -iu;
        case ResistanceCalibrator::Pair::UW:
            return -iv;
        case ResistanceCalibrator::Pair::VW:
            return -iw;
    }
    return 0.0f;
}

float pairCurrentInactive(float iu, float iv, float iw, ResistanceCalibrator::Pair pair) {
    /* One of the two return phases.  Both return phases carry roughly half of
     * the active current by design; the check in finishPairMeasurement()
     * tolerates that and only flags gross wiring/sensor faults. */
    switch (pair) {
        case ResistanceCalibrator::Pair::UV:
            return iw;
        case ResistanceCalibrator::Pair::UW:
            return iw;
        case ResistanceCalibrator::Pair::VW:
            return iv;
    }
    return 0.0f;
}

} // namespace

const char* ResistanceCalibrator::pairName(Pair pair) {
    switch (pair) {
        case Pair::UV: return "UV";
        case Pair::UW: return "UW";
        case Pair::VW: return "VW";
    }
    return "?";
}

int ResistanceCalibrator::pairIndex(Pair pair) {
    switch (pair) {
        case Pair::UV: return 0;
        case Pair::UW: return 1;
        case Pair::VW: return 2;
    }
    return 0;
}

float ResistanceCalibrator::lastResult(Pair pair) const {
    return m_results[pairIndex(pair)];
}

bool ResistanceCalibrator::start(float bus_pct, Pair pair, bool run_all,
                                 uint32_t timeout_ms, float max_current_a) {
    if (isActive()) {
        Telemetry::printf("[CAL] RES: already running");
        return false;
    }

    if (openLoopController().isRunning()) {
        Telemetry::printf("[CAL] RES: stop the motor before calibration");
        return false;
    }

    if (FaultManager::instance().isSeverityActive(FaultSeverity::Critical) ||
        FaultManager::instance().isSeverityActive(FaultSeverity::High)) {
        Telemetry::printf("[CAL] RES: active faults, cannot start");
        return false;
    }

    if (bus_pct < 0.0f) bus_pct = 0.0f;
    if (bus_pct > MAX_BUS_PCT) {
        Telemetry::printf("[CAL] RES: clamped bus_pct to %.2f %%", static_cast<double>(MAX_BUS_PCT));
        bus_pct = MAX_BUS_PCT;
    }

    m_mode = Mode::VOLTAGE_STEP;
    /* Evenly spaced duty points from bus_pct/NUM_POINTS up to bus_pct. */
    for (uint8_t i = 0; i < NUM_POINTS; ++i) {
        m_targets[i] = bus_pct * static_cast<float>(i + 1U) /
                       static_cast<float>(NUM_POINTS);
    }
    m_max_current_a = (max_current_a < 0.0f) ? 0.0f : max_current_a;
    m_timeout_ms = timeout_ms;
    m_pair_index = 0;
    m_point_index = 0;
    m_num_pairs = run_all ? 3U : 1U;
    m_pairs[0] = pair;
    if (run_all) {
        m_pairs[1] = (pair == Pair::UV) ? Pair::UW :
                     (pair == Pair::UW) ? Pair::VW : Pair::UV;
        m_pairs[2] = (pair == Pair::UV) ? Pair::VW :
                     (pair == Pair::UW) ? Pair::UV : Pair::UW;
    }

    m_results[0] = m_results[1] = m_results[2] = 0.0f;
    m_result_valid[0] = m_result_valid[1] = m_result_valid[2] = false;
    m_average_r_phase = 0.0f;
    for (uint8_t i = 0; i < NUM_POINTS; ++i) {
        resetMeasurementAccumulators(i);
    }
    m_pi_integral = 0.0f;
    m_pi_duty = 0.0f;
    m_pi_last_ms = 0;

    enterState(State::ENABLE);

    Telemetry::printf("[CAL] RES: starting (%s) max %.4f %% bus, %u points, max I=%.3f A",
                      run_all ? "UV/UW/VW" : pairName(pair),
                      static_cast<double>(bus_pct),
                      static_cast<unsigned>(NUM_POINTS),
                      static_cast<double>(m_max_current_a));
    return true;
}

bool ResistanceCalibrator::startCurrentCtrl(float max_current_a, Pair pair,
                                            bool run_all, uint32_t timeout_ms,
                                            float oc_limit_a) {
    if (isActive()) {
        Telemetry::printf("[CAL] RES: already running");
        return false;
    }

    if (openLoopController().isRunning()) {
        Telemetry::printf("[CAL] RES: stop the motor before calibration");
        return false;
    }

    if (FaultManager::instance().isSeverityActive(FaultSeverity::Critical) ||
        FaultManager::instance().isSeverityActive(FaultSeverity::High)) {
        Telemetry::printf("[CAL] RES: active faults, cannot start");
        return false;
    }

    if (max_current_a < 0.0f) max_current_a = 0.0f;
    if (oc_limit_a <= 0.0f) oc_limit_a = max_current_a * 1.2f;
    if (oc_limit_a < max_current_a) oc_limit_a = max_current_a;

    m_mode = Mode::CURRENT_CTRL;
    /* Exponentially spaced current setpoints from CURRENT_CTRL_MIN_A up to
     * max_current_a, concentrating points at the low-current IGBT knee. */
    if (max_current_a <= CURRENT_CTRL_MIN_A) {
        max_current_a = CURRENT_CTRL_MIN_A + 1.0f;
    }
    const float ratio = std::pow(max_current_a / CURRENT_CTRL_MIN_A,
                                 1.0f / static_cast<float>(NUM_POINTS));
    for (uint8_t i = 0; i < NUM_POINTS; ++i) {
        m_targets[i] = CURRENT_CTRL_MIN_A * std::pow(ratio, static_cast<float>(i + 1U));
    }
    m_max_current_a = oc_limit_a;
    /* Do NOT change the global PhaseCurrentADC overcurrent threshold here.
     * The resistance calibrator has its own per-sample abort check.  Raising
     * a system-wide PhaseOvercurrent fault would turn off the gate-driver
     * power rail and can leave the current-sense telemetry path in a bad
     * state after the cal finishes or aborts. */
    m_timeout_ms = timeout_ms;
    m_pair_index = 0;
    m_point_index = 0;
    m_num_pairs = run_all ? 3U : 1U;
    m_pairs[0] = pair;
    if (run_all) {
        m_pairs[1] = (pair == Pair::UV) ? Pair::UW :
                     (pair == Pair::UW) ? Pair::VW : Pair::UV;
        m_pairs[2] = (pair == Pair::UV) ? Pair::VW :
                     (pair == Pair::UW) ? Pair::UV : Pair::UW;
    }

    m_results[0] = m_results[1] = m_results[2] = 0.0f;
    m_result_valid[0] = m_result_valid[1] = m_result_valid[2] = false;
    m_average_r_phase = 0.0f;
    for (uint8_t i = 0; i < NUM_POINTS; ++i) {
        resetMeasurementAccumulators(i);
    }
    m_pi_integral = 0.0f;
    m_pi_duty = PI_MIN_DUTY;
    m_pi_last_ms = 0;

    enterState(State::ENABLE);

    Telemetry::printf("[CAL] RES: I-ctrl (%s) target %.3f A, %u points, oc=%.3f A",
                      run_all ? "UV/UW/VW" : pairName(pair),
                      static_cast<double>(max_current_a),
                      static_cast<unsigned>(NUM_POINTS),
                      static_cast<double>(m_max_current_a));
    return true;
}

void ResistanceCalibrator::enterState(State state) {
    m_state = state;
    m_state_enter_ms = HAL_GetTick();
}

void ResistanceCalibrator::stop() {
    if (m_state != State::IDLE && m_state != State::DONE && m_state != State::FAIL) {
        Telemetry::printf("[CAL] RES: stopped by user");
        restoreHardware();
        enterState(State::FAIL);
    }
    m_force_mode = false;
}

void ResistanceCalibrator::fail(const char* reason_fmt, ...) {
    restoreHardware();

    va_list ap;
    va_start(ap, reason_fmt);
    Telemetry::vprintf(reason_fmt, ap);
    va_end(ap);

    enterState(State::FAIL);
}

void ResistanceCalibrator::resetMeasurementAccumulators(uint8_t point) {
    m_sample_count[point] = 0;
    m_sum_i_active[point] = 0.0f;
    m_sum_i_inactive[point] = 0.0f;
    m_sum_vdc[point] = 0.0f;
    m_sum_duty[point] = 0.0f;
}

void ResistanceCalibrator::configureHardware(float bus_pct) {
    /* Drive the pair's driven phase at the commanded duty through the standard
     * PWM driver; the two return phases sit at 0 % (low side on) and share the
     * return current.  The driver applies the same Motor.PhaseSwap duty remap
     * FOC uses, so the calibrator measures in exactly the frame the controller
     * runs in. */
    float du = 0.0f, dv = 0.0f, dw = 0.0f;
    switch (m_pairs[m_pair_index]) {
        case Pair::UV: du = bus_pct; break;
        case Pair::UW: dv = bus_pct; break;
        case Pair::VW: dw = bus_pct; break;
    }
    PWM_SetThreePhaseDuty(du, dv, dw);

    /* A DESAT trip can latch between gate-driver release and the first PWM
     * edge.  Check immediately so we don't run the PI on stale/frozen
     * current. */
    if (GateDriver_IsFault()) {
        fail("[CAL] RES: FAIL: DESAT/gate-driver fault latched after PWM enable");
        return;
    }
}

void ResistanceCalibrator::restoreHardware() {
    /* The driver owns the timer and GPIO configuration; just stop switching
     * and let CalibrationHardware hold the gate driver in reset. */
    PWM_Stop();
    CalibrationHardware::parkOutputs();
    CalibrationHardware::shutdown();

    /* Restore the software overcurrent threshold so the post-calibration
     * idle state is not left with a sensitive trip point. */
    phaseCurrentADC().setOvercurrentThreshold(m_saved_oc_threshold_a);
}

void ResistanceCalibrator::finishPairMeasurement() {
    const Pair pair = m_pairs[m_pair_index];

    float v[NUM_POINTS];
    float i[NUM_POINTS];
    float vdc_avg = 0.0f;
    float i_active_max = 0.0f;

    for (uint8_t pt = 0; pt < NUM_POINTS; ++pt) {
        if (m_sample_count[pt] < MIN_SAMPLES) {
            fail("[CAL] RES: FAIL: not enough samples");
            return;
        }

        const float vdc = m_sum_vdc[pt] / static_cast<float>(m_sample_count[pt]);
        const float i_active = m_sum_i_active[pt] / static_cast<float>(m_sample_count[pt]);
        const float i_inactive = std::fabs(
            m_sum_i_inactive[pt] / static_cast<float>(m_sample_count[pt]));

        /* With complementary PWM the two return phases each carry roughly half
         * of the active current at standstill.  On a free-spinning rotor the
         * PM back-EMF drives additional circulating current through the
         * low-side-clamped return windings (R is only ~10 mOhm, so even slow
         * motion produces multi-x currents); anything above the ratio is
         * suspicious and gets flagged, only an extreme excursion is a hard
         * fail.  Force mode skips even the hard fail. */
        if (!m_force_mode) {
            const float inactive_limit = std::max(
                MAX_INACTIVE_CURRENT_MIN_A, std::fabs(i_active) * MAX_INACTIVE_CURRENT_RATIO);
            if (i_inactive > inactive_limit) {
                fail("[CAL] RES: FAIL: %s inactive current %.3f A exceeds limit (active %.3f A)",
                     pairName(pair),
                     static_cast<double>(i_inactive),
                     static_cast<double>(std::fabs(i_active)));
                return;
            }
            const float inactive_warn = std::max(
                2.0f * MAX_INACTIVE_CURRENT_MIN_A, std::fabs(i_active) * 0.75f);
            if (i_inactive > inactive_warn) {
                Telemetry::printf("[CAL] RES: %s: note: return current %.3f A (%.2fx active) - "
                                  "rotor motion back-EMF, fit unaffected",
                                  pairName(pair),
                                  static_cast<double>(i_inactive),
                                  static_cast<double>(i_inactive /
                                                      std::max(std::fabs(i_active), 0.001f)));
            }
        }

        const float duty = m_sum_duty[pt] / static_cast<float>(m_sample_count[pt]);
        v[pt] = (duty / 100.0f) * vdc;
        i[pt] = i_active;
        vdc_avg += vdc;
        i_active_max = std::max(i_active_max, std::fabs(i_active));
    }
    vdc_avg /= static_cast<float>(NUM_POINTS);

    /* Print the raw (V, I) points used for the fit. */
    {
        Telemetry::printf("[CAL] RES: %s fit data: "
                          "(V=%.3fV I=%.3fA), "
                          "(V=%.3fV I=%.3fA), "
                          "(V=%.3fV I=%.3fA), "
                          "(V=%.3fV I=%.3fA), "
                          "(V=%.3fV I=%.3fA), "
                          "(V=%.3fV I=%.3fA), "
                          "(V=%.3fV I=%.3fA)",
                          pairName(pair),
                          static_cast<double>(v[0]), static_cast<double>(i[0]),
                          static_cast<double>(v[1]), static_cast<double>(i[1]),
                          static_cast<double>(v[2]), static_cast<double>(i[2]),
                          static_cast<double>(v[3]), static_cast<double>(i[3]),
                          static_cast<double>(v[4]), static_cast<double>(i[4]),
                          static_cast<double>(v[5]), static_cast<double>(i[5]),
                          static_cast<double>(v[6]), static_cast<double>(i[6]));
    }

    /* Linear regression: V = R_meas * I + V_offset.
     * R_meas is driven-phase-to-parallel-returns: R_phase + R_phase/2 =
     * 1.5 * R_phase.  We want the slope; V_offset (switch drops, dead time,
     * wiring) is discarded. */
    float sum_v = 0.0f;
    float sum_i = 0.0f;
    float sum_vi = 0.0f;
    float sum_ii = 0.0f;
    for (uint8_t pt = 0; pt < NUM_POINTS; ++pt) {
        sum_v  += v[pt];
        sum_i  += i[pt];
        sum_vi += v[pt] * i[pt];
        sum_ii += i[pt] * i[pt];
    }

    const float n = static_cast<float>(NUM_POINTS);
    const float denom = n * sum_ii - sum_i * sum_i;

    {
        Telemetry::printf("[CAL] RES: %s fit math: sumV=%.3f sumI=%.3f sumVI=%.3f sumII=%.3f denom=%.3f",
                          pairName(pair),
                          static_cast<double>(sum_v),
                          static_cast<double>(sum_i),
                          static_cast<double>(sum_vi),
                          static_cast<double>(sum_ii),
                          static_cast<double>(denom));
    }

    if (std::fabs(denom) < 1e-9f || !std::isfinite(denom)) {
        fail("[CAL] RES: FAIL: current did not vary enough between points");
        return;
    }

    const float r_meas = (n * sum_vi - sum_v * sum_i) / denom;
    {
        Telemetry::printf("[CAL] RES: %s fit result: R_meas=%.4f mohm",
                          pairName(pair),
                          static_cast<double>(r_meas * 1000.0f));
    }
    if (r_meas <= 0.0f || !std::isfinite(r_meas)) {
        fail("[CAL] RES: FAIL: computed resistance is non-positive; increase current/voltage");
        return;
    }

    const float v_offset = (sum_v - r_meas * sum_i) / n;

    const float r_phase = r_meas / 1.5f;
    const int idx = pairIndex(pair);
    m_results[idx] = r_phase;
    m_result_valid[idx] = true;

    Telemetry::printf("[CAL] RES: %s: R_meas=%.4f mohm  R_phase=%.4f mohm  Imax=%.3f A  Vdc=%.3f V  V_off=%.3f V",
                      pairName(pair),
                      static_cast<double>(r_meas * 1000.0f),
                      static_cast<double>(r_phase * 1000.0f),
                      static_cast<double>(i_active_max),
                      static_cast<double>(vdc_avg),
                      static_cast<double>(v_offset));

    enterState(State::NEXT_PAIR);
}


void ResistanceCalibrator::reportResults() {
    float sum_ll = 0.0f;
    float sum_ph = 0.0f;
    uint32_t count = 0;
    for (int i = 0; i < 3; ++i) {
        if (m_result_valid[i]) {
            sum_ll += m_results[i] * 2.0f; /* m_results stores phase resistance */
            sum_ph += m_results[i];
            ++count;
        }
    }
    m_average_r_phase = (count > 0) ? (sum_ph / static_cast<float>(count)) : 0.0f;
    const float avg_r_ll = (count > 0) ? (sum_ll / static_cast<float>(count)) : 0.0f;

    Telemetry::printf("[CAL] RES: DONE: Rll_uv=%.4f%s Rll_uw=%.4f%s Rll_vw=%.4f%s Rll_avg=%.4f mohm",
                      static_cast<double>(m_results[0] * 2000.0f), m_result_valid[0] ? "" : "(--)",
                      static_cast<double>(m_results[1] * 2000.0f), m_result_valid[1] ? "" : "(--)",
                      static_cast<double>(m_results[2] * 2000.0f), m_result_valid[2] ? "" : "(--)",
                      static_cast<double>(avg_r_ll * 1000.0f));

    Telemetry::log("r_ll_uv", m_results[0] * 2.0f);
    Telemetry::log("r_ll_uw", m_results[1] * 2.0f);
    Telemetry::log("r_ll_vw", m_results[2] * 2.0f);
    Telemetry::log("r_phase_avg", m_average_r_phase);
}

void ResistanceCalibrator::update() {
    if (m_state == State::IDLE || m_state == State::DONE ||
        m_state == State::FAIL) {
        return;
    }

    ++m_update_calls;
    const uint32_t now_ms = HAL_GetTick();

    /* Abort on any active Critical or High fault. */
    if (FaultManager::instance().isSeverityActive(FaultSeverity::Critical) ||
        FaultManager::instance().isSeverityActive(FaultSeverity::High)) {
        Telemetry::printf("[CAL] RES: FAIL: fault detected");
        restoreHardware();
        enterState(State::FAIL);
        return;
    }

    if (m_state == State::ENABLE) {
        m_saved_oc_threshold_a = phaseCurrentADC().overcurrentThreshold();

        /* Clear any previous TIM1 break before releasing the gate driver. */
        PWM_ClearBreakFlag();
        PWM_ClearFault();

        /* Hardware startup is driven by CalibrationHardware (gate-driver power
         * and reset sequencing); the PWM driver keeps ownership of TIM1 and
         * the gate pins throughout the measurement. */
        m_hw.begin();
        enterState(State::WAIT_READY);
        return;
    }

    if (m_state == State::WAIT_READY) {
        m_hw.update();
        if (m_hw.hasFailed()) {
            restoreHardware();
            enterState(State::FAIL);
            return;
        }
        if (!m_hw.isReady()) {
            return;
        }

        /* Gate driver is ready: park the bridge at zero voltage (all phases
         * at 0 % duty -> all low sides on, no voltage across the windings),
         * then release the timer outputs. */
        PWM_SetThreePhaseDuty(0.0f, 0.0f, 0.0f);
        PWM_ClearFault();
        PWM_Start();

        m_point_index = 0;
        if (m_mode == Mode::VOLTAGE_STEP) {
            configureHardware(m_targets[m_point_index]);
        } else {
            m_pi_integral = 0.0f;
            m_pi_duty = PI_MIN_DUTY;
            m_pi_last_ms = now_ms;
            configureHardware(m_pi_duty);
        }

        /* configureHardware() can transition to FAIL if DESAT/FLT trips
         * immediately.  Do not enter SETTLE if it failed. */
        if (m_state == State::FAIL) {
            return;
        }

        enterState(State::SETTLE);
        return;
    }

    const uint32_t elapsed_ms = now_ms - m_state_enter_ms;
    if (elapsed_ms > m_timeout_ms) {
        Telemetry::printf("[CAL] RES: FAIL: timeout");
        restoreHardware();
        enterState(State::FAIL);
        return;
    }

    const Pair pair = m_pairs[m_pair_index];

    if (m_state == State::SETTLE) {
        if (elapsed_ms >= SETTLE_TIME_MS) {
            const uint8_t pt = m_point_index;
            resetMeasurementAccumulators(pt);
            if (m_mode == Mode::VOLTAGE_STEP) {
                Telemetry::printf("[CAL] RES: %s point %u/%u: target Vll=%.3f V",
                                  pairName(pair), static_cast<unsigned>(pt + 1U),
                                  static_cast<unsigned>(NUM_POINTS),
                                  static_cast<double>(m_targets[pt] * 0.01f * dcLinkVoltageSensor().voltage()));
            } else {
                Telemetry::printf("[CAL] RES: %s point %u/%u: target I=%.3f A",
                                  pairName(pair), static_cast<unsigned>(pt + 1U),
                                  static_cast<unsigned>(NUM_POINTS),
                                  static_cast<double>(m_targets[pt]));
            }
            m_update_calls = 0;
            m_sample_calls = 0;
            m_last_rate_log_ms = now_ms;
            m_last_sample_ms = now_ms;
            m_pi_last_ms = now_ms; /* avoid a large initial dt in the PI */
            enterState(State::MEASURE);
        }
        return;
    }

    if (m_state == State::MEASURE) {
        const uint8_t pt = m_point_index;

        /* If the ADC trigger has stopped, the PI will wind up on stale data and
         * we risk a hardware overcurrent.  Abort if no new sample arrives. */
        if (now_ms - m_last_sample_ms > 100U) {
            Telemetry::printf("[CAL] RES: FAIL: no new ADC sample for %lu ms (stale current)",
                              static_cast<unsigned long>(now_ms - m_last_sample_ms));
            restoreHardware();
            enterState(State::FAIL);
            return;
        }

        float iu, iv, iw;
        if (phaseCurrentADC().sample(iu, iv, iw)) {
            ++m_sample_calls;
            m_last_sample_ms = now_ms;
            const float i_active = pairCurrentActive(iu, iv, iw, pair);
            m_sum_i_active[pt] += i_active;
            m_sum_i_inactive[pt] += pairCurrentInactive(iu, iv, iw, pair);
            const float vdc = dcLinkVoltageSensor().voltage();
            m_sum_vdc[pt] += vdc;
            ++m_sample_count[pt];

            if (m_mode == Mode::CURRENT_CTRL) {
                /* PI current controller: update duty every sample to regulate
                 * active current to the target. */
                const float dt_s = (m_pi_last_ms == 0) ? 0.001f :
                    static_cast<float>(now_ms - m_pi_last_ms) * 0.001f;
                m_pi_last_ms = now_ms;

                const float error = m_targets[pt] - i_active;
                m_pi_integral += PI_KI * error * dt_s;
                /* Simple anti-windup: clamp integral when output saturates. */
                float duty = PI_KP * error + m_pi_integral;
                if (duty > MAX_BUS_PCT) {
                    duty = MAX_BUS_PCT;
                    if (m_pi_integral > 0.0f) m_pi_integral -= PI_KI * error * dt_s;
                } else if (duty < PI_MIN_DUTY) {
                    duty = PI_MIN_DUTY;
                    if (m_pi_integral < 0.0f) m_pi_integral -= PI_KI * error * dt_s;
                }
                m_pi_duty = duty;
                configureHardware(m_pi_duty);
                m_sum_duty[pt] += m_pi_duty;
            } else {
                m_sum_duty[pt] += m_targets[pt];
            }

            if (m_max_current_a > 0.0f && std::fabs(i_active) > m_max_current_a) {
                Telemetry::printf("[CAL] RES: FAIL: overcurrent %.3f A > limit %.3f A",
                                  static_cast<double>(i_active),
                                  static_cast<double>(m_max_current_a));
                restoreHardware();
                enterState(State::FAIL);
                return;
            }
        }

        /* Diagnostic: log actual call and sample rates every 250 ms. */
        if (now_ms - m_last_rate_log_ms >= 250U) {
            const uint32_t dt_ms = now_ms - m_last_rate_log_ms;
            const float update_hz = static_cast<float>(m_update_calls) * 1000.0f /
                                    static_cast<float>(dt_ms);
            const float sample_hz = static_cast<float>(m_sample_calls) * 1000.0f /
                                    static_cast<float>(dt_ms);
            Telemetry::printf("[CAL] RES: %s timing: update=%.3f Hz  sample=%.3f Hz  n_samp=%lu",
                              pairName(pair),
                              static_cast<double>(update_hz),
                              static_cast<double>(sample_hz),
                              static_cast<unsigned long>(m_sample_calls));
            m_update_calls = 0;
            m_sample_calls = 0;
            m_last_rate_log_ms = now_ms;
        }

        if (elapsed_ms >= MEASURE_TIME_MS && m_sample_count[pt] >= MIN_SAMPLES) {
            const float vdc_pt = m_sum_vdc[pt] / static_cast<float>(m_sample_count[pt]);
            const float iact_pt = m_sum_i_active[pt] / static_cast<float>(m_sample_count[pt]);
            const float iinact_pt = std::fabs(
                m_sum_i_inactive[pt] / static_cast<float>(m_sample_count[pt]));
            const float duty_pt = m_sum_duty[pt] / static_cast<float>(m_sample_count[pt]);
            const float vll_pt = (duty_pt / 100.0f) * vdc_pt;
            Telemetry::printf("[CAL] RES: %s point %u/%u done: Vll=%.3f V  Vdc=%.3f V  duty=%.3f %%  Iact=%.3f A  Iinact=%.3f A",
                              pairName(pair), static_cast<unsigned>(pt + 1U),
                              static_cast<unsigned>(NUM_POINTS),
                              static_cast<double>(vll_pt),
                              static_cast<double>(vdc_pt),
                              static_cast<double>(duty_pt),
                              static_cast<double>(iact_pt),
                              static_cast<double>(iinact_pt));

            if (m_point_index + 1U < NUM_POINTS) {
                ++m_point_index;
                if (m_mode == Mode::VOLTAGE_STEP) {
                    configureHardware(m_targets[m_point_index]);
                } else {
                    /* Carry PI state forward so the next setpoint starts near the
                     * previous operating point instead of winding up from zero. */
                    m_pi_last_ms = now_ms;
                    configureHardware(m_pi_duty);
                }
                enterState(State::SETTLE);
            } else {
                enterState(State::FINISH_PAIR);
            }
        }
        return;
    }

    if (m_state == State::FINISH_PAIR) {
        finishPairMeasurement();
        return;
    }

    if (m_state == State::NEXT_PAIR) {
        ++m_pair_index;
        if (m_pair_index >= m_num_pairs) {
            restoreHardware();
            reportResults();
            enterState(State::DONE);
            return;
        }
        m_point_index = 0;
        if (m_mode == Mode::VOLTAGE_STEP) {
            configureHardware(m_targets[m_point_index]);
        } else {
            m_pi_integral = 0.0f;
            m_pi_duty = PI_MIN_DUTY;
            m_pi_last_ms = now_ms;
            configureHardware(m_pi_duty);
        }
        enterState(State::SETTLE);
        return;
    }
}

} // namespace Inverter
