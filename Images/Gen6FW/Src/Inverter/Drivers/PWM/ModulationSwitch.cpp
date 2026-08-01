#include "Inverter/Drivers/PWM/ModulationSwitch.h"

#include "Inverter/Calibration/MotorCalibration.h"
#include "Inverter/Control/ControlSupervisor.h"
#include "Inverter/Control/FocControlManager.h"
#include "Inverter/Control/MotorParameters.h"
#include "Inverter/Control/OpenLoopController.h"
#include "Inverter/Drivers/PWM/Modulator.h"
#include "Inverter/Drivers/PWM/pwm.h"
#include "Inverter/Drivers/Sensors/EncoderADC.h"
#include "Inverter/Telemetry.h"

namespace Inverter {
namespace {

/* Where the pattern mode was entered from: decides what modulationToRamp()
 * resumes. */
bool s_patternFromFoc = false;
bool s_patternFromGraph = false;

} // namespace

ModulationMode modulationMode() {
    return (activeModulator() == &shepwmModulator())
               ? ModulationMode::Pattern
               : ModulationMode::Ramp;
}

bool modulationToPattern(uint32_t pulses_per_quarter, float duty) {
    if (shepwmIsRunning()) {
        return true;   /* already there */
    }

    if (focControlManager().isRunning()) {
        /* Legacy FOC -> pattern: capture the applied voltage-vector angle and
         * electrical frequency, suspend the current loop (outputs stay
         * live), and enter the pattern phase-locked to that angle. */
        const float angle = focControlManager().electricalVoltageAngleRad();
        float fe = focControlManager().electricalSpeedRadPerSec() / 6.283185307f;
        if (fe < 0.0f) fe = -fe;
        if (fe < 0.1f) {
            return false;
        }

        shepwmSetPulsePattern(fe, pulses_per_quarter, duty);
        focControlManager().suspendForHandoff();
        if (!shepwmModulator().enter(angle, duty)) {
            (void)focControlManager().restartLastSetpoints();
            return false;
        }
        setActiveModulator(&shepwmModulator());
        s_patternFromFoc = true;

        Telemetry::printf("[MOD] FOC -> pattern: fe=%.2f Hz pulses/qtr=%lu duty=%.3f",
                          static_cast<double>(fe),
                          static_cast<unsigned long>(pulses_per_quarter),
                          static_cast<double>(duty));
        return true;
    }

    if (ControlSupervisor::instance().isRunning()) {
        /* Graph FOC -> pattern: the generated control step dies with the
         * TIM1 update ISR; outputs (and MOE) stay live.  Angle from the
         * encoder + calibration, led by a q-axis approximation. */
        const MotorCalibration& cal = MotorCalibration::instance();
        if (!cal.valid || cal.pole_count <= 0.0f) {
            Telemetry::printf("[MOD] ERROR: motor calibration invalid; cannot phase-lock");
            return false;
        }
        const MotorParameters mp = buildMotorParametersFromCalibration(cal, 0.0f);
        constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
        const float rpm = encoderADC().rpmMech();
        float fe = rpm * mp.pole_pairs / 60.0f;
        if (fe < 0.0f) fe = -fe;
        if (fe < 0.1f) {
            return false;
        }
        const float lead = (rpm >= 0.0f) ? 1.5707963268f : -1.5707963268f;
        float angle = mp.encoder_offset_rad +
                      mp.encoder_sign * encoderADC().extrapolatedAngleDeg() * kDegToRad *
                          mp.pole_pairs / mp.encoder_cycles_per_rev + lead;

        shepwmSetPulsePattern(fe, pulses_per_quarter, duty);
        PWM_DisableFocMode();
        PWM_StopUpdateInterrupt();
        if (!shepwmModulator().enter(angle, duty)) {
            PWM_EnableFocMode();
            PWM_StartUpdateInterrupt();
            return false;
        }
        setActiveModulator(&shepwmModulator());
        s_patternFromGraph = true;

        Telemetry::printf("[MOD] graph-FOC -> pattern: fe=%.2f Hz pulses/qtr=%lu duty=%.3f",
                          static_cast<double>(fe),
                          static_cast<unsigned long>(pulses_per_quarter),
                          static_cast<double>(duty));
        return true;
    }

    if (!spwmIsRunning()) {
        return false;
    }

    const float angle = spwmAngleRad();
    const float fe = spwmFundamentalFreqHz();

    shepwmSetPulsePattern(fe, pulses_per_quarter, duty);
    PWM_StopSPWM();
    if (!shepwmModulator().enter(angle, duty)) {
        /* Restore the ramp where it was. */
        PWM_StartSPWM(fe, spwmModulationIndex());
        spwmSetAngle(angle);
        return false;
    }
    setActiveModulator(&shepwmModulator());
    s_patternFromFoc = false;
    s_patternFromGraph = false;

    Telemetry::printf("[MOD] ramp -> pattern: fe=%.2f Hz pulses/qtr=%lu duty=%.3f",
                      static_cast<double>(fe),
                      static_cast<unsigned long>(pulses_per_quarter),
                      static_cast<double>(duty));
    return true;
}

bool modulationToRamp() {
    if (!shepwmIsRunning()) {
        return true;   /* already there */
    }

    if (s_patternFromFoc) {
        /* Pattern -> FOC: release the pins and restart the current loop
         * with the last setpoints (flying restart into the spinning motor). */
        s_patternFromFoc = false;
        shepwmModulator().exit();
        setActiveModulator(nullptr);
        const bool ok = focControlManager().restartLastSetpoints();
        Telemetry::printf("[MOD] pattern -> FOC: %s", ok ? "restarted" : "RESTART FAILED");
        return ok;
    }

    if (s_patternFromGraph) {
        /* Pattern -> graph FOC: re-enable the update ISR; the generated
         * domain resumes with its preserved state (ControlSupervisor never
         * left Running). */
        s_patternFromGraph = false;
        shepwmModulator().exit();
        setActiveModulator(nullptr);
        PWM_EnableFocMode();
        PWM_StartUpdateInterrupt();
        Telemetry::printf("[MOD] pattern -> graph FOC: resumed");
        return true;
    }

    const float angle = shepwmAngleRad();
    const float fe = shepwmFrequencyHz();
    const float mi = openLoopController().modulationIndex();

    shepwmModulator().exit();
    setActiveModulator(nullptr);
    PWM_StartSPWM(fe, mi);
    spwmSetAngle(angle);

    Telemetry::printf("[MOD] pattern -> ramp: fe=%.2f Hz mi=%.3f",
                      static_cast<double>(fe), static_cast<double>(mi));
    return true;
}

} // namespace Inverter
