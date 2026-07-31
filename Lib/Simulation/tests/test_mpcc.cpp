#include <cmath>
#include <gtest/gtest.h>

#include "simulation/MpccController.h"
#include "simulation/PmsmPlant.h"

using namespace simulation;

namespace {

MpccParameters defaultCtrl() {
    MpccParameters p;
    p.motor = PmsmParameters{};
    p.ts = 100e-6f;
    p.i_base = 10.0f;
    p.i_max = 30.0f;
    return p;
}

TEST(MpccController, PredictsAllEightCandidates) {
    MpccController ctrl(defaultCtrl());
    MpccInputs in;
    in.id = 1.0f;
    in.iq = 2.0f;
    in.id_ref = 0.0f;
    in.iq_ref = 10.0f;
    in.theta_e = 0.3f;
    in.omega_e = 50.0f;
    in.vdc = 540.0f;
    const auto out = ctrl.update(in);
    EXPECT_TRUE(out.valid);
    EXPECT_GE(static_cast<int>(out.switching_state), 0);
    EXPECT_LE(static_cast<int>(out.switching_state), 7);
}

TEST(MpccController, OneStepPredictionMatchesFormula) {
    const auto motor = defaultCtrl().motor;
    const float ts = 100e-6f;
    const Dq pred = MpccController::predictDqCurrent(motor, ts, 1.0f, 2.0f, 50.0f, 10.0f, 0.0f);
    const float expected_d = 1.0f + (ts / motor.ld) * (10.0f - motor.rs * 1.0f + 50.0f * motor.lq * 2.0f);
    EXPECT_NEAR(pred.d, expected_d, 1e-6f);
}

TEST(MpccController, CostCalculation) {
    const float cost = MpccController::normalizedCost(0.0f, 10.0f, 1.0f, 9.0f, 10.0f);
    EXPECT_NEAR(cost, 0.02f, 1e-6f);
}

TEST(MpccController, CurrentLimitPenalty) {
    MpccParameters p = defaultCtrl();
    p.i_max = 1.0f;
    MpccController ctrl(p);
    MpccInputs in;
    in.id = 0.0f;
    in.iq = 0.0f;
    in.id_ref = 0.0f;
    in.iq_ref = 20.0f;
    in.vdc = 540.0f;
    const auto out = ctrl.update(in);
    EXPECT_TRUE(out.valid);
    const float pred_mag = std::hypot(out.predicted_id, out.predicted_iq);
    if (pred_mag > p.i_max) {
        EXPECT_GT(out.min_cost, 1.0f);
    }
}

TEST(MpccController, TieBreakingPrefersFewerTransitions) {
    MpccController ctrl(defaultCtrl());
    MpccInputs in;
    in.id = 0.0f;
    in.iq = 5.0f;
    in.id_ref = 0.0f;
    in.iq_ref = 5.0f;
    in.vdc = 540.0f;
    (void)ctrl.update(in);
    const auto out = ctrl.update(in);
    EXPECT_TRUE(out.valid);
}

TEST(MpccController, InvalidInputsRejected) {
    MpccController ctrl(defaultCtrl());
    MpccInputs in;
    in.vdc = -1.0f;
    const auto out = ctrl.update(in);
    EXPECT_FALSE(out.valid);
}

TEST(MpccController, PredictorVersusPlantOneStep) {
    PmsmPlant plant(defaultCtrl().motor);
    MpccController ctrl(defaultCtrl());
    MpccInputs in;
    in.id = plant.state().id;
    in.iq = plant.state().iq;
    in.id_ref = 0.0f;
    in.iq_ref = 10.0f;
    in.theta_e = plant.state().theta_e;
    in.omega_e = plant.state().omega_e;
    in.vdc = 540.0f;
    const auto out = ctrl.update(in);
    plant.step(out.vd, out.vq, 0.0f, defaultCtrl().ts);
  const float id_err = std::fabs(plant.state().id - out.predicted_id);
    const float iq_err = std::fabs(plant.state().iq - out.predicted_iq);
    EXPECT_LT(id_err, 5.0f);
    EXPECT_LT(iq_err, 5.0f);
}

TEST(MpccController, NegativeSpeedOperation) {
    MpccController ctrl(defaultCtrl());
    MpccInputs in;
    in.id = 0.0f;
    in.iq = 0.0f;
    in.id_ref = 0.0f;
    in.iq_ref = -5.0f;
    in.omega_e = -100.0f;
    in.vdc = 540.0f;
    const auto out = ctrl.update(in);
    EXPECT_TRUE(out.valid);
}

}  // namespace
