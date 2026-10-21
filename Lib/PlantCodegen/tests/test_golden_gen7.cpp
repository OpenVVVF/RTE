// PlantCodegen golden test: drives the *generated* gen7 plant engine
// (plant_golden_gen7, compiled at build time from the emitted
// plant_generated.cpp/h) against a checkpoint tape recorded from the python
// reference evaluator (twin/plantgraph/plantgraph.py, float64).
//
// Scenario: dyno 3311.6 rpm, substep dt = 200e-6 (2 substeps per 400 us
// control period), duty vector (52.0, 50.5, 49.0) — knee phase volts
// (du-d_avg)/50*vdc = (4.341, 0, -4.341) V, i.e. the rotor-frame vector
// (vd, vq) = (4.341, 2.507) V. The drive recomputes duties once per period
// rotor-locked (mid-period theta), exactly the protocol the validated
// python proof twin/plantgraph/proof_bench_parity.py runs for the composed
// graph itself.
//
// NOTE on the tape: a *constant* stator-duty open drive at 3311.6 rpm is an
// exponentially diverging trajectory in the python reference itself (the
// substep-Euler machine integration at omega_e = 1734 rad/s and dt = 200 us
// is anti-damped; see the proof's staircase note), and so is the rotor-locked
// equivalent run for 200 periods — both engines must then still track each
// other. The checkpoints below assert exactly that cross-engine parity:
// C++ float32 emulation stays within the float64 tape's divergence budget.

#include <cmath>
#include <cstring>
#include <gtest/gtest.h>

#include "plant_generated.h"

namespace {

constexpr double kDt = 200e-6;      // plant substep
constexpr double kPeriod = 400e-6;  // control period = 2 substeps
constexpr double kVdcRef = 144.7;   // duty-map reference volts
constexpr double kVd = 4.341;
constexpr double kVq = 2.507;
constexpr double kRpm = 3311.6;
constexpr double kOmegaE = kRpm * (2.0 * M_PI / 60.0) * 5.0;  // Poles = 5

struct GoldenDrive {
    plant::PlantState state{};
    double theta = 0.0;

    void StepPeriod() {
        const double th = theta + kOmegaE * kPeriod / 2.0;
        const double ct = std::cos(th), st = std::sin(th);
        const double va = ct * kVd - st * kVq;
        const double vb = st * kVd + ct * kVq;
        const double vv = -0.5 * va + 0.8660254037844386 * vb;
        const double vw = -0.5 * va - 0.8660254037844386 * vb;
        plant::PlantSetDuty(state,
                            static_cast<float>(50.0 + 50.0 * va / kVdcRef),
                            static_cast<float>(50.0 + 50.0 * vv / kVdcRef),
                            static_cast<float>(50.0 + 50.0 * vw / kVdcRef));
        plant::PlantStep(state, kDt);
        plant::PlantStep(state, kDt);
        theta = std::fmod(theta + kOmegaE * kPeriod, 2.0 * M_PI);
    }
};

// Checkpoint tape from the python reference (PlantGraphSim on
// RTE/Assets/PlantExamples/gen7_bench_pmsm.json, dt_sub = 200e-6, identical
// rotor-locked drive), recorded 2026-10-20. Tolerances = measured C++-float32
// divergence against the tape, times ~3 headroom.
struct Checkpoint {
    int period;
    double id, iq, vdc, iu, vdcMeas, dclinkP;
    int encSin, encCos;
    double tolCurrent, tolVolt, tolPower;
};

constexpr Checkpoint kTape[] = {
    //   Tolerances: id/iq tight early (bit-comparable regime), widened late
    //   where the ~+200/s anti-damped mode amplifies float32 rounding.
    {10,   197.103716, -128.895915, 144.998801,  234.85,  145.20,     2284.5104,
     32966, 53724, 0.05, 0.01, 2.0},
    {25,  -793.269246,  639.506296, 139.492656,  590.17,  139.48,  -126588.4477,
     48435, 46691, 0.05, 0.01, 2.0},
    {50, -1810.766227, -1355.270941, 212.555583, 1200.00,  212.52,   434718.4366,
     43504, 50741, 0.05, 0.01, 2.0},
    {100, -2000.000000, -439.336114, 300.841511, -1200.0,  300.96,   142078.2902,
     31228, 53679, 0.10, 0.01, 10.0},
    {200,   201.343297, 1012.329320, 341.140439, -294.70,  341.00,   -41823.1034,
     32258, 53723, 0.50, 0.01, 50.0},
};

TEST(PlantCodegenGolden, Gen7TracksPythonReferenceTape) {
    GoldenDrive drive;
    plant::PlantInit(drive.state);
    plant::PlantSetDynoRpm(drive.state, static_cast<float>(kRpm));

    plant::PlantMeas meas{};
    std::size_t next = 0;
    for (int period = 1; period <= 200; ++period) {
        drive.StepPeriod();
        plant::PlantCollectMeas(drive.state, meas);

        // Rails valid at every period: healthy encoder mid+amp bounds, the
        // physical bus rail, and the machine current rails.
        EXPECT_GE(meas.Enc_Sin, 11000.0f) << "period " << period;
        EXPECT_LE(meas.Enc_Sin, 54000.0f) << "period " << period;
        EXPECT_GE(meas.Vdc_V, 0.5f) << "period " << period;
        EXPECT_LE(std::fabs(drive.state.Machine.Id), 2000.0f) << "period " << period;
        EXPECT_FLOAT_EQ(meas.Mech_Rpm, static_cast<float>(kRpm));

        if (next < std::size(kTape) && kTape[next].period == period) {
            const Checkpoint& cp = kTape[next++];
            EXPECT_NEAR(drive.state.Machine.Id, cp.id, cp.tolCurrent)
                << "id at period " << period;
            EXPECT_NEAR(drive.state.Machine.Iq, cp.iq, cp.tolCurrent)
                << "iq at period " << period;
            EXPECT_NEAR(drive.state.DcBus.Vdc, cp.vdc, cp.tolVolt)
                << "vdc at period " << period;
            EXPECT_NEAR(meas.Iu_A, cp.iu, 0.2) << "iu at period " << period;
            EXPECT_NEAR(meas.Vdc_V, cp.vdcMeas, 0.5)
                << "measured vdc at period " << period;
            EXPECT_NEAR(meas.Dclink_P_W, cp.dclinkP, cp.tolPower)
                << "dclink power at period " << period;
            // Encoder counts: int()-truncated identically in all engines;
            // allow a single quantization-count boundary flip.
            EXPECT_NEAR(meas.Enc_Sin, cp.encSin, 1.0) << "enc_sin at " << period;
            EXPECT_NEAR(meas.Enc_Cos, cp.encCos, 1.0) << "enc_cos at " << period;
        }
    }
    EXPECT_EQ(next, std::size(kTape));

    // The MeasOut seam in v1.1: the three phase-ADC channels are wired
    // (Knee raw taps through the 1.5 ms lag senses); only the two genuinely
    // unpopulated NTC channels read quiet NaN.
    EXPECT_FALSE(std::isnan(meas.Vu_V));
    EXPECT_FALSE(std::isnan(meas.Vv_V));
    EXPECT_FALSE(std::isnan(meas.Vw_V));
    EXPECT_TRUE(std::isnan(meas.Temp_Inv3_C));
    EXPECT_TRUE(std::isnan(meas.Temp_Motor_C));
}

TEST(PlantCodegenGolden, HostExternals) {
    plant::PlantState state{};
    plant::PlantInit(state);

    // DutyIn zero-vector default, then host override.
    EXPECT_FLOAT_EQ(state.DutyIn.Du, 50.0f);
    plant::PlantSetDuty(state, 52.0f, 50.5f, 49.0f);
    EXPECT_FLOAT_EQ(state.DutyIn.Du, 52.0f);
    EXPECT_FLOAT_EQ(state.DutyIn.Dv, 50.5f);
    EXPECT_FLOAT_EQ(state.DutyIn.Dw, 49.0f);

    // Dyno host publish: rpm -> electrical views.
    plant::PlantSetDynoRpm(state, 3311.6f);
    EXPECT_FLOAT_EQ(state.Dyno.Rpm, 3311.6f);
    EXPECT_NEAR(state.Dyno.Omega_Elec,
                static_cast<float>(3311.6 * (2.0 * M_PI / 60.0) * 5.0), 1e-2f);
    EXPECT_FLOAT_EQ(state.Dyno.Elec_Rpm, 3311.6f * 5.0f);

    // Fault point by name; D8: the return says whether the name exists.
    EXPECT_TRUE(plant::PlantSetFault(state, "OpenGates", true));
    EXPECT_FLOAT_EQ(state.Fault.OpenGates, 1.0f);
    EXPECT_FALSE(plant::PlantSetFault(state, "NotARealFault", true));
    EXPECT_FLOAT_EQ(state.Fault.EncoderDisconnect, 0.0f);
    EXPECT_TRUE(plant::PlantSetFault(state, "OpenGates", false));
    EXPECT_FLOAT_EQ(state.Fault.OpenGates, 0.0f);

    // PlantSetInput: unwired ports of the graph match by name; wired/absent
    // names return false. Draw.C is the gen7 unwired input.
    EXPECT_TRUE(plant::PlantSetInput(state, "C", 7.0f));
    EXPECT_FLOAT_EQ(state.Draw.C, 7.0f);
    EXPECT_FALSE(plant::PlantSetInput(state, "Du", 1.0f));         // wired
    EXPECT_FALSE(plant::PlantSetInput(state, "Omega_Cmd", 1.0f));  // absent here
}

// D11: after PlantInit the measurement view must already reflect t0 — the
// python reference's _initial_eval pass publishes every comb/stateful output
// before the first substep (encoder counts, thermal reads, idle heat...).
// Tape recorded from twin/plantgraph/plantgraph.py (PlantGraphSim on
// gen7_bench_pmsm, before any step), 2026-10-21.
TEST(PlantCodegenGolden, T0PublishMatchesPythonInitialEval) {
    plant::PlantState state{};
    plant::PlantInit(state);
    plant::PlantMeas meas{};
    plant::PlantCollectMeas(state, meas);

    EXPECT_FLOAT_EQ(meas.Iu_A, 0.0f);
    EXPECT_FLOAT_EQ(meas.Iv_A, 0.0f);
    EXPECT_FLOAT_EQ(meas.Iw_A, 0.0f);
    // Unwired channels are NaN; wired phase-ADC taps read the 50/50/50
    // zero-vector raw volts through un-sampled lags (python: 0.0).
    EXPECT_FLOAT_EQ(meas.Vu_V, 0.0f);
    EXPECT_FLOAT_EQ(meas.Vv_V, 0.0f);
    EXPECT_FLOAT_EQ(meas.Vw_V, 0.0f);
    EXPECT_NEAR(meas.Enc_Sin, 30201.0f, 1.0f);  // int()-truncation band
    EXPECT_NEAR(meas.Enc_Cos, 53586.0f, 1.0f);
    EXPECT_NEAR(meas.Temp_Inv1_C, 22.8f, 1e-4f);
    EXPECT_NEAR(meas.Temp_Inv2_C, 25.9f, 1e-4f);  // Off2Idle law at idle
    EXPECT_TRUE(std::isnan(meas.Temp_Inv3_C));
    EXPECT_TRUE(std::isnan(meas.Temp_Motor_C));
    EXPECT_FLOAT_EQ(meas.Mech_Rpm, 0.0f);
    EXPECT_FLOAT_EQ(meas.Elec_Rpm, 0.0f);
    EXPECT_FLOAT_EQ(meas.Dclink_I_A, 0.0f);
    EXPECT_FLOAT_EQ(meas.Dclink_P_W, 0.0f);
    EXPECT_FLOAT_EQ(meas.P_Heat_W, 120.0f);  // idle heat floor (IdleHeat_W)
    EXPECT_FLOAT_EQ(meas.P_Dc_W, 0.0f);
    EXPECT_FLOAT_EQ(meas.P_ShaftEm_W, 0.0f);
    EXPECT_FLOAT_EQ(state.DcBus.Vdc, 144.7f);  // V0_V, still bus-rest
    EXPECT_FLOAT_EQ(state.Loss.FloorHits, 0.0f);
    // NOTE on Vdc_V (D11 divergence, documented): the C++ bus channel fuses
    // sampling and integration in one body, so the t=0 publish pass already
    // samples the 144.7 V rest bus through the lag-free path; python's split
    // emit() publishes 0.0 there. The quantized 144.76 V assertion below
    // pins the C++ behavior; every other channel matches python verbatim.
    EXPECT_NEAR(meas.Vdc_V, 144.76f, 0.01f);

    // Re-init restores the exact t0 bit pattern (all persistent state in the
    // struct; PlantInit is idempotent under D11's publish pass).
    plant::PlantState fresh{};
    plant::PlantInit(fresh);
    state.DcBus.Vdc = 999.0f;
    plant::PlantInit(state);
    EXPECT_EQ(std::memcmp(&state, &fresh, sizeof(state)), 0);
}

// D9/D10: graph-supplied supply defaults and boot params drive the partial
// override composition the sim host performs (--supply partial fill).
TEST(PlantCodegenGolden, SupplyDefaultsAndBootParams) {
    plant::PlantState state{};
    plant::PlantInit(state);

    // D9: defaults accessor hands back the graph instance supply values.
    double vSet = 0, iLimit = 0, sinkLimit = 0, cvClamp = -1, cvClampSink = -1,
           seriesR = 0;
    bool canSink = false;
    plant::PlantSupplyDefaults(state, &vSet, &iLimit, &sinkLimit, &canSink,
                               &cvClamp, &cvClampSink, &seriesR);
    EXPECT_DOUBLE_EQ(vSet, 144.7);
    EXPECT_DOUBLE_EQ(iLimit, 400.0);
    EXPECT_DOUBLE_EQ(sinkLimit, 50.0);
    EXPECT_TRUE(canSink);
    EXPECT_DOUBLE_EQ(cvClamp, 0.0);
    EXPECT_DOUBLE_EQ(cvClampSink, 0.0);
    EXPECT_DOUBLE_EQ(seriesR, 5.0);
    // Null pointers are skipped without touching anything.
    plant::PlantSupplyDefaults(state, nullptr, &iLimit, nullptr, nullptr,
                               nullptr, nullptr, nullptr);
    EXPECT_DOUBLE_EQ(iLimit, 400.0);

    // D10: boot constants from the graph's machine/encoder params.
    double poles = 0, rsOhm = 0, encOffset = 0;
    plant::PlantBootParams(state, &poles, &rsOhm, &encOffset);
    EXPECT_DOUBLE_EQ(poles, 5.0);
    EXPECT_DOUBLE_EQ(rsOhm, 0.00765);
    EXPECT_DOUBLE_EQ(encOffset, 33.034);

    // Partial-override composition: repoint just the setpoint and clamp via
    // defaults-read -> modify -> full 7-arg set; the untouched knobs keep
    // their graph values.
    plant::PlantSupplyDefaults(state, &vSet, &iLimit, &sinkLimit, &canSink,
                               &cvClamp, &cvClampSink, &seriesR);
    vSet = 120.0;
    plant::PlantSetSupply(state, vSet, iLimit, sinkLimit, canSink, cvClamp,
                          cvClampSink, seriesR);
    EXPECT_DOUBLE_EQ(state.DcBus.VSet_V, 120.0);
    EXPECT_DOUBLE_EQ(state.DcBus.ILimit_A, 400.0);
    EXPECT_DOUBLE_EQ(state.DcBus.SeriesR_mOhm, 5.0);

    GoldenDrive drive;
    plant::PlantInit(drive.state);
    plant::PlantSetDynoRpm(drive.state, 0.0f);
    plant::PlantSetSupply(drive.state, 120.0, 400.0, 50.0, true, 0.0, 0.0, 5.0);
    for (int period = 0; period < 100; ++period) {
        drive.StepPeriod();
    }
    EXPECT_LT(drive.state.DcBus.Vdc, 121.0f);
    EXPECT_GT(drive.state.DcBus.Vdc, 119.0f);
}

TEST(PlantCodegenGolden, OpenGatesCollapseMatchesTimeConstant) {
    // After a gate kill the machine currents decay with tau = 2 ms
    // (six-switch-open freewheel collapse; the zero-divergence property the
    // python parity checks assert exactly).
    GoldenDrive drive;
    plant::PlantInit(drive.state);
    plant::PlantSetDynoRpm(drive.state, static_cast<float>(kRpm));
    for (int period = 0; period < 25; ++period) drive.StepPeriod();

    const float idBefore = std::fabs(drive.state.Machine.Id);
    ASSERT_GT(idBefore, 100.0f);
    EXPECT_TRUE(plant::PlantSetFault(drive.state, "OpenGates", true));
    for (int period = 0; period < 25; ++period) {  // 10 ms = 5 tau
        drive.StepPeriod();
    }
    EXPECT_LT(std::fabs(drive.state.Machine.Id), 0.05f * idBefore);
}

}  // namespace
