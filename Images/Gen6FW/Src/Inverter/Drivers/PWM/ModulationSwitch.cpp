#include "Inverter/Drivers/PWM/ModulationSwitch.h"

#include "Inverter/Control/FocControlManager.h"
#include "Inverter/Control/OpenLoopController.h"
#include "Inverter/Drivers/PWM/Modulator.h"
#include "Inverter/Drivers/PWM/pwm.h"
#include "Inverter/Telemetry.h"

namespace Inverter {
namespace {

/* Where the pattern mode was entered from: decides what modulationToRamp()
 * resumes. */
bool s_patternFromFoc = false;

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
        /* FOC -> pattern: capture the applied voltage-vector angle and
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
