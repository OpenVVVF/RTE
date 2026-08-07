#include "Inverter/Calibration/BreakawayCalibrator.h"

#include "Inverter/Calibration/CalKvStore.h"
#include "Inverter/Control/FaultManager.h"
#include "Inverter/Drivers/PWM/pwm.h"
#include "Inverter/Drivers/Sensors/DcLinkVoltageSensor.h"
#include "Inverter/Drivers/Sensors/EncoderADC.h"
#include "Inverter/Telemetry.h"

#include "main.h"
#include <cmath>

namespace Inverter {

static constexpr float RAMP_FREQUENCY_HZ = 1.0f;
static constexpr uint32_t RAMP_TIMEOUT_MS = 15000U;
static constexpr float MIN_TRUSTED_VOLTAGE_V = 0.3f;
static constexpr uint32_t HW_INIT_TIMEOUT_MS = 2000U;

static BreakawayCalibrator s_instance;

BreakawayCalibrator& BreakawayCalibrator::instance() {
    return s_instance;
}

float BreakawayCalibrator::voltageToMod(float v_v, float vdc_v) {
    return (vdc_v > 1.0f) ? (v_v * 2.0f / vdc_v) : 0.0f;
}

float BreakawayCalibrator::modToVoltage(float m, float vdc_v) {
    return m * vdc_v * 0.5f;
}

bool BreakawayCalibrator::start(float max_voltage, float step_voltage,
                                uint32_t ramp_period_ms, float detect_cycles) {
    if (isActive()) {
        Telemetry::printf("[CAL] BREAK: already active");
        return false;
    }

    if (max_voltage <= 0.0f || step_voltage <= 0.0f || ramp_period_ms == 0U || detect_cycles <= 0.0f) {
        Telemetry::printf("[CAL] BREAK: invalid parameters");
        return false;
    }

    m_max_voltage = max_voltage;
    m_step_voltage = step_voltage;
    m_ramp_period_ms = ramp_period_ms;
    m_detect_cycles = detect_cycles;

    m_mod = 0.0f;
    m_breakaway_mod = 0.0f;
    m_breakaway_voltage = 0.0f;
    m_have_start = false;
    m_in_trusted_region = false;
    m_max_phantom_cycles = 0.0f;
    m_tracker.reset();

    m_hw.begin();
    enterState(State::HW_INIT);
    return true;
}

void BreakawayCalibrator::enterState(State state) {
    m_state = state;
    m_state_start_ms = HAL_GetTick();
}

void BreakawayCalibrator::fail(const char* reason) {
    PWM_StopSPWM();
    CalibrationHardware::shutdown();
    Telemetry::printf("[CAL] BREAK: FAIL: %s", reason);
    m_state = State::FAIL;
}

void BreakawayCalibrator::stop() {
    if (isActive()) {
        PWM_StopSPWM();
        CalibrationHardware::shutdown();
        Telemetry::printf("[CAL] BREAK: stopped by user");
        m_state = State::IDLE;
    }
}

void BreakawayCalibrator::update() {
    if (!isActive()) {
        return;
    }

    const uint32_t now_ms = HAL_GetTick();

    /* Any critical fault (overcurrent, gate driver, etc.) aborts immediately. */
    if (FaultManager::instance().isSeverityActive(FaultSeverity::Critical)) {
        fail("critical fault detected");
        return;
    }

    m_tracker.update();

    switch (m_state) {
        case State::HW_INIT: {
            m_hw.update();
            if (m_hw.hasFailed()) {
                fail("gate driver not ready");
            } else if (m_hw.isReady()) {
                PWM_ClearFault();
                PWM_StartPhase(0);
                PWM_StartPhase(1);
                PWM_StartPhase(2);
                PWM_ResetSPWMElectricalCycles();
                PWM_StartSPWM(RAMP_FREQUENCY_HZ, 0.0f);
                Telemetry::printf("[CAL] BREAK: starting ramp to %.2f V in %.2f V steps",
                                  static_cast<double>(m_max_voltage),
                                  static_cast<double>(m_step_voltage));
                enterState(State::RAMP);
            } else if ((now_ms - m_state_start_ms) > HW_INIT_TIMEOUT_MS) {
                fail("gate driver init timeout");
            }
            break;
        }

        case State::RAMP: {
            updateRamp();
            break;
        }

        case State::DONE:
        case State::FAIL:
        case State::IDLE:
        default:
            break;
    }
}

void BreakawayCalibrator::updateRamp() {
    const uint32_t now_ms = HAL_GetTick();
    const float vdc = dcLinkVoltageSensor().voltage();
    const float cycles = m_tracker.mechanicalCycles();

    if (!m_have_start) {
        m_start_cycles = cycles;
        m_have_start = true;
    }
    const float moved_cycles = cycles - m_start_cycles;

    /* Step the voltage ramp. */
    if ((now_ms - m_last_ramp_ms) >= m_ramp_period_ms) {
        m_last_ramp_ms = now_ms;
        m_mod += voltageToMod(m_step_voltage, vdc);
        const float max_mod = voltageToMod(m_max_voltage, vdc);
        if (m_mod > max_mod) {
            m_mod = max_mod;
        }
    }

    PWM_SetSPWMParams(RAMP_FREQUENCY_HZ, m_mod);

    /* Do not trust encoder motion until the applied voltage is high enough to
     * produce real torque.  Floating sin/cos inputs often show smooth phantom
     * rotation at zero voltage. */
    if (!m_in_trusted_region) {
        if (std::fabs(moved_cycles) > m_max_phantom_cycles) {
            m_max_phantom_cycles = std::fabs(moved_cycles);
        }
        const float v_applied = modToVoltage(m_mod, vdc);
        if (v_applied < MIN_TRUSTED_VOLTAGE_V) {
            return;
        }
        Telemetry::printf("[CAL] BREAK: entering trusted region at mod=%.3f (%.2f V), discarding %.3f cycles of phantom motion",
                          static_cast<double>(m_mod),
                          static_cast<double>(v_applied),
                          static_cast<double>(moved_cycles));
        m_in_trusted_region = true;
        m_start_cycles = cycles;
        return;
    }

    const float moved_since_trusted = cycles - m_start_cycles;

    if (std::fabs(moved_since_trusted) >= m_detect_cycles) {
        m_breakaway_mod = m_mod;
        m_breakaway_voltage = modToVoltage(m_mod, vdc);
        CalKvStore::saveBreakaway(m_breakaway_mod);
        PWM_StopSPWM();
        CalibrationHardware::shutdown();
        Telemetry::printf("[CAL] BREAK: DONE: breakaway at mod=%.3f (%.2f V), moved=%.3f cycles",
                          static_cast<double>(m_breakaway_mod),
                          static_cast<double>(m_breakaway_voltage),
                          static_cast<double>(moved_since_trusted));
        m_state = State::DONE;
        return;
    }

    if (m_mod >= voltageToMod(m_max_voltage, vdc) &&
        (now_ms - m_state_start_ms) > RAMP_TIMEOUT_MS) {
        fail("ramp timeout: rotor did not move");
    }
}

BreakawayCalibrator& breakawayCalibrator() {
    return BreakawayCalibrator::instance();
}

} // namespace Inverter
