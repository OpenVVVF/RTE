#include "Inverter/Command/CommandInterface.h"
#include "Inverter/Command/CommandContext.h"
#include "Inverter/Control/FocControlManager.h"
#include "Inverter/Control/OpenLoopController.h"
#include "Inverter/Control/FaultManager.h"
#include "Inverter/Drivers/GateDriver/gate_driver.h"
#include "Inverter/Drivers/PWM/pwm.h"
#include "Inverter/Drivers/Sensors/DcLinkVoltageSensor.h"
#include "Inverter/Drivers/Sensors/DcLinkCurrentSensor.h"
#include "Inverter/Drivers/Sensors/EncoderADC.h"
#include "Inverter/Drivers/Sensors/PhaseCurrentADC.h"
#include "Inverter/Telemetry.h"
#include "Inverter/platform_api.h"

#include "main.h"

#include <cmath>

static bool stringsEqual(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

using Inverter::dcLinkVoltageSensor;
using Inverter::encoderADC;
using Inverter::FocControlManager;
using Inverter::OpenLoopController;
using Inverter::FaultManager;
using Inverter::focControlManager;
using Inverter::openLoopController;
using Inverter::phaseCurrentADC;

/* Enable gate-driver outputs with the proven OpenLoopController::start
 * timing: a 1 ms RESET assert clears any latched /FLT (stays under the
 * NCx5710y 8-10 ms DSCHK window), then 100 ms for the charge pump before
 * /RDY+/FLT are meaningful.  Must run unconditionally: after a stop, RESET
 * is asserted while /RDY can still read high, so a conditional "is it
 * healthy?" enable silently leaves the outputs off. */
static bool gateDriverEnableForPulseTest() {
    GateDriver_DisableOutputs();
    HAL_Delay(1);
    GateDriver_EnableOutputs();
    HAL_Delay(100);
    return GateDriver_IsReady() && !GateDriver_IsFault();
}

class StartCommand : public CommandInterface {
public:
    StartCommand()
      : CommandInterface("start", "Start open-loop PWM output",
            {ArgSpec{"freq_hz", "Hz", 0.0f, 1000.0f, 0.0f, true, ArgSpec::FLOAT},
             ArgSpec{"mod_idx", "", 0.0f, 1.2f, 0.0f, true, ArgSpec::FLOAT}}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        openLoopController().start(args[0].f_val, args[1].f_val);
    }
};

class StopCommand : public CommandInterface {
public:
    StopCommand() : CommandInterface("stop", "Stop open-loop PWM output") {}

    void execute(const ArgValue*, CommandContext&) override {
        openLoopController().stop();
    }
};

class FreqCommand : public CommandInterface {
public:
    FreqCommand()
      : CommandInterface("freq", "Set open-loop frequency",
            ArgSpec{"freq_hz", "Hz", 0.0f, 1000.0f, 0.0f, true, ArgSpec::FLOAT}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        openLoopController().setFrequency(args[0].f_val);
        Telemetry::printf("[SHELL] freq set to %.2f Hz", static_cast<double>(args[0].f_val));
    }
};

class ModCommand : public CommandInterface {
public:
    ModCommand()
      : CommandInterface("mod", "Set open-loop modulation index",
            ArgSpec{"mod_idx", "", 0.0f, 1.2f, 0.0f, true, ArgSpec::FLOAT}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        openLoopController().setModulationIndex(args[0].f_val);
        Telemetry::printf("[SHELL] mod set to %.3f", static_cast<double>(args[0].f_val));
    }
};

class SwFreqCommand : public CommandInterface {
public:
    SwFreqCommand()
      : CommandInterface("swfreq", "Set PWM switching frequency [Hz]",
            ArgSpec{"freq_hz", "Hz", 1000.0f, 16000.0f, 0.0f, false, ArgSpec::FLOAT}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        if (!args[0].present) {
            Telemetry::printf("[SHELL] swfreq = %.0f Hz (update %.0f Hz)",
                              static_cast<double>(PWM_GetFrequency()),
                              static_cast<double>(PWM_GetUpdateFrequency()));
            return;
        }
        if (openLoopController().isRunning() || focControlManager().isRunning()) {
            Telemetry::printf("[SHELL] stop the motor before changing swfreq");
            return;
        }
        PWM_SetFrequency(static_cast<uint32_t>(args[0].f_val + 0.5f));
        Telemetry::printf("[SHELL] swfreq set to %.0f Hz (update %.0f Hz)",
                          static_cast<double>(PWM_GetFrequency()),
                          static_cast<double>(PWM_GetUpdateFrequency()));
    }
};

class StatusCommand : public CommandInterface {
public:
    StatusCommand() : CommandInterface("status", "Show OL controller and fault status") {}

    void execute(const ArgValue*, CommandContext&) override {
        OpenLoopController& ol = openLoopController();
        Telemetry::printf("[SHELL] run=%s f=%.2f m=%.3f",
                          ol.isRunning() ? "Y" : "N",
                          static_cast<double>(ol.frequencyHz()),
                          static_cast<double>(ol.modulationIndex()));
        Telemetry::printf("[SHELL] ready=%s gd_fault=%s",
                          GateDriver_IsReady() ? "Y" : "N",
                          GateDriver_IsFault() ? "Y" : "N");
        Telemetry::printf("[SHELL] pins: RESET(PD5 odr)=%d POWER(PC10 odr)=%d SLEEP(PD6 odr)=%d",
                          (GPIOD->ODR & GPIO_PIN_5) ? 1 : 0,
                          (GPIOC->ODR & GPIO_PIN_10) ? 1 : 0,
                          (GPIOD->ODR & GPIO_PIN_6) ? 1 : 0);
        FaultManager::instance().printSummary();
    }
};

class RampCurrentLimitCommand : public CommandInterface {
public:
    RampCurrentLimitCommand()
      : CommandInterface("rclimit", "Set ramp current limit",
            ArgSpec{"amps", "A", 0.0f, 2000.0f, 0.0f, true, ArgSpec::FLOAT}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        float amps = args[0].f_val;
        if (amps < 0.0f) amps = 0.0f;
        openLoopController().setRampCurrentLimit(amps);
        Telemetry::printf("[SHELL] ramp current limit set to %.3f A", static_cast<double>(amps));
    }
};

/**
 * @brief Apply short static voltage-vector pulses at 0/90/180/270 deg and log
 * the sensor response.
 *
 * Useful for verifying current-sensor polarity, encoder offset sign, and phase
 * mapping before running closed-loop FOC.  The command is blocking and parks
 * the outputs at 50 % when finished.
 *
 * Because the motor resistance is very low, long DC vectors would pull huge
 * current.  Each vector is held only for a short pulse so the current stays
 * small but the polarity is still visible.
 *
 * Usage: vectorscan <mod_idx> <pulse_ms> [max_a]
 */
class VectorScanCommand : public CommandInterface {
public:
    VectorScanCommand()
      : CommandInterface("vectorscan", "Apply 0/90/180/270 deg voltage pulses and log currents/encoder",
            {ArgSpec{"mod_idx", "", 0.0f, 1.2f, 0.05f, true, ArgSpec::FLOAT},
             ArgSpec{"pulse_ms", "ms", 1.0f, 5000.0f, 10.0f, true, ArgSpec::FLOAT},
             ArgSpec{"max_a", "A", 0.0f, 2000.0f, 0.0f, false, ArgSpec::FLOAT}}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        const float mod_idx = args[0].f_val;
        const uint32_t pulse_ms = static_cast<uint32_t>(args[1].f_val);
        const float max_a = args[2].present ? args[2].f_val : 0.0f;

        if (focControlManager().isRunning()) {
            focControlManager().stop();
            Telemetry::printf("[SHELL] stopped FOC first");
        }
        if (openLoopController().isRunning()) {
            openLoopController().stop();
            Telemetry::printf("[SHELL] stopped open-loop first");
        }

        if (FaultManager::instance().isSeverityActive(Inverter::FaultSeverity::Critical) ||
            FaultManager::instance().isSeverityActive(Inverter::FaultSeverity::High)) {
            Telemetry::printf("[SHELL] active Critical/High faults, cannot run vectorscan");
            FaultManager::instance().printSummary();
            return;
        }

        PWM_SetThreePhaseDuty(50.0f, 50.0f, 50.0f);
        if (!gateDriverEnableForPulseTest()) {
            Telemetry::printf("[SHELL] ERROR: gate driver not ready or fault latched");
            GateDriver_DisableOutputs();
            return;
        }

        PWM_ClearFault();
        PWM_Start();

        Telemetry::printf("[SHELL] vectorscan mod=%.3f pulse=%lu ms max_a=%.1f A",
                          static_cast<double>(mod_idx),
                          static_cast<unsigned long>(pulse_ms),
                          static_cast<double>(max_a));

        static constexpr uint32_t ANGLE_COUNT = 4U;
        const float angles_deg[ANGLE_COUNT] = {0.0f, 90.0f, 180.0f, 270.0f};
        bool aborted = false;

        for (uint32_t i = 0; i < ANGLE_COUNT && !aborted; ++i) {
            const float angle_rad = angles_deg[i] * (3.14159265358979323846f / 180.0f);
            PWM_SetVoltageAngle(angle_rad, mod_idx);
            HAL_Delay(pulse_ms);

            float iu = 0.0f, iv = 0.0f, iw = 0.0f;
            (void)phaseCurrentADC().sample(iu, iv, iw);
            const float enc_deg = encoderADC().lastAngle();
            const float vdc = dcLinkVoltageSensor().voltage();

            Telemetry::printf("[SHELL] vectorscan angle=%.0f deg | iu=%+.2f iv=%+.2f iw=%+.2f | enc=%.1f | vdc=%.1f",
                              static_cast<double>(angles_deg[i]),
                              static_cast<double>(iu),
                              static_cast<double>(iv),
                              static_cast<double>(iw),
                              static_cast<double>(enc_deg),
                              static_cast<double>(vdc));

            if (max_a > 0.0f) {
                float peak = std::fabs(iu);
                if (std::fabs(iv) > peak) peak = std::fabs(iv);
                if (std::fabs(iw) > peak) peak = std::fabs(iw);
                if (peak > max_a) {
                    Telemetry::printf("[SHELL] vectorscan ABORT: current %.1f A exceeded limit %.1f A",
                                      static_cast<double>(peak),
                                      static_cast<double>(max_a));
                    aborted = true;
                }
            }
        }

        PWM_SetThreePhaseDuty(50.0f, 50.0f, 50.0f);
        GateDriver_DisableOutputs();
        Telemetry::printf("[SHELL] vectorscan done%s", aborted ? " (aborted)" : "");
    }
};

/**
 * @brief Induction-motor scalar (V/Hz) control.
 *
 * The simplest way to spin an induction machine: apply a rotating voltage vector
 * whose magnitude is roughly proportional to frequency.  This is not field-oriented
 * control — there is no current loop and no slip control — but it will spin the
 * rotor if the V/Hz ratio is in the right ballpark.
 */
class InductionCommand : public CommandInterface {
public:
    InductionCommand()
      : CommandInterface("induction",
            "Induction motor scalar control: induction start <freq_hz> <mod_idx> | stop | demo",
            {ArgSpec{"subcmd", "", 0.0f, 0.0f, 0.0f, true, ArgSpec::STRING},
             ArgSpec{"freq_hz", "Hz", -200.0f, 200.0f, 0.0f, false, ArgSpec::FLOAT},
             ArgSpec{"mod_idx", "", 0.0f, 1.0f, 0.0f, false, ArgSpec::FLOAT}}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        const char* sub = args[0].s_val;

        if (stringsEqual(sub, "stop")) {
            openLoopController().stop();
            Telemetry::printf("[SHELL] induction stopped");
            return;
        }

        if (stringsEqual(sub, "start")) {
            if (!args[1].present || !args[2].present) {
                Telemetry::printf("[SHELL] usage: induction start <freq_hz> <mod_idx>");
                return;
            }
            const float freq = args[1].f_val;
            const float mod = args[2].f_val;
            if (focControlManager().isRunning()) {
                focControlManager().stop();
                Telemetry::printf("[SHELL] stopped FOC first");
            }
            openLoopController().start(freq, mod);
            Telemetry::printf("[SHELL] induction start f=%.2f Hz mod=%.3f", static_cast<double>(freq), static_cast<double>(mod));
            return;
        }

        if (stringsEqual(sub, "demo")) {
            if (focControlManager().isRunning()) {
                focControlManager().stop();
                Telemetry::printf("[SHELL] stopped FOC first");
            }
            Telemetry::printf("[SHELL] induction demo: ramping 0 -> 30 Hz, mod 0.02 -> 0.20");
            openLoopController().start(0.0f, 0.02f);
            constexpr float F_MAX = 30.0f;
            constexpr float M_MAX = 0.20f;
            constexpr uint32_t RAMP_MS = 6000U;
            constexpr uint32_t HOLD_MS = 3000U;
            constexpr uint32_t STEP_MS = 100U;
            const uint32_t steps = RAMP_MS / STEP_MS;
            for (uint32_t i = 0; i <= steps; ++i) {
                const float frac = static_cast<float>(i) / static_cast<float>(steps);
                openLoopController().setFrequency(frac * F_MAX);
                openLoopController().setModulationIndex(0.02f + frac * (M_MAX - 0.02f));
                HAL_Delay(STEP_MS);
            }
            Telemetry::printf("[SHELL] induction demo: holding 30 Hz / mod 0.20");
            HAL_Delay(HOLD_MS);
            for (uint32_t i = 0; i <= steps; ++i) {
                const float frac = 1.0f - static_cast<float>(i) / static_cast<float>(steps);
                openLoopController().setFrequency(frac * F_MAX);
                openLoopController().setModulationIndex(0.02f + frac * (M_MAX - 0.02f));
                HAL_Delay(STEP_MS);
            }
            openLoopController().stop();
            Telemetry::printf("[SHELL] induction demo done");
            return;
        }

        Telemetry::printf("[SHELL] induction: unknown subcommand '%s' (start/stop/demo)", sub);
    }
};

static StartCommand    sStartCmd;
static StopCommand     sStopCmd;
static FreqCommand     sFreqCmd;
static ModCommand      sModCmd;
static SwFreqCommand   sSwFreqCmd;
static StatusCommand   sStatusCmd;
static RampCurrentLimitCommand sRampCurrentLimitCmd;
static VectorScanCommand sVectorScanCmd;

/**
 * @brief Fire a single half-bridge for a fixed time (gate-driver bringup aid).
 *
 * gatefire <phase 0..2> <duty_pct 0..100> <ms>
 *
 * Parks the other two phases at 50 % (gate off) and runs only the requested
 * phase channel for <ms>.  Duty > 50 % pulses the high-side switch, < 50 %
 * the low-side.  Prints the phase currents and gate-driver /RDY+/FLT state
 * afterwards so a scope can be correlated one switch at a time.
 */
class GateFireCommand : public CommandInterface {
public:
    GateFireCommand()
      : CommandInterface("gatefire",
            "Fire one half-bridge: gatefire <phase 0-2> <duty_pct 0-100> <ms>",
            {ArgSpec{"phase", "", 0.0f, 2.0f, 0.0f, true, ArgSpec::FLOAT},
             ArgSpec{"duty_pct", "%", 0.0f, 100.0f, 50.0f, true, ArgSpec::FLOAT},
             ArgSpec{"ms", "ms", 1.0f, 5000.0f, 10.0f, true, ArgSpec::FLOAT}}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        const uint8_t phase = static_cast<uint8_t>(args[0].f_val + 0.5f);
        const float duty = args[1].f_val;
        const uint32_t ms = static_cast<uint32_t>(args[2].f_val + 0.5f);

        if (focControlManager().isRunning()) {
            focControlManager().stop();
            Telemetry::printf("[SHELL] stopped FOC first");
        }
        if (openLoopController().isRunning()) {
            openLoopController().stop();
            Telemetry::printf("[SHELL] stopped open-loop first");
        }

        if (FaultManager::instance().isSeverityActive(Inverter::FaultSeverity::Critical) ||
            FaultManager::instance().isSeverityActive(Inverter::FaultSeverity::High)) {
            Telemetry::printf("[SHELL] active Critical/High faults, cannot fire");
            FaultManager::instance().printSummary();
            return;
        }

        float du = 50.0f, dv = 50.0f, dw = 50.0f;
        float* duties[3] = {&du, &dv, &dw};
        *duties[phase] = duty;
        PWM_SetThreePhaseDuty(du, dv, dw);

        if (!gateDriverEnableForPulseTest()) {
            Telemetry::printf("[GF] ERROR: gate driver not ready or fault latched");
            GateDriver_DisableOutputs();
            return;
        }

        PWM_ClearFault();
        PWM_StartPhase(phase);
        HAL_Delay(ms);

        /* Sample mid-pulse (gate driver still enabled, phase still firing) so
         * the DC-link shunt arbitrates whether phase currents are real. */
        float iu = 0.0f, iv = 0.0f, iw = 0.0f;
        (void)phaseCurrentADC().sample(iu, iv, iw);
        const float vdc = dcLinkVoltageSensor().voltage();
        float dcl_i = Inverter::dcLinkCurrentSensor().current();
        const float enc_deg = encoderADC().extrapolatedAngleDeg();

        PWM_StopPhase(phase);
        PWM_SetThreePhaseDuty(50.0f, 50.0f, 50.0f);
        GateDriver_DisableOutputs();

        Telemetry::printf("[GF] phase=%u duty=%.1f%% t=%lu ms | iu=%+.2f iv=%+.2f iw=%+.2f | dcl_i=%+.2f enc=%.1f | vdc=%.1f | ready=%s fault=%s",
                          static_cast<unsigned>(phase),
                          static_cast<double>(duty),
                          static_cast<unsigned long>(ms),
                          static_cast<double>(iu),
                          static_cast<double>(iv),
                          static_cast<double>(iw),
                          static_cast<double>(dcl_i),
                          static_cast<double>(enc_deg),
                          static_cast<double>(vdc),
                          GateDriver_IsReady() ? "Y" : "N",
                          GateDriver_IsFault() ? "Y" : "N");
    }
};
static GateFireCommand sGateFireCmd;

/*
 * phasemap [settle_ms]
 *
 * Phase-drive -> voltage-sense map check.  REQUIRES the motor disconnected
 * (phase outputs floating): each half-bridge then drives its own output pin
 * and the MAX22530 phase-voltage channels read it back.
 *
 * Static levels only - no PWM averaging/aliasing: each phase in turn is
 * driven at 100% duty (high-side conducts continuously -> output at DC+)
 * then 0% duty (low-side conducts continuously -> output at GND).  All
 * other phases are parked at 0% duty (outputs solidly at GND).
 *
 * A healthy, correctly-mapped phase shows a full-span positive swing on its
 * OWN sense channel while the parked channels stay near GND.
 */
class PhaseMapCommand : public CommandInterface {
public:
    PhaseMapCommand()
      : CommandInterface("phasemap",
            "Verify phase drive->sense map (motor disconnected!): phasemap [settle_ms]",
            {ArgSpec{"settle_ms", "ms", 20.0f, 2000.0f, 250.0f, false, ArgSpec::FLOAT}}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        const uint32_t settle_ms =
            static_cast<uint32_t>((args[0].present ? args[0].f_val : 250.0f) + 0.5f);

        if (focControlManager().isRunning()) {
            focControlManager().stop();
            Telemetry::printf("[SHELL] stopped FOC first");
        }
        if (openLoopController().isRunning()) {
            openLoopController().stop();
            Telemetry::printf("[SHELL] stopped open-loop first");
        }

        if (FaultManager::instance().isSeverityActive(Inverter::FaultSeverity::Critical) ||
            FaultManager::instance().isSeverityActive(Inverter::FaultSeverity::High)) {
            Telemetry::printf("[PM] active Critical/High faults, cannot run");
            FaultManager::instance().printSummary();
            return;
        }

        const float vdc = dcLinkVoltageSensor().voltage();
        if (vdc < 5.0f) {
            Telemetry::printf("[PM] ERROR: vdc=%.1f V too low for a meaningful test",
                              static_cast<double>(vdc));
            return;
        }

        PWM_SetThreePhaseDuty(50.0f, 50.0f, 50.0f);
        if (!gateDriverEnableForPulseTest()) {
            Telemetry::printf("[PM] ERROR: gate driver not ready or fault latched");
            GateDriver_DisableOutputs();
            return;
        }

        PWM_ClearFault();
        PWM_Start();

        /* Phase-sense channels share the DC-link divider ratio, so a full
         * GND->DC+ swing should produce ~1.0x the DC-link channel's raw
         * input volts on the responding sense channel. */
        const float expected_swing = 1.0f * Inverter::dcLinkVoltageSensor().adc().voltage(3);
        const char names[3] = {'U', 'V', 'W'};
        bool all_ok = true;

        Telemetry::printf("[PM] vdc=%.1f V, expected sense swing ~%+.3f raw V",
                          static_cast<double>(vdc), static_cast<double>(expected_swing));

        for (uint8_t p = 0; p < 3; ++p) {
            float du = 0.0f, dv = 0.0f, dw = 0.0f;  /* parked: solid GND */
            float* duties[3] = {&du, &dv, &dw};
            float vhi[3], vlo[3], swing[3];

            *duties[p] = 100.0f;  /* high-side on -> DC+ */
            PWM_SetThreePhaseDuty(du, dv, dw);
            HAL_Delay(settle_ms);
            vhi[0] = platform_phase_voltage_u();
            vhi[1] = platform_phase_voltage_v();
            vhi[2] = platform_phase_voltage_w();

            *duties[p] = 0.0f;    /* low-side on -> GND */
            PWM_SetThreePhaseDuty(du, dv, dw);
            HAL_Delay(settle_ms);
            vlo[0] = platform_phase_voltage_u();
            vlo[1] = platform_phase_voltage_v();
            vlo[2] = platform_phase_voltage_w();

            int best = 0;
            for (int c = 0; c < 3; ++c) {
                swing[c] = vhi[c] - vlo[c];
                if (swing[c] > swing[best]) best = c;
            }

            const bool own_ok = swing[p] > 0.5f * expected_swing;
            bool cross_ok = true;
            for (int c = 0; c < 3; ++c) {
                if (c != p && std::fabs(swing[c]) > 0.35f * swing[p]) cross_ok = false;
            }
            const bool map_ok = (best == p);
            const bool pass = own_ok && cross_ok && map_ok;
            all_ok = all_ok && pass;

            Telemetry::printf("[PM] drive %c @100%%(=DC+): U=%.3f V=%.3f W=%.3f | @0%%(=GND): U=%.3f V=%.3f W=%.3f",
                              names[p],
                              static_cast<double>(vhi[0]), static_cast<double>(vhi[1]),
                              static_cast<double>(vhi[2]),
                              static_cast<double>(vlo[0]), static_cast<double>(vlo[1]),
                              static_cast<double>(vlo[2]));
            Telemetry::printf("[PM] drive %c: swing U=%+.3f V=%+.3f W=%+.3f responder=%c -> %s%s",
                              names[p],
                              static_cast<double>(swing[0]), static_cast<double>(swing[1]),
                              static_cast<double>(swing[2]), names[best],
                              pass ? "PASS" : (map_ok ? "FAIL (weak/dead drive or sense)"
                                                      : "FAIL (MAPPING: drive and sense disagree)"),
                              cross_ok ? "" : " +cross-talk");
        }

        PWM_Stop();
        PWM_SetThreePhaseDuty(50.0f, 50.0f, 50.0f);
        GateDriver_DisableOutputs();

        Telemetry::printf("[PM] overall: %s | ready=%s fault=%s",
                          all_ok ? "PASS" : "FAIL",
                          GateDriver_IsReady() ? "Y" : "N",
                          GateDriver_IsFault() ? "Y" : "N");
    }
};
static PhaseMapCommand sPhaseMapCmd;

/* Induction command object is larger than the others; keep it out of DTCM. */
static InductionCommand sInductionCmd __attribute__((section(".dma_buffers")));

#include "Inverter/Command/CommandManager.h"

void registerOpenLoopCommands(CommandManager& mgr) {
    mgr.registerCommand(&sStartCmd);
    mgr.registerCommand(&sStopCmd);
    mgr.registerCommand(&sFreqCmd);
    mgr.registerCommand(&sModCmd);
    mgr.registerCommand(&sSwFreqCmd);
    mgr.registerCommand(&sStatusCmd);
    mgr.registerCommand(&sRampCurrentLimitCmd);
    mgr.registerCommand(&sVectorScanCmd);
    mgr.registerCommand(&sGateFireCmd);
    mgr.registerCommand(&sPhaseMapCmd);
    mgr.registerCommand(&sInductionCmd);
}
