#include "Inverter/Command/CommandInterface.h"
#include "Inverter/Command/CommandContext.h"
#include "Inverter/Command/CommandManager.h"
#include "Inverter/Control/OpenLoopController.h"
#include "Inverter/Calibration/PoleCalibrator.h"
#include "Inverter/Calibration/EncoderOffsetCalibrator.h"
#include "Inverter/Calibration/EncoderLinearityCalibrator.h"
#include "Inverter/Calibration/ResistanceCalibrator.h"
#include "Inverter/Calibration/InductanceCalibrator.h"
#include "Inverter/Calibration/InductionMotorCalibrator.h"
#include "Inverter/Calibration/InductionVHzCalibrator.h"
#include "Inverter/Calibration/FluxLinkageCalibrator.h"
#include "Inverter/Calibration/EncoderCycleCalibrator.h"
#include "Inverter/Calibration/AutoCalibrationCoordinator.h"
#include "Inverter/Calibration/BreakawayCalibrator.h"
#include "Inverter/Calibration/CalKvStore.h"
#include "Inverter/Calibration/MotorCalibration.h"
#include "Inverter/Drivers/Storage/MotorConfigStore.h"
#include "Inverter/Drivers/Sensors/PoleEstimator.h"
#include "Inverter/Drivers/Storage/RteParamStore.h"
#include "Inverter/Telemetry.h"

#include <cstring>
#include <cctype>
#include <cmath>
#include <cstdlib>

using Inverter::OpenLoopController;
using Inverter::PoleEstimator;
using Inverter::PoleCalibrator;
using Inverter::EncoderOffsetCalibrator;
using Inverter::EncoderLinearityCalibrator;
using Inverter::ResistanceCalibrator;
using Inverter::InductanceCalibrator;
using Inverter::InductionMotorCalibrator;
using Inverter::InductionVHzCalibrator;
using Inverter::FluxLinkageCalibrator;
using Inverter::EncoderCycleCalibrator;
using Inverter::AutoCalibrationCoordinator;
using Inverter::EncoderADC;
using Inverter::MotorType;
using Inverter::openLoopController;
using Inverter::poleCalibrator;
using Inverter::encoderOffsetCalibrator;
using Inverter::encoderLinearityCalibrator;
using Inverter::resistanceCalibrator;
using Inverter::inductanceCalibrator;
using Inverter::inductionMotorCalibrator;
using Inverter::inductionVHzCalibrator;
using Inverter::fluxLinkageCalibrator;
using Inverter::motorCalibration;
using Inverter::autoCalibrationCoordinator;
using Inverter::breakawayCalibrator;
using Inverter::encoderADC;

static bool stringsEqual(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) return false;
    while (*a && *b) {
        if (std::tolower(static_cast<unsigned char>(*a)) !=
            std::tolower(static_cast<unsigned char>(*b))) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

static bool parsePair(const char* token, ResistanceCalibrator::Pair& out) {
    if (stringsEqual(token, "uv")) {
        out = ResistanceCalibrator::Pair::UV;
        return true;
    }
    if (stringsEqual(token, "uw")) {
        out = ResistanceCalibrator::Pair::UW;
        return true;
    }
    if (stringsEqual(token, "vw")) {
        out = ResistanceCalibrator::Pair::VW;
        return true;
    }
    return false;
}

class PolesCommand : public CommandInterface {
public:
    PolesCommand() : CommandInterface("poles", "Print estimated pole count and cycle counts") {}

    void execute(const ArgValue*, CommandContext&) override {
        PoleEstimator& poles = PoleEstimator::instance();
        Telemetry::printf("[CAL] poles=%.3f mech=%.2f elec=%.2f",
                          static_cast<double>(poles.estimate()),
                          static_cast<double>(poles.mechanicalCycles()),
                          static_cast<double>(poles.electricalCycles()));
    }
};

class EncoderCalCommand : public CommandInterface {
public:
    EncoderCalCommand()
      : CommandInterface("encodercal", "Manual one-revolution encoder-cycle calibration",
            ArgSpec{"subcmd", "", 0.0f, 0.0f, 0.0f, true, ArgSpec::STRING}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        EncoderCycleCalibrator& cal = EncoderCycleCalibrator::instance();
        const char* sub = args[0].s_val;

        if (stringsEqual(sub, "start")) {
            cal.start();
            Telemetry::printf("[CAL] encodercal started; rotate shaft exactly 1 rev, then 'encodercal stop'");
        } else if (stringsEqual(sub, "stop")) {
            cal.stop();
            const float cycles = cal.cycles();
            Telemetry::printf("[CAL] encoder cycles in 1 rev = %.2f; true poles = poles_estimate * %.2f",
                              static_cast<double>(cycles), static_cast<double>(cycles));
        } else if (stringsEqual(sub, "status")) {
            Telemetry::printf("[CAL] encodercal: active=%s cycles=%.2f",
                              cal.isActive() ? "Y" : "N",
                              static_cast<double>(cal.cycles()));
        } else {
            Telemetry::printf("[CAL] encodercal: unknown subcommand '%s' (start/stop/status)", sub);
        }
    }
};

class CalPolesCommand : public CommandInterface {
public:
    CalPolesCommand() : CommandInterface("calpoles", "Start automatic pole calibration") {}

    void execute(const ArgValue*, CommandContext&) override {
        PoleCalibrator& cal = poleCalibrator();
        if (openLoopController().isRunning()) {
            Telemetry::printf("[CAL] stop the motor before starting calpoles");
        } else if (cal.isActive()) {
            Telemetry::printf("[CAL] calibration already running");
        } else {
            cal.start();
        }
    }
};

class EncOffsetCommand : public CommandInterface {
public:
    EncOffsetCommand()
      : CommandInterface("encoffset", "Encoder-offset calibration",
            {ArgSpec{"subcmd", "", 0.0f, 0.0f, 0.0f, true, ArgSpec::STRING},
             ArgSpec{"poles", "", 2.0f, 100.0f, 10.0f, false, ArgSpec::FLOAT},
             ArgSpec{"enc_cycles", "", 0.1f, 100.0f, 1.0f, false, ArgSpec::FLOAT},
             ArgSpec{"breakaway_mod", "", 0.0f, 1.0f, 0.0f, false, ArgSpec::FLOAT}}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        EncoderOffsetCalibrator& cal = encoderOffsetCalibrator();
        const char* sub = args[0].s_val;

        if (stringsEqual(sub, "start")) {
            if (openLoopController().isRunning()) {
                Telemetry::printf("[CAL] stop the motor before starting encoffset");
                return;
            }
            if (cal.isActive()) {
                Telemetry::printf("[CAL] calibration already running");
                return;
            }
            if (!args[1].present || !args[2].present) {
                Telemetry::printf("[CAL] encoffset start <poles> <enc_cycles> [breakaway_mod]");
                return;
            }
            cal.start(args[1].f_val, args[2].f_val, args[3].f_val);
        } else if (stringsEqual(sub, "status")) {
            Telemetry::printf("[CAL] encoffset: samples=%d avg=%.3f deg",
                              cal.sampleCount(),
                              static_cast<double>(cal.averageOffset()));
        } else {
            Telemetry::printf("[CAL] encoffset: unknown subcommand '%s' (start/status)", sub);
        }
    }
};

class ResCalCommand : public CommandInterface {
public:
    ResCalCommand()
      : CommandInterface("rescal", "Resistance calibration",
            {ArgSpec{"subcmd", "", 0.0f, 0.0f, 0.0f, true, ArgSpec::STRING},
             ArgSpec{"pair", "", 0.0f, 0.0f, 0.0f, false, ArgSpec::STRING},
             ArgSpec{"value", "", 0.0f, 1000.0f, 0.0f, false, ArgSpec::FLOAT},
             ArgSpec{"limit", "", 0.0f, 1000.0f, 0.0f, false, ArgSpec::FLOAT}}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        ResistanceCalibrator& rc = resistanceCalibrator();
        const char* sub = args[0].s_val;

        if (stringsEqual(sub, "stop")) {
            rc.stop();
            return;
        }

        if (stringsEqual(sub, "status")) {
            Telemetry::printf("[CAL] rescal: R_uv=%.3f R_uw=%.3f R_vw=%.3f avg=%.3f ohm",
                              static_cast<double>(rc.lastResult(ResistanceCalibrator::Pair::UV)),
                              static_cast<double>(rc.lastResult(ResistanceCalibrator::Pair::UW)),
                              static_cast<double>(rc.lastResult(ResistanceCalibrator::Pair::VW)),
                              static_cast<double>(rc.lastAverage()));
            return;
        }

        const char* pairStr = args[1].present ? args[1].s_val : "all";
        const bool runAll = stringsEqual(pairStr, "all");
        ResistanceCalibrator::Pair pair = ResistanceCalibrator::Pair::UV;
        if (!runAll && !parsePair(pairStr, pair)) {
            Telemetry::printf("[CAL] rescal: pair must be uv, uw, vw, or all");
            return;
        }

        if (stringsEqual(sub, "start")) {
            if (!args[2].present) {
                Telemetry::printf("[CAL] rescal start <pair|all> <bus_pct> [max_a]");
                return;
            }
            const float maxA = args[3].present ? args[3].f_val : 50.0f;
            if (!rc.start(args[2].f_val, pair, runAll, 15000U, maxA)) {
                Telemetry::printf("[CAL] rescal start failed");
            }
        } else if (stringsEqual(sub, "ictrl")) {
            if (!args[2].present) {
                Telemetry::printf("[CAL] rescal ictrl <pair|all> <current_a> [oc_limit_a]");
                return;
            }
            const float ocLimit = args[3].present ? args[3].f_val : 0.0f;
            if (!rc.startCurrentCtrl(args[2].f_val, pair, runAll, 15000U, ocLimit)) {
                Telemetry::printf("[CAL] rescal ictrl failed");
            }
        } else {
            Telemetry::printf("[CAL] rescal: unknown subcommand '%s' (start/ictrl/stop/status)", sub);
        }
    }
};

class MotorCalCommand : public CommandInterface {
public:
    MotorCalCommand()
      : CommandInterface("motorcal", "Automatic motor profiling (poles, encoder cycles, offset, resistance)",
            ArgSpec{"subcmd", "", 0.0f, 0.0f, 0.0f, true, ArgSpec::STRING}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        AutoCalibrationCoordinator& coord = autoCalibrationCoordinator();
        const char* sub = args[0].s_val;

        if (stringsEqual(sub, "start")) {
            if (openLoopController().isRunning()) {
                Telemetry::printf("[CAL] stop the motor before starting motorcal");
                return;
            }
            if (!coord.start()) {
                Telemetry::printf("[CAL] motorcal start failed");
            }
        } else if (stringsEqual(sub, "stop")) {
            coord.stop();
        } else if (stringsEqual(sub, "status")) {
            Telemetry::printf("[CAL] motorcal: state=%s poles=%.2f enc_cycles=%.2f offset=%.3f R_avg=%.4f",
                              coord.stateName(),
                              static_cast<double>(coord.lastPoles()),
                              static_cast<double>(coord.lastEncoderCyclesPerRev()),
                              static_cast<double>(coord.lastEncoderOffset()),
                              static_cast<double>(coord.lastResistanceAverage()));
        } else {
            Telemetry::printf("[CAL] motorcal: unknown subcommand '%s' (start/status)", sub);
        }
    }
};

class FluxCalCommand : public CommandInterface {
public:
    FluxCalCommand()
      : CommandInterface("fluxcal", "PM flux linkage calibration (back-EMF speed sweep)",
            {ArgSpec{"subcmd", "", 0.0f, 0.0f, 0.0f, true, ArgSpec::STRING},
             ArgSpec{"max_iq", "", 0.0f, 200.0f, 0.0f, false, ArgSpec::FLOAT}}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        FluxLinkageCalibrator& fc = fluxLinkageCalibrator();
        const char* sub = args[0].s_val;

        if (stringsEqual(sub, "stop")) {
            fc.stop();
            return;
        }

        if (stringsEqual(sub, "status")) {
            Telemetry::printf("[CAL] fluxcal: state=%s psi_m=%.5f Wb points=%d",
                              fc.stateName(), static_cast<double>(fc.lastFlux()),
                              fc.pointCount());
            for (int i = 0; i < fc.pointCount(); ++i) {
                if (fc.fluxPoint(i) > 0.0f) {
                    Telemetry::printf("[CAL] fluxcal:   iq=%5.1f A rpm=%6.0f psi_m=%.5f Wb",
                                      static_cast<double>(fc.iqPoint(i)),
                                      static_cast<double>(fc.rpmPoint(i)),
                                      static_cast<double>(fc.fluxPoint(i)));
                }
            }
            return;
        }

        if (stringsEqual(sub, "start")) {
            const float maxIq = args[1].present ? args[1].f_val : 20.0f;
            if (!fc.start(maxIq)) {
                Telemetry::printf("[CAL] fluxcal start failed");
            }
            return;
        }

        Telemetry::printf("[CAL] fluxcal: unknown subcommand '%s' (start/stop/status)", sub);
    }
};

static PolesCommand      sPolesCmd;
static EncoderCalCommand sEncoderCalCmd;
static CalPolesCommand   sCalPolesCmd;
static EncOffsetCommand  sEncOffsetCmd;
/**
 * @brief `cal <path>` — hierarchical calibration dispatcher.
 *
 *   cal list                     Show the routine tree and stored values.
 *   cal all                      Full profile, dependency order.
 *   cal stop / cal status        Abort / show progress.
 *   cal Motor.Poles              Pole count (+ encoder cycle count).
 *   cal Motor.Encoder[.SinCos]   Encoder offset + sign.
 *   cal Motor.Resistance [I_A]   Phase resistances. Optional target current;
 *                                default uses the firmware default. Append
 *                                --force to skip the inactive-phase check.
 *   cal Motor.PMSM               Inductance then flux linkage.
 *   cal Motor.PMSM.Inductance [I_A]  Ld/Lq only. Optional max bias current;
 *                                    default uses firmware default.
 *   cal Motor.PMSM.FluxLinkage   PM flux linkage only.
 *   cal Motor.Induction [I_A]    Induction-machine sigma_Ls / tau_r. Optional
 *                                flux current; default uses firmware default.
 *   cal Motor.Induction.VHz [f]  Encoderless V/Hz spin Ls sweep. Optional
 *                                electrical frequency in Hz (default 5).
 *                                Append --wye if the motor is not delta.
 *
 * Results are stored in the RTE KV store under the same Motor.* paths and
 * picked up by graph config nodes at boot.
 */
class CalCommand : public CommandInterface {
public:
    CalCommand()
      : CommandInterface("cal", "Calibration: cal list/all/stop/status/<path> [target_A] [--no-save] [--force]",
            {ArgSpec{"path", "", 0.0f, 0.0f, 0.0f, true, ArgSpec::STRING},
             ArgSpec{"flags", "", 0.0f, 0.0f, 0.0f, false, ArgSpec::STRING},
             ArgSpec{"target_a", "", 0.0f, 200.0f, 0.0f, false, ArgSpec::FLOAT}}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        const char* path = args[0].s_val;
        const char* flags = args[1].present ? args[1].s_val : "";
        const bool save_results = std::strstr(flags, "--no-save") == nullptr;
        const bool force_mode = std::strstr(flags, "--force") != nullptr;
        using S = AutoCalibrationCoordinator::State;

        if (stringsEqual(path, "list")) {
            printList();
        } else if (stringsEqual(path, "status")) {
            printStatus();
        } else if (stringsEqual(path, "stop")) {
            autoCalibrationCoordinator().stop();
            Telemetry::printf("[CAL] stop requested");
        } else if (stringsEqual(path, "all")) {
            startRun(S::POLE, S::FLUX, path, save_results);
        } else if (stringsEqual(path, "Motor.Poles")) {
            startRun(S::POLE, S::POLE, path, save_results);
        } else if (stringsEqual(path, "Motor.Encoder.SinCos.Breakaway") ||
                   stringsEqual(path, "Motor.Encoder.Breakaway")) {
            if (openLoopController().isRunning()) {
                Telemetry::printf("[CAL] stop the motor before starting %s", path);
                return;
            }
            if (breakawayCalibrator().isActive()) {
                Telemetry::printf("[CAL] %s: already running", path);
                return;
            }
            if (breakawayCalibrator().start()) {
                Telemetry::printf("[CAL] %s: started", path);
            } else {
                Telemetry::printf("[CAL] %s: failed to start", path);
            }
        } else if (stringsEqual(path, "Motor.Encoder") ||
                   stringsEqual(path, "Motor.Encoder.SinCos")) {
            startRun(S::OFFSET, S::OFFSET, path, save_results);
        } else if (stringsEqual(path, "Motor.Encoder.Linearity")) {
            if (openLoopController().isRunning()) {
                Telemetry::printf("[CAL] stop the motor before starting %s", path);
                return;
            }
            if (encoderLinearityCalibrator().isActive()) {
                Telemetry::printf("[CAL] %s: already running", path);
                return;
            }
            float poles = 0.0f, cycles = 0.0f;
            if (!Inverter::RteParamStore::isReady() ||
                !Inverter::RteParamStore::get("Motor.Poles", &poles) ||
                !Inverter::RteParamStore::get("Motor.Encoder.SinCos.CyclesRev", &cycles)) {
                Telemetry::printf("[CAL] %s: need Motor.Poles and Motor.Encoder.SinCos.CyclesRev", path);
                return;
            }
            encoderLinearityCalibrator().start(poles, cycles);
        } else if (stringsEqual(path, "Motor.Resistance")) {
            resistanceCalibrator().setForceMode(force_mode);
            if (args[2].present) {
                autoCalibrationCoordinator().setResistanceTargetCurrent(args[2].f_val);
            }
            startRun(S::SETTLE, S::RESISTANCE, path, save_results);
        } else if (stringsEqual(path, "Motor.PMSM")) {
            startRun(S::INDUCTANCE, S::FLUX, path, save_results);
        } else if (stringsEqual(path, "Motor.PMSM.Inductance")) {
            if (args[2].present) {
                autoCalibrationCoordinator().setInductanceParams(args[2].f_val, 0.0f, 0.0f);
            }
            startRun(S::INDUCTANCE, S::INDUCTANCE, path, save_results);
        } else if (stringsEqual(path, "Motor.PMSM.FluxLinkage")) {
            startRun(S::FLUX, S::FLUX, path, save_results);
        } else if (stringsEqual(path, "Motor.Induction")) {
            /* Ensure the coordinator branches to the induction parameter-identification
             * routine rather than the PMSM Ld/Lq + flux-linkage path. */
            if (motorCalibration().motor_type != MotorType::Induction) {
                motorCalibration().motor_type = MotorType::Induction;
                Telemetry::printf("[CAL] motor type set to induction for this run");
            }
            if (args[2].present) {
                autoCalibrationCoordinator().setInductionParams(args[2].f_val, 0.0f, 0.0f, 0.0f);
            }
            startRun(S::SETTLE, S::INDUCTION_PARAMS, path, save_results);
        } else if (stringsEqual(path, "Motor.Induction.VHz")) {
            if (motorCalibration().motor_type != MotorType::Induction) {
                motorCalibration().motor_type = MotorType::Induction;
                Telemetry::printf("[CAL] motor type set to induction for this run");
            }
            /* Accept either `cal ...VHz 20` or `cal ...VHz --wye 20`.
             * args[1] is the string slot, so a bare number lands there. */
            float freq_hz = 5.0f;
            bool wye = false;

            if (args[1].present) {
                if (std::strstr(args[1].s_val, "--wye") != nullptr) {
                    wye = true;
                } else {
                    /* atof is safer than sscanf in newlib-nano. */
                    const float parsed = static_cast<float>(std::atof(args[1].s_val));
                    if (parsed > 0.0f) {
                        freq_hz = parsed;
                    }
                }
            }
            if (args[2].present) {
                freq_hz = args[2].f_val;
                if (std::strstr(args[2].s_val, "--wye") != nullptr) {
                    wye = true;
                }
            }
            wye = wye || (std::strstr(flags, "--wye") != nullptr);
            if (inductionVHzCalibrator().start(freq_hz, 0.35f, 10, 1000U, 2000U,
                                               80.0f, !wye)) {
                Telemetry::printf("[CAL] %s: started f=%.1f Hz (%s)", path,
                                  static_cast<double>(freq_hz),
                                  wye ? "wye" : "delta");
            } else {
                Telemetry::printf("[CAL] %s: failed to start", path);
            }
        } else {
            Telemetry::printf("[CAL] unknown routine '%s' (try 'cal list')", path);
        }
    }

private:
    void startRun(AutoCalibrationCoordinator::State first,
                  AutoCalibrationCoordinator::State last,
                  const char* name,
                  bool save_results = true) const {
        Inverter::CalKvStore::ensureBaseInfo();
        if (autoCalibrationCoordinator().startSlice(first, last, save_results)) {
            if (save_results) {
                Telemetry::printf("[CAL] %s: started", name);
            } else {
                Telemetry::printf("[CAL] %s: started (--no-save)", name);
            }
        }
    }

    static void printStored(const char* key, const char* label) {
        float v = 0.0f;
        if (Inverter::RteParamStore::isReady() &&
            Inverter::RteParamStore::get(key, &v)) {
            Telemetry::printf("[CAL]   %-28s = %.5f   (%s)", key,
                              static_cast<double>(v), label);
        } else {
            Telemetry::printf("[CAL]   %-28s   unset   (%s)", key, label);
        }
    }

    void printList() const {
        Telemetry::printf("[CAL] routines (cal <path> to run):");
        Telemetry::printf("[CAL]   all                         full profile, in order");
        Telemetry::printf("[CAL]   Motor.Poles");
        Telemetry::printf("[CAL]   Motor.Encoder.SinCos.Breakaway   find breakaway voltage only");
        printStored("Motor.Poles", "rotor pole count");
        printStored("Motor.Encoder.SinCos.CyclesRev", "encoder elec cycles/mech rev");
        printStored("Motor.Encoder.SinCos.BreakMod", "breakaway modulation");
        Telemetry::printf("[CAL]   Motor.Encoder.SinCos");
        printStored("Motor.Encoder.SinCos.OffsetRad", "encoder offset, elec rad");
        printStored("Motor.Encoder.SinCos.Sign", "encoder direction (+1/-1)");
        Telemetry::printf("[CAL]   Motor.Encoder.Linearity");
        Telemetry::printf("[CAL]     run 'cal Motor.Encoder.Linearity' for INL harmonic report");
        printStored("Motor.Encoder.SinCos.Fit.Valid", "ellipse fit valid (1=yes)");
        printStored("Motor.Encoder.SinCos.Fit.Cs", "fit sin center, counts");
        printStored("Motor.Encoder.SinCos.Fit.Cc", "fit cos center, counts");
        printStored("Motor.Encoder.SinCos.Fit.As", "fit sin amplitude, counts");
        printStored("Motor.Encoder.SinCos.Fit.Ac", "fit cos amplitude, counts");
        printStored("Motor.Encoder.SinCos.Fit.Phi", "fit quadrature phase, rad");
        Telemetry::printf("[CAL]   Motor.Resistance");
        printStored("Motor.Resistance.Uv", "phase resistance, ohm");
        printStored("Motor.Resistance.Uw", "phase resistance, ohm");
        printStored("Motor.Resistance.Vw", "phase resistance, ohm");
        printStored("Motor.Resistance.Avg", "average, ohm");
        Telemetry::printf("[CAL]   Motor.PMSM[.Inductance|.FluxLinkage]");
        printStored("Motor.PMSM.Inductance.Ld", "d-axis inductance, H");
        printStored("Motor.PMSM.Inductance.Lq", "q-axis inductance, H");
        printStored("Motor.PMSM.FluxLinkage.Wb", "PM flux linkage, Wb");
        Telemetry::printf("[CAL]   Motor.Induction");
        Telemetry::printf("[CAL]   Motor.Induction.VHz [freq_hz]  V/Hz spin Ls sweep (encoderless)");
        printStored("Motor.Type", "1=PMSM 3=induction");
        printStored("Motor.Induction.SigmaLs", "stator transient inductance, H");
        printStored("Motor.Induction.TauR", "rotor time constant, ms");
        printStored("Motor.Induction.Lm", "magnetizing inductance, H");
        printStored("Motor.Induction.Lr", "rotor inductance, H");
        printStored("Motor.Induction.Rr", "rotor resistance, ohm");
        printStored("Motor.Induction.LLeak", "leakage inductance, H");
    }

    void printStatus() const {
        AutoCalibrationCoordinator& coord = autoCalibrationCoordinator();
        Telemetry::printf("[CAL] state=%s type=%s poles=%.2f enc_cycles=%.2f offset=%.3fdeg sign=%.0f R=%.4f",
                          coord.stateName(),
                          Inverter::MotorConfigStore::typeName(motorCalibration().motor_type),
                          static_cast<double>(coord.lastPoles()),
                          static_cast<double>(coord.lastEncoderCyclesPerRev()),
                          static_cast<double>(coord.lastEncoderOffset()),
                          static_cast<double>(coord.lastResistanceAverage()));
    }
};

class EncFitCommand : public CommandInterface {
public:
    EncFitCommand()
      : CommandInterface("encfit", "Show/clear the active sin/cos ellipse fit",
            ArgSpec{"subcmd", "", 0.0f, 0.0f, 0.0f, true, ArgSpec::STRING}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        const char* sub = args[0].s_val;

        if (stringsEqual(sub, "clear")) {
            encoderADC().clearFit();
            Telemetry::printf("[CAL] encfit: cleared");
            return;
        }

        if (!stringsEqual(sub, "status") && sub[0] != '\0') {
            Telemetry::printf("[CAL] encfit: unknown subcommand '%s' (status/clear)", sub);
            return;
        }

        const EncoderADC::SinCosFit fit = encoderADC().currentFit();
        if (!fit.valid) {
            Telemetry::printf("[CAL] encfit: no active fit");
            return;
        }

        const float phi_deg = fit.phase_err * 57.2957795131f;
        const float amp_mismatch_pct =
            100.0f * std::fabs(fit.amp_sin - fit.amp_cos) /
            (0.5f * (fit.amp_sin + fit.amp_cos));
        Telemetry::printf("[CAL] encfit: Cs=%.1f Cc=%.1f As=%.1f Ac=%.1f phi=%.4f deg",
                          static_cast<double>(fit.center_sin),
                          static_cast<double>(fit.center_cos),
                          static_cast<double>(fit.amp_sin),
                          static_cast<double>(fit.amp_cos),
                          static_cast<double>(phi_deg));
        Telemetry::printf("[CAL] encfit: samples=%lu gain_mismatch=%.2f%%",
                          static_cast<unsigned long>(fit.sample_count),
                          static_cast<double>(amp_mismatch_pct));
    }
};

class EncLinCommand : public CommandInterface {
public:
    EncLinCommand()
      : CommandInterface("encinl", "Encoder linearity measurement",
            {ArgSpec{"subcmd", "", 0.0f, 0.0f, 0.0f, true, ArgSpec::STRING},
             ArgSpec{"poles", "", 2.0f, 100.0f, 10.0f, false, ArgSpec::FLOAT},
             ArgSpec{"enc_cycles", "", 0.1f, 100.0f, 1.0f, false, ArgSpec::FLOAT},
             ArgSpec{"rotate_mod", "", 0.0f, 1.0f, 0.0f, false, ArgSpec::FLOAT},
             ArgSpec{"revs", "", 0.5f, 20.0f, 3.0f, false, ArgSpec::FLOAT},
             ArgSpec{"step_size_deg", "", 0.1f, 10.0f, 0.0f, false, ArgSpec::FLOAT},
             ArgSpec{"dual_pass", "", 0.0f, 1.0f, 1.0f, false, ArgSpec::FLOAT}}) {}

    void execute(const ArgValue* args, CommandContext&) override {
        EncoderLinearityCalibrator& cal = encoderLinearityCalibrator();
        const char* sub = args[0].s_val;

        if (stringsEqual(sub, "stop")) {
            cal.stop();
            return;
        }

        if (stringsEqual(sub, "status")) {
            Telemetry::printf("[CAL] encinl: state=%s bins=%d paired=%d lag_rms=%.4f rms=%.4f pp=%.4f",
                              cal.stateName(),
                              cal.validBinCount(),
                              cal.pairedBinCount(),
                              static_cast<double>(cal.lagRmsDeg()),
                              static_cast<double>(cal.residualRmsDeg()),
                              static_cast<double>(cal.residualPpDeg()));
            for (int k = 1; k <= 8; ++k) {
                Telemetry::printf("[CAL] encinl:   H%02d=%.4f deg",
                                  k,
                                  static_cast<double>(cal.harmonicAmplitude(k)));
            }
            return;
        }

        if (stringsEqual(sub, "start")) {
            if (openLoopController().isRunning()) {
                Telemetry::printf("[CAL] encinl: stop the motor first");
                return;
            }
            if (cal.isActive()) {
                Telemetry::printf("[CAL] encinl: already running");
                return;
            }
            if (!args[1].present || !args[2].present) {
                Telemetry::printf("[CAL] encinl start <poles> <enc_cycles> [rotate_mod] [revs] [step_size_deg] [dual_pass]");
                return;
            }
            cal.start(args[1].f_val, args[2].f_val, args[3].f_val, args[4].f_val,
                      args[5].f_val, !args[6].present || (args[6].f_val > 0.5f));
            return;
        }

        Telemetry::printf("[CAL] encinl: unknown subcommand '%s' (start/stop/status)", sub);
    }
};

static ResCalCommand     sResCalCmd;
static FluxCalCommand    sFluxCalCmd;
static MotorCalCommand   sMotorCalCmd;
static EncFitCommand     sEncFitCmd;
/* EncLinCommand is the largest of the calibration commands (~208 B).  Keep it
 * in AXISRAM so the growing command set does not consume scarce DTCM. */
static EncLinCommand     sEncLinCmd __attribute__((section(".dma_buffers")));
static CalCommand        sCalCmd;

void registerCalibrationCommands(CommandManager& mgr) {
    /* Only the hierarchical `cal` command is registered; the legacy flat
     * commands above stay available for debug builds but are not exposed. */
    mgr.registerCommand(&sCalCmd);
    mgr.registerCommand(&sEncFitCmd);
    mgr.registerCommand(&sEncLinCmd);
}
