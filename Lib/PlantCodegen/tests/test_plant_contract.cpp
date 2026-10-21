// PlantCodegen contract tests on the Zero DSR placeholder and the CvClamp
// supply variant: smoke tapes recorded from the python reference evaluator
// (twin/plantgraph/plantgraph.py, 2026-10-21) plus the D10 boot-params
// surface on the 16 pole-pair machine.
//
// - zero_dsr_zf75_7: dyno 600 rpm, duty (52, 49, 49), dt_sub = 200 us, 200
//   substeps (mirrors the dsr_smoke segment of proof_teco_parity.py).
// - gen7_cvclamp.json: gen7 with CvClamp_V = 150 V / CvClampSink_A = 50 A,
//   driven negative-torque (regen) so the link charges through the clamp —
//   asserts the D9-widened clamp branch end-to-end against the python tape.

#include <cmath>
#include <gtest/gtest.h>

#include "zero_generated.h"
#include "cvclamp_generated.h"

namespace {

// --- Zero DSR smoke tape ----------------------------------------------------

TEST(PlantCodegenContract, ZeroDsrSmokeTracksPythonTape) {
    plant_zero_dsr_zf75_7::PlantState state{};
    plant_zero_dsr_zf75_7::PlantInit(state);
    plant_zero_dsr_zf75_7::PlantSetDuty(state, 52.0f, 49.0f, 49.0f);
    plant_zero_dsr_zf75_7::PlantSetDynoRpm(state, 517.0f);  // off electrical-rev wrap razors (600rpm*pp5 landed exactly on 2pi at substep 100)

    struct Checkpoint {
        int substep;
        double vdcMeas, id, iq, vdc, iu, iv, iw;
        int encSin, encCos;
    };
    // python PlantGraphSim tape (200 us substeps).
    constexpr Checkpoint kTape[] = {
        {50,  117.04, -842.3522386908612, -426.1891790081615, 117.04129704791694,
         943.46, -444.08000000000004, -499.38000000000005, 41414, 51832},
        {100, 114.84, 29.537479263922616, -37.546184977516006, 114.80536371551598,
         -9.590000000000002, -35.7, 45.36000000000001, 50117, 44559},
        {200, 114.40, -374.1675589970487, -65.83057204337571, 114.5790831184064,
         -2.5200000000000005, 330.26000000000005, -327.74, 47772, 47396},
    };

    plant_zero_dsr_zf75_7::PlantMeas meas{};
    std::size_t next = 0;
    for (int i = 1; i <= 200; ++i) {
        plant_zero_dsr_zf75_7::PlantStep(state, 200e-6);
        plant_zero_dsr_zf75_7::PlantCollectMeas(state, meas);

        // Healthy encoder bounds (the dsr_smoke gate of the teco proof).
        EXPECT_GE(meas.Enc_Sin, 11423.0f) << "substep " << i;
        EXPECT_LE(meas.Enc_Sin, 53864.0f) << "substep " << i;
        EXPECT_GE(meas.Enc_Cos, 11346.0f) << "substep " << i;
        EXPECT_LE(meas.Enc_Cos, 53727.0f) << "substep " << i;
        EXPECT_GE(meas.Vdc_V, 0.5f) << "substep " << i;
        EXPECT_LE(meas.Vdc_V, 2000.0f) << "substep " << i;
        EXPECT_FLOAT_EQ(meas.Mech_Rpm, 517.0f);
        // pp = 5 (ZF75-7, ride-data verified 2026-10-21; was placeholder 16)
        EXPECT_FLOAT_EQ(meas.Elec_Rpm, 2585.0f);

        if (next < std::size(kTape) && kTape[next].substep == i) {
            const Checkpoint& cp = kTape[next++];
            EXPECT_NEAR(meas.Vdc_V, cp.vdcMeas, 0.5) << "Vdc_V @" << i;
            EXPECT_NEAR(state.Machine.Id, cp.id, 0.5) << "id @" << i;
            EXPECT_NEAR(state.Machine.Iq, cp.iq, 0.5) << "iq @" << i;
            EXPECT_NEAR(state.DcBus.Vdc, cp.vdc, 0.02) << "vdc @" << i;
            EXPECT_NEAR(meas.Iu_A, cp.iu, 0.15) << "iu @" << i;
            EXPECT_NEAR(meas.Iv_A, cp.iv, 0.15) << "iv @" << i;
            EXPECT_NEAR(meas.Iw_A, cp.iw, 0.15) << "iw @" << i;
            EXPECT_NEAR(meas.Enc_Sin, cp.encSin, 1.0) << "enc_sin @" << i;
            EXPECT_NEAR(meas.Enc_Cos, cp.encCos, 1.0) << "enc_cos @" << i;
        }
    }
    EXPECT_EQ(next, std::size(kTape));

    // D10: the 5 pole-pair ZF75-7 DSR machine (ride-data verified; was the
    // 16-pp placeholder) with its ride-fitted Rs + the shared encoder offset.
    double poles = 0, rsOhm = 0, encOffset = 0;
    plant_zero_dsr_zf75_7::PlantBootParams(state, &poles, &rsOhm, &encOffset);
    EXPECT_DOUBLE_EQ(poles, 5.0);
    EXPECT_DOUBLE_EQ(rsOhm, 0.0087);
    EXPECT_DOUBLE_EQ(encOffset, 33.034);
}

// --- CvClamp supply branch --------------------------------------------------

TEST(PlantCodegenContract, CvClampBranchTracksPythonTape) {
    constexpr double kDt = 200e-6, kPeriod = 400e-6, kVdcRef = 144.7;
    constexpr double kOmegaE = 3311.6 * (2.0 * M_PI / 60.0) * 5.0;
    // Rotor-locked drive with strongly negative q volts: sustained negative
    // P_Elec (<-> negative P_Draw) charges the link through the 150 V clamp.
    constexpr double kVd = 4.341, kVq = -30.0;

    plant_cvclamp::PlantState state{};
    plant_cvclamp::PlantInit(state);
    // The graph's clamp knobs must have landed as mutable state (D9).
    EXPECT_DOUBLE_EQ(state.DcBus.CvClamp_V, 150.0);
    EXPECT_DOUBLE_EQ(state.DcBus.CvClampSink_A, 50.0);
    plant_cvclamp::PlantSetDynoRpm(state, 3311.6f);

    struct Checkpoint {
        int period;
        double vdc, id, iq, dclinkP, dclinkI, iu, vdcMeas;
    };
    // python PlantGraphSim tape on gen7_cvclamp.json.
    constexpr Checkpoint kTape[] = {
        {10, 144.22109148084442, 499.5864037597299, -209.0256406301707,
         -9153.321268148502, -63.467286054788005, 523.88, 144.32},
        {25, 218.68900882477922, -1895.7540056585353, 265.29325824198423,
         -108720.6692724395, -497.14738686090095, 149.73000000000002, 218.68},
        {50, 227.90737404978884, 69.45380329406248, -975.8425625909483,
         37759.838045415396, 165.68063320832414, -187.18, 227.92},
        {100, 331.39090953073367, -1678.099720304833, -1016.4976985206556,
         301930.10407861584, 911.0995365146382, -1200.0, 331.32},
        {150, 413.94876560759025, -2000.0, -409.9277649442762,
         131602.76168417497, 317.9204109740722, 1200.0, 414.04},
    };

    plant_cvclamp::PlantMeas meas{};
    double theta = 0.0;
    std::size_t next = 0;
    for (int period = 1; period <= 150; ++period) {
        const double th = theta + kOmegaE * kPeriod / 2.0;
        const double ct = std::cos(th), st = std::sin(th);
        const double va = ct * kVd - st * kVq, vb = st * kVd + ct * kVq;
        const double vv = -0.5 * va + 0.8660254037844386 * vb;
        const double vw = -0.5 * va - 0.8660254037844386 * vb;
        plant_cvclamp::PlantSetDuty(state,
                                    static_cast<float>(50.0 + 50.0 * va / kVdcRef),
                                    static_cast<float>(50.0 + 50.0 * vv / kVdcRef),
                                    static_cast<float>(50.0 + 50.0 * vw / kVdcRef));
        plant_cvclamp::PlantStep(state, kDt);
        plant_cvclamp::PlantStep(state, kDt);
        theta = std::fmod(theta + kOmegaE * kPeriod, 2.0 * M_PI);
        plant_cvclamp::PlantCollectMeas(state, meas);

        if (next < std::size(kTape) && kTape[next].period == period) {
            const Checkpoint& cp = kTape[next++];
            EXPECT_NEAR(state.DcBus.Vdc, cp.vdc, 2.0) << "vdc @" << period;
            EXPECT_NEAR(state.Machine.Id, cp.id, 2.0) << "id @" << period;
            EXPECT_NEAR(state.Machine.Iq, cp.iq, 2.0) << "iq @" << period;
            EXPECT_NEAR(meas.Dclink_P_W, cp.dclinkP, 1000.0) << "dclinkP @" << period;
            EXPECT_NEAR(meas.Dclink_I_A, cp.dclinkI, 20.0) << "dclinkI @" << period;
            EXPECT_NEAR(meas.Iu_A, cp.iu, 0.15) << "iu @" << period;
            EXPECT_NEAR(meas.Vdc_V, cp.vdcMeas, 0.3) << "vdcMeas @" << period;
        }
    }
    EXPECT_EQ(next, std::size(kTape));

    // The clamp was engaged for most of the run: the regen feed is positive
    // INTO the link (Dclink_P_W > 0) while the bus sits above the 150 V
    // clamp setpoint.
    EXPECT_GT(meas.Dclink_P_W, 0.0f);
    EXPECT_GT(state.DcBus.Vdc, 150.0f);
    // Plumbing control: the SAME drive with CvClampSink_A = 25 charges
    // measurably harder in python (vdc = 472.25 at period 150) — the C++
    // engine must land far below that, proving the 50 A graph value is what
    // actually sank current here (not a stale/hardcoded sink limit).
    EXPECT_LT(state.DcBus.Vdc, 450.0f);
}

}  // namespace
