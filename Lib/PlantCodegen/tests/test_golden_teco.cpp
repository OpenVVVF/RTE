// PlantCodegen golden test for the TECO V/Hz induction graph: drives the
// *generated* plant engine (plant_golden_teco) against a checkpoint tape
// recorded from the python reference evaluator, mirroring the protocol of
// twin/plantgraph/proof_teco_parity.py: rotating 30 Hz V/Hz duties
// (v_amp = 18 V on the 141.8 V starved 1 A bench), Omega_Cmd pushed through
// PlantSetInput, dt_sub = 50e-6 -> 8 substeps per 400 us control period,
// 3750 periods = 1.5 s.
//
// The tape below is the python PlantGraphSim run (teco_vhz_induction.json,
// recorded 2026-10-21); tolerances carry the C++ float32 emulation noise
// against the float64 tape with ~5-10x headroom over the measured worst
// drift (~1e-4 relative on the dq states, ~0.01 on the power channels).

#include <cmath>
#include <gtest/gtest.h>

#include "teco_generated.h"

namespace {

constexpr double kDt = 50e-6;
constexpr double kPeriod = 400e-6;
constexpr double kOmegaCmd = 2.0 * M_PI * 30.0;
constexpr double kVAmp = 18.0;
constexpr double kVdcRef = 141.8;

struct TecoDrive {
    plant_teco_vhz_induction::PlantState state{};
    double phi = 0.0;

    void StepPeriod() {
        const double va = kVAmp * std::cos(phi);
        const double vb = kVAmp * std::sin(phi);
        const double vv = -0.5 * va + 0.8660254037844386 * vb;
        const double vw = -0.5 * va - 0.8660254037844386 * vb;
        plant_teco_vhz_induction::PlantSetDuty(
            state, static_cast<float>(50.0 + 50.0 * va / kVdcRef),
            static_cast<float>(50.0 + 50.0 * vv / kVdcRef),
            static_cast<float>(50.0 + 50.0 * vw / kVdcRef));
        phi = std::fmod(phi + kOmegaCmd * kPeriod, 2.0 * M_PI);
        for (int s = 0; s < 8; ++s) {
            plant_teco_vhz_induction::PlantStep(state, kDt);
        }
    }
};

struct TecoCheckpoint {
    int period;
    double isd, isq, vdc, rpm, iu, iw, pheat, pdc, pshaft;
};

// python reference tape (isd/isq/vdc/rpm are float64 truth/meas; iu/iw the
// 0.07 A-quantized hall view; power channels the truth diag ports).
constexpr TecoCheckpoint kTecoTape[] = {
    {250,  5.50353538837236,  -13.44232210795699, 141.60370970710952,
     1.6392309462309886, 5.53, 8.89, 159.7302291278083, 68.1240203756569,
     11.366550193073639},
    {500,  5.534667262864145, -13.450057107103923, 141.62820979031724,
     2.988218082521281, 5.53, 8.89, 159.2201531785632, 73.19544824997506,
     17.166657994884762},
    {1000, 5.555712485918804, -13.447654560830083, 141.66718796995067,
     5.680887755722264, 5.53, 8.89, 158.11397138539294, 87.10125282843741,
     32.652722277876066},
    {2000, 5.571950088216129, -13.418944915034912, 141.72558745586133,
     11.054378273047373, 5.6000000000000005, 8.82, 155.81839485723748,
     122.0858041137445, 70.91666860340524},
    {3000, 5.521199446524344, -13.395706416799161, 141.7780086334167,
     16.412071628938325, 5.53, 8.82, 153.47147357911922, 146.28794390591008,
     98.4715530785969},
    {3750, 5.47499974047772,  -13.435683729439315, 141.79514205061096,
     20.437387787733247, 5.460000000000001, 8.89, 151.84810699666718,
     137.90288958840773, 92.4055938788832},
};

TEST(PlantCodegenGoldenTeco, TracksPythonReferenceTape) {
    TecoDrive drive;
    plant_teco_vhz_induction::PlantInit(drive.state);
    // IM V/Hz: the synchronous speed is an unwired external; python feeds it
    // via set_input("Omega_Cmd", 2*pi*30.0), C++ via PlantSetInput.
    EXPECT_TRUE(plant_teco_vhz_induction::PlantSetInput(
        drive.state, "Omega_Cmd", static_cast<float>(kOmegaCmd)));

    plant_teco_vhz_induction::PlantMeas meas{};
    std::size_t next = 0;
    for (int period = 1; period <= 3750; ++period) {
        drive.StepPeriod();
        plant_teco_vhz_induction::PlantCollectMeas(drive.state, meas);

        // The teco rig has no encoder: both Enc channels read NaN always.
        EXPECT_TRUE(std::isnan(meas.Enc_Sin)) << "period " << period;
        EXPECT_TRUE(std::isnan(meas.Enc_Cos)) << "period " << period;
        EXPECT_GE(meas.Vdc_V, 0.5f) << "period " << period;
        EXPECT_NEAR(meas.Vdc_V, 141.68f, 0.30f) << "period " << period;

        if (next < std::size(kTecoTape) && kTecoTape[next].period == period) {
            const TecoCheckpoint& cp = kTecoTape[next++];
            EXPECT_NEAR(drive.state.Machine.Isd, cp.isd, 0.02) << "isd @" << period;
            EXPECT_NEAR(drive.state.Machine.Isq, cp.isq, 0.02) << "isq @" << period;
            EXPECT_NEAR(drive.state.DcBus.Vdc, cp.vdc, 0.05) << "vdc @" << period;
            EXPECT_NEAR(meas.Mech_Rpm, cp.rpm, 0.05) << "rpm @" << period;
            EXPECT_NEAR(meas.Iu_A, cp.iu, 0.15) << "iu @" << period;
            EXPECT_NEAR(meas.Iw_A, cp.iw, 0.15) << "iw @" << period;
            EXPECT_NEAR(meas.P_Heat_W, cp.pheat, 0.5) << "pheat @" << period;
            EXPECT_NEAR(meas.P_Dc_W, cp.pdc, 0.5) << "pdc @" << period;
            EXPECT_NEAR(meas.P_ShaftEm_W, cp.pshaft, 0.5) << "pshaft @" << period;
        }
    }
    EXPECT_EQ(next, std::size(kTecoTape));
}

TEST(PlantCodegenGoldenTeco, OpenGatesWindowAndSensorRailWindow) {
    TecoDrive drive;
    plant_teco_vhz_induction::PlantInit(drive.state);
    EXPECT_TRUE(plant_teco_vhz_induction::PlantSetInput(
        drive.state, "Omega_Cmd", static_cast<float>(kOmegaCmd)));
    for (int period = 0; period < 1000; ++period) drive.StepPeriod();

    const double magBefore = std::hypot(drive.state.Machine.Isd,
                                        drive.state.Machine.Isq);
    ASSERT_GT(magBefore, 10.0);

    // D8: the OpenGates raise reports the name exists; bogus names -> false.
    EXPECT_TRUE(plant_teco_vhz_induction::PlantSetFault(drive.state, "OpenGates", true));
    EXPECT_FALSE(plant_teco_vhz_induction::PlantSetFault(drive.state, "OpenGte", true));
    // 10 ms = 5 tau of the six-switch-open freewheel collapse.
    for (int period = 0; period < 25; ++period) drive.StepPeriod();
    const double magAfter = std::hypot(drive.state.Machine.Isd,
                                       drive.state.Machine.Isq);
    EXPECT_LT(magAfter, 0.01 * magBefore);

    plant_teco_vhz_induction::PlantMeas meas{};
    plant_teco_vhz_induction::PlantCollectMeas(drive.state, meas);
    // Gates open -> P_Elec gated to zero, Loss idle -> no plate heat.
    EXPECT_FLOAT_EQ(meas.P_ShaftEm_W, 0.0f);
    EXPECT_FLOAT_EQ(meas.P_Heat_W, 0.0f);
    // The starved source alone recharges the link toward the 141.8 setpoint.
    EXPECT_NEAR(drive.state.DcBus.Vdc, 141.8f, 0.5f);
    EXPECT_TRUE(plant_teco_vhz_induction::PlantSetFault(drive.state, "OpenGates", false));

    // D7: teco's three Hall Rail ports are unwired; one PlantSetInput must
    // assign ALL of them (python set_input broadcast semantics).
    EXPECT_TRUE(plant_teco_vhz_induction::PlantSetInput(drive.state, "Rail", 1.0f));
    EXPECT_FLOAT_EQ(drive.state.HallU.Rail, 1.0f);
    EXPECT_FLOAT_EQ(drive.state.HallV.Rail, 1.0f);
    EXPECT_FLOAT_EQ(drive.state.HallW.Rail, 1.0f);
    for (int period = 0; period < 10; ++period) drive.StepPeriod();
    plant_teco_vhz_induction::PlantCollectMeas(drive.state, meas);
    // Recorded isolated-rail-collapse artifacts verbatim (per-channel
    // RailVal_A), bypassing quantization AND the range clamp.
    EXPECT_FLOAT_EQ(meas.Iu_A, 1390.0f);
    EXPECT_FLOAT_EQ(meas.Iv_A, -1250.0f);
    EXPECT_FLOAT_EQ(meas.Iw_A, 1180.0f);
    // ...and back to live on release.
    EXPECT_TRUE(plant_teco_vhz_induction::PlantSetInput(drive.state, "Rail", 0.0f));
    drive.StepPeriod();
    plant_teco_vhz_induction::PlantCollectMeas(drive.state, meas);
    EXPECT_LT(std::fabs(meas.Iu_A), 1200.0f);
}

TEST(PlantCodegenGoldenTeco, BootParamsAndSupplyDefaults) {
    plant_teco_vhz_induction::PlantState state{};
    plant_teco_vhz_induction::PlantInit(state);

    // D10: poles/Rs from the InductDq machine (PmsmDq absent); the graph
    // carries no sin/cos encoder -> NaN for the absent source.
    double poles = 0, rsOhm = 0, encOffset = 0;
    plant_teco_vhz_induction::PlantBootParams(state, &poles, &rsOhm, &encOffset);
    EXPECT_DOUBLE_EQ(poles, 2.0);
    EXPECT_DOUBLE_EQ(rsOhm, 0.208);
    EXPECT_TRUE(std::isnan(encOffset));

    // D9: the starved bench defaults live in the graph, not the host.
    double vSet = 0, iLimit = 0, sinkLimit = 0, cvClamp = -1, cvClampSink = -1,
           seriesR = 0;
    bool canSink = false;
    plant_teco_vhz_induction::PlantSupplyDefaults(
        state, &vSet, &iLimit, &sinkLimit, &canSink, &cvClamp, &cvClampSink, &seriesR);
    EXPECT_DOUBLE_EQ(vSet, 141.8);
    EXPECT_DOUBLE_EQ(iLimit, 1.0);
    EXPECT_DOUBLE_EQ(sinkLimit, 1.0);
    EXPECT_TRUE(canSink);
    EXPECT_DOUBLE_EQ(cvClamp, 0.0);
    EXPECT_DOUBLE_EQ(cvClampSink, 0.0);
    EXPECT_DOUBLE_EQ(seriesR, 5.0);
}

}  // namespace
