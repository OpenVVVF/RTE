// PlantCodegen golden tests for the DC/DC converter family: drives the
// *generated* plants (plant_golden_buckboost / plant_golden_dab, emitted at
// build time from buckboost_charger_example.json / dab_pack_example.json)
// against checkpoint tapes recorded from the python reference evaluator
// (twin/plantgraph/plantgraph.py, float64), mirroring the protocols of
// twin/plantgraph/proof_machines_parity.py (_run_rig_graph /
// _dab_law_pins):
//
// - converter rigs have no Machine/DutyIn/Dyno: duty and bridge phase shift
//   are pushed through the unwired-input seam (PlantSetInput, python
//   set_input), checkpoints read converter/bus state fields directly;
// - the DAB transfer law itself is pinned engine-isolated: bus voltage
//   fields state-fed, one 200 us substep per case (the proof's pin run);
// - the buck/boost trajectory pins the buck rail settle, the boost
//   direction, and the OpenGates freewheel collapse (family 2 ms tau).
//
// Tape recorded 2026-10-21 from PlantGraphSim on the shipped graphs
// (dt_sub = 200e-6); the proof's own cross-engine numbers for these two
// rigs are 0.047 abs / 6e-4 rel (buck trajectory) and 6.6e-5 worst rel
// (DAB law pins). Every band below carries >= 10x headroom over that.

#include <cmath>
#include <gtest/gtest.h>

#include "buckboost_generated.h"
#include "dab_generated.h"

namespace {

constexpr double kDt = 200e-6;  // plant substep the rigs are proven at
constexpr int kStepsPerHalfSecond = 2500;

namespace buck = plant_buckboost_charger_example;
namespace dab = plant_dab_pack_example;

void RunSteps(buck::PlantState& state, int steps) {
    for (int k = 0; k < steps; ++k) buck::PlantStep(state, kDt);
}

// Drives one isolated DAB law case exactly like the proof's _dab_law_pins:
// both bus voltage fields state-fed (overwriting the bus integrator state),
// phase shift through the unwired input, a single substep, then read back.
void DabCase(dab::PlantState& state, double vh, double vl, double phi) {
    state.HVBus.Vdc = static_cast<float>(vh);
    state.LVPack.Vdc = static_cast<float>(vl);
    dab::PlantSetInput(state, "PhaseShift", static_cast<float>(phi));
    dab::PlantStep(state, kDt);
}

// Trajectory checkpoint tape for buckboost_charger_example: duty pushed
// through the single unwired input "Duty", 200 us substeps.
TEST(PlantCodegenGoldenDcDc, BuckBoostTracksPythonReferenceTape) {
    buck::PlantState state{};
    buck::PlantInit(state);

    // D7: "Duty" is this graph's only unwired input — the assignment must
    // match it (and return true); wired ("Rail" -> Fault.SensorRail) and
    // absent ("PhaseShift") names return false without touching anything.
    EXPECT_TRUE(buck::PlantSetInput(state, "Duty", 55.0f));
    EXPECT_FLOAT_EQ(state.Conv.Duty, 55.0f);
    EXPECT_FALSE(buck::PlantSetInput(state, "Rail", 1.0f));
    EXPECT_FALSE(buck::PlantSetInput(state, "PhaseShift", 0.1f));

    // Segment 1: duty 55 for 0.5 s. python reference (float64, same drive):
    //   Conv.I_L   = 0.728830  (buck rail: the source-blocked stiff pack
    //                           charges until the winding-distortion balance
    //                           starves the inductor)
    //   LVPack.Vdc = 51.603985
    //   HVBus.Vdc  = 95.996787 (bench CV rail, 60 A source class)
    RunSteps(state, kStepsPerHalfSecond);
    EXPECT_GE(state.Conv.I_L, 0.5f);
    EXPECT_LE(state.Conv.I_L, 1.1f);
    EXPECT_GE(state.LVPack.Vdc, 51.0f);
    EXPECT_LE(state.LVPack.Vdc, 52.4f);
    EXPECT_GE(state.HVBus.Vdc, 95.5f);
    EXPECT_LE(state.HVBus.Vdc, 96.5f);
    EXPECT_GT(state.Conv.P_High, 0.0f);  // power leaves the HV bus (buck)

    // Measurement view at the buck rail: wired channels track their truth
    // source inside sensor quantization; channels the rig does not populate
    // read quiet NaN (D12 — no encoder, no shaft, no NTC strip).
    buck::PlantMeas meas{};
    buck::PlantCollectMeas(state, meas);
    EXPECT_NEAR(meas.Iu_A, state.Conv.I_L, 0.08);    // HallL, 0.07 A LSB
    EXPECT_NEAR(meas.Iv_A, state.Conv.I_High, 0.08); // HallH, 0.07 A LSB
    EXPECT_NEAR(meas.Vdc_V, state.HVBus.Vdc, 0.45);  // VSenseH, 0.44 V LSB
    EXPECT_NEAR(meas.Vu_V, state.LVPack.Vdc, 0.13);  // VSenseL, 0.12 V LSB
    EXPECT_FLOAT_EQ(meas.Iw_A, state.Conv.I_Low);    // direct truth tap
    EXPECT_FLOAT_EQ(meas.P_Dc_W, state.Conv.P_High); // direct truth taps
    EXPECT_FLOAT_EQ(meas.P_Heat_W, state.Conv.P_Low);
    EXPECT_TRUE(std::isnan(meas.Enc_Sin));
    EXPECT_TRUE(std::isnan(meas.Enc_Cos));
    EXPECT_TRUE(std::isnan(meas.Vv_V));
    EXPECT_TRUE(std::isnan(meas.Vw_V));
    EXPECT_TRUE(std::isnan(meas.Temp_Inv1_C));
    EXPECT_TRUE(std::isnan(meas.Temp_Motor_C));
    EXPECT_TRUE(std::isnan(meas.Mech_Rpm));
    EXPECT_TRUE(std::isnan(meas.Elec_Rpm));

    // Segment 2: duty 45 for 0.5 s continuing the trajectory (the proof's
    // second drive step). python reference:
    //   Conv.I_L   = -43.599347 (boost direction: the winding-distortion
    //                            balance pins the discharge while the pack
    //                            runs down)
    //   LVPack.Vdc = 47.124270  (still falling toward the ~44.4 V floor)
    //   HVBus.Vdc  = 100.512740 (regen lifts the sink-capable HV bus)
    //   Conv.P_High = -1972.03  (power flows INTO the HV bus)
    EXPECT_TRUE(buck::PlantSetInput(state, "Duty", 45.0f));
    RunSteps(state, kStepsPerHalfSecond);
    EXPECT_GE(state.Conv.I_L, -45.5f);
    EXPECT_LE(state.Conv.I_L, -41.5f);
    EXPECT_LT(state.Conv.P_High, -1500.0f);
    EXPECT_GE(state.HVBus.Vdc, 99.5f);
    EXPECT_LE(state.HVBus.Vdc, 101.5f);

    // Segment 3: same duty-45 window extended to 1.0 s of boost — the
    // ring-down as the pack nears its trickle floor. python reference:
    //   Conv.I_L = -1.5049 at duty-45 +1.0 s (monotonic crossing of the
    //   band from the -5.41 @+0.9 s / -0.06 @+1.25 s tape neighbours;
    //   cross-engine drift here is mA-class, so the crossing point is
    //   robust).
    RunSteps(state, kStepsPerHalfSecond);
    EXPECT_GE(state.Conv.I_L, -3.0f);
    EXPECT_LE(state.Conv.I_L, -0.5f);  // still the boost direction
    EXPECT_GE(state.LVPack.Vdc, 43.5f);  // python: 44.4075 at +1.0 s
    EXPECT_LT(state.LVPack.Vdc, 45.0f);
}

// Freewheel collapse on the family 2 ms evidence tau: gates open mid-run,
// the inductor state dies off exponentially, and the gate-side emulation
// keeps publishing I_High = duty * I_L from the decaying state.
TEST(PlantCodegenGoldenDcDc, BuckBoostOpenGatesCollapseAndGateSideEmulation) {
    buck::PlantState state{};
    buck::PlantInit(state);
    buck::PlantSetInput(state, "Duty", 55.0f);

    // 0.2 s live drive. python reference pre-fault: Conv.I_L = 27.626100.
    RunSteps(state, 1000);
    const float iPre = state.Conv.I_L;
    ASSERT_GE(iPre, 24.0f);
    ASSERT_LE(iPre, 31.0f);

    EXPECT_TRUE(buck::PlantSetFault(state, "OpenGates", true));
    EXPECT_FLOAT_EQ(state.Fault.OpenGates, 1.0f);
    EXPECT_FALSE(buck::PlantSetFault(state, "OpenGatez", true));  // D8

    // The discrete collapse is I_L *= (1 - dt/tau) per substep, tau = 2 ms:
    // exact ratios at dt = 200 us are 0.9^5 = 0.59049 (+1 ms),
    // 0.9^10 = 0.34868 (+2 ms — that is ONE tau, i.e. ~35 % remaining, so
    // a "<20 % inside 2 ms" reading only crosses after ~3.2 ms = 16
    // substeps), 0.9^16 = 0.18530, 0.9^25 = 0.07179 (+5 ms). python
    // reference confirmed identical to 1e-4 rel.
    RunSteps(state, 5);  // +1.0 ms
    float ratio = state.Conv.I_L / iPre;
    EXPECT_GE(ratio, 0.55f);
    EXPECT_LE(ratio, 0.63f);
    // Gate-side emulation: the averaged-switch view keeps publishing
    // I_High = duty * I_L from the decaying state rather than snapping the
    // gate-side channels to zero (contrast the DAB, which zeroes — it has
    // no integrator to emulate from).
    EXPECT_GT(state.Conv.I_High, 0.4f * 0.55f * iPre);
    EXPECT_NEAR(state.Conv.I_High, 0.55f * state.Conv.I_L,
                0.02f * std::fabs(state.Conv.I_L) + 1e-3f);
    EXPECT_NEAR(state.Conv.P_High, state.HVBus.Vdc * state.Conv.I_High,
                1.0f);

    RunSteps(state, 5);  // +2.0 ms total
    ratio = state.Conv.I_L / iPre;
    EXPECT_GE(ratio, 0.30f);
    EXPECT_LE(ratio, 0.40f);  // pins tau = 2 ms (e^-1 = 0.368)
    EXPECT_NEAR(state.Conv.I_High, 0.55f * state.Conv.I_L,
                0.02f * std::fabs(state.Conv.I_L) + 1e-3f);

    RunSteps(state, 6);  // +3.2 ms total: below 20 % remaining
    ratio = state.Conv.I_L / iPre;
    EXPECT_LT(ratio, 0.20f);

    RunSteps(state, 9);  // +5 ms = 2.5 tau
    EXPECT_LT(state.Conv.I_L / iPre, 0.10f);

    // Release the fault: the leg re-latches and the converter settles back
    // onto the buck rail — python reference: Conv.I_L = 0.0704 (the float
    // trickle branch, rising toward the ~0.73 A rail balance),
    // LVPack.Vdc = 51.6148.
    EXPECT_TRUE(buck::PlantSetFault(state, "OpenGates", false));
    RunSteps(state, kStepsPerHalfSecond);
    EXPECT_FLOAT_EQ(state.Fault.OpenGates, 0.0f);
    EXPECT_GE(state.Conv.I_L, -0.5f);
    EXPECT_LE(state.Conv.I_L, 1.5f);
    EXPECT_GE(state.LVPack.Vdc, 51.0f);
    EXPECT_LE(state.LVPack.Vdc, 52.4f);
}

// Isolated DAB transfer-law pins: pack bus voltages state-fed, one substep
// per case — python tape values in the comments (six-case sweep subset +
// clamp/reversal/OpenGates; proof worst rel diff 6.6e-5 vs this C++ engine).
TEST(PlantCodegenGoldenDcDc, DabIsolatedLawPinsPython) {
    dab::PlantState state{};
    dab::PlantInit(state);

    // D7: this rig's unwired input is "PhaseShift"; "Duty" does not exist.
    EXPECT_FALSE(dab::PlantSetInput(state, "Duty", 50.0f));

    struct LawCase {
        double vh, vl, phi;
        float pLo, pHi;
        const char* tag;
    };
    // Bands: python reference values below with ~1.5-2 % headroom — 200x+
    // over the proof's measured 6.6e-5 worst rel diff.
    //   (350,  96, 0.05) -> P = 13156.240734
    //   (350, 192, 0.20) -> P = 100143.338217
    //   (350,  96, 1.20) -> P = 198298.261539
    const LawCase cases[] = {
        {350.0, 96.0, 0.05, 12900.0f, 13400.0f, "small forward"},
        {350.0, 192.0, 0.20, 99000.0f, 101300.0f, "matched-ratio"},
        {350.0, 96.0, 1.2, 196000.0f, 200600.0f, "deep forward"},
    };
    for (const LawCase& c : cases) {
        DabCase(state, c.vh, c.vl, c.phi);
        EXPECT_GE(state.Dab.P_Transfer, c.pLo) << c.tag;
        EXPECT_LE(state.Dab.P_Transfer, c.pHi) << c.tag;
        // Algebraic side currents consistent with P: I_High = P/V_High;
        // receiver convention — I_Low is NEGATIVE while the HV side
        // delivers (the LV pack receives).
        EXPECT_NEAR(state.Dab.I_High, state.Dab.P_Transfer / c.vh, 1.0f)
            << c.tag;
        EXPECT_NEAR(state.Dab.I_Low, -state.Dab.P_Transfer / c.vl, 2.5f)
            << c.tag;
        EXPECT_LT(state.Dab.I_Low, 0.0f) << c.tag;
    }

    // Reversal: python (200, 40, -0.05) -> P = -3132.438270, sign flip with
    // the LV side now leading; I_Low turns positive (LV sources).
    DabCase(state, 200.0, 40.0, -0.05);
    EXPECT_GE(state.Dab.P_Transfer, -3300.0f);
    EXPECT_LE(state.Dab.P_Transfer, -2950.0f);
    EXPECT_GT(state.Dab.I_Low, 0.0f);

    // Clamp at |phi|=pi: python (350, 96, 3.14) -> P = 425.628317 sits
    // within eps of the law's zero-crossing; beyond pi the clamp lands
    // exactly on it.
    DabCase(state, 350.0, 96.0, 3.14);
    EXPECT_GE(state.Dab.P_Transfer, 0.0f);
    EXPECT_LT(std::fabs(state.Dab.P_Transfer), 1500.0f);
    DabCase(state, 350.0, 96.0, 3.2);
    EXPECT_FLOAT_EQ(state.Dab.P_Transfer, 0.0f);

    // OpenGates kills the transfer instantly — there is no integrator, so
    // every gate-side channel zeroes; release restores the law.
    EXPECT_TRUE(dab::PlantSetFault(state, "OpenGates", true));
    EXPECT_FALSE(dab::PlantSetFault(state, "NotARealFault", true));
    DabCase(state, 350.0, 96.0, 1.2);
    EXPECT_FLOAT_EQ(state.Dab.P_Transfer, 0.0f);
    EXPECT_FLOAT_EQ(state.Dab.I_High, 0.0f);
    EXPECT_FLOAT_EQ(state.Dab.I_Low, 0.0f);
    EXPECT_FLOAT_EQ(state.Dab.P_DrawHigh, 0.0f);
    EXPECT_FLOAT_EQ(state.Dab.P_DrawLow, 0.0f);
    EXPECT_TRUE(dab::PlantSetFault(state, "OpenGates", false));
    DabCase(state, 350.0, 96.0, 1.2);
    EXPECT_GE(state.Dab.P_Transfer, 196000.0f);
    EXPECT_LE(state.Dab.P_Transfer, 200600.0f);

    // Degenerate rail: V_Low pulled to zero -> v2' <= 0 branch, P = 0.
    DabCase(state, 350.0, 0.0, 1.2);
    EXPECT_FLOAT_EQ(state.Dab.P_Transfer, 0.0f);
}

// Graph-supplied identity channels (D9/D10) for both rigs: the machines are
// absent so boot params are NaN; the supply defaults come from the first
// DcBus (the HV bench unit in both graphs).
TEST(PlantCodegenGoldenDcDc, BootIdentityAndSupplyDefaults) {
    {
        buck::PlantState state{};
        buck::PlantInit(state);
        double vSet = 0, iLimit = 0, sinkLimit = 0, cvClamp = -1,
               cvClampSink = -1, seriesR = 0;
        bool canSink = false;
        buck::PlantSupplyDefaults(state, &vSet, &iLimit, &sinkLimit, &canSink,
                                  &cvClamp, &cvClampSink, &seriesR);
        EXPECT_DOUBLE_EQ(vSet, 96.0);
        EXPECT_DOUBLE_EQ(iLimit, 60.0);
        EXPECT_DOUBLE_EQ(sinkLimit, 20.0);
        EXPECT_TRUE(canSink);
        EXPECT_DOUBLE_EQ(cvClamp, 0.0);
        EXPECT_DOUBLE_EQ(cvClampSink, 0.0);
        EXPECT_DOUBLE_EQ(seriesR, 8.0);

        double poles = 0, rsOhm = 0, encOffset = 0;
        buck::PlantBootParams(state, &poles, &rsOhm, &encOffset);
        EXPECT_TRUE(std::isnan(poles));
        EXPECT_TRUE(std::isnan(rsOhm));
        EXPECT_TRUE(std::isnan(encOffset));
    }
    {
        dab::PlantState state{};
        dab::PlantInit(state);
        double vSet = 0, iLimit = 0, sinkLimit = 0, cvClamp = -1,
               cvClampSink = -1, seriesR = 0;
        bool canSink = false;
        dab::PlantSupplyDefaults(state, &vSet, &iLimit, &sinkLimit, &canSink,
                                 &cvClamp, &cvClampSink, &seriesR);
        EXPECT_DOUBLE_EQ(vSet, 350.0);
        EXPECT_DOUBLE_EQ(iLimit, 250.0);
        EXPECT_DOUBLE_EQ(sinkLimit, 80.0);
        EXPECT_TRUE(canSink);
        EXPECT_DOUBLE_EQ(cvClamp, 0.0);
        EXPECT_DOUBLE_EQ(seriesR, 15.0);

        double poles = 0, rsOhm = 0, encOffset = 0;
        dab::PlantBootParams(state, &poles, &rsOhm, &encOffset);
        EXPECT_TRUE(std::isnan(poles));
        EXPECT_TRUE(std::isnan(rsOhm));
        EXPECT_TRUE(std::isnan(encOffset));

        // The DAB rig's measurement view: truth channels are direct wires
        // (no hall/loss chain), the machine-only channels stay NaN.
        DabCase(state, 350.0, 96.0, 0.05);
        dab::PlantMeas meas{};
        dab::PlantCollectMeas(state, meas);
        EXPECT_FLOAT_EQ(meas.Iu_A, state.Dab.I_High);
        EXPECT_FLOAT_EQ(meas.Iv_A, state.Dab.I_Low);
        EXPECT_FLOAT_EQ(meas.P_Dc_W, state.Dab.P_Transfer);
        EXPECT_FLOAT_EQ(meas.P_ShaftEm_W, state.Dab.P_Transfer);
        EXPECT_NEAR(meas.Vdc_V, state.HVBus.Vdc, 0.45);  // 0.44 V LSB
        EXPECT_NEAR(meas.Vu_V, state.LVPack.Vdc, 0.25);  // 0.24 V LSB
        EXPECT_TRUE(std::isnan(meas.P_Heat_W));  // no loss node on this rig
        EXPECT_TRUE(std::isnan(meas.Enc_Sin));
        EXPECT_TRUE(std::isnan(meas.Mech_Rpm));
        EXPECT_TRUE(std::isnan(meas.Temp_Inv1_C));
    }
}

}  // namespace
