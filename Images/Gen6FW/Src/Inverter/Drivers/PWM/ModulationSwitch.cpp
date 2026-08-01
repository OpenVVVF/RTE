#include "Inverter/Drivers/PWM/ModulationSwitch.h"

#include "Inverter/Control/FocControlManager.h"
#include "Inverter/Control/OpenLoopController.h"
#include "Inverter/Drivers/PWM/Modulator.h"
#include "Inverter/Drivers/PWM/pwm.h"
#include "Inverter/Telemetry.h"

namespace Inverter {

ModulationMode modulationMode() {
    return (activeModulator() == &shepwmModulator())
               ? ModulationMode::Pattern
               : ModulationMode::Ramp;
}

bool modulationToPattern(uint32_t pulses_per_quarter, float duty) {
    if (focControlManager().isRunning() || !spwmIsRunning()) {
        return false;
    }
    if (shepwmIsRunning()) {
        return true;   /* already there */
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
