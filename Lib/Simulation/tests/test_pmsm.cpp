#include <cmath>
#include <gtest/gtest.h>

#include "simulation/PmsmPlant.h"

using namespace simulation;

namespace {

PmsmParameters defaultMotor() {
    PmsmParameters p;
    p.rs = 2.2479f;
    p.ld = 17.65e-3f;
    p.lq = 17.65e-3f;
    p.psi_f = 0.4686f;
    p.pole_pairs = 4;
    p.inertia = 0.0254f;
    return p;
}

TEST(PmsmPlant, ZeroVoltageCurrentDecay) {
    PmsmPlant plant(defaultMotor());
    PmsmState st;
    st.id = 5.0f;
    st.iq = -3.0f;
    plant.reset(st);
    const float tau = defaultMotor().ld / defaultMotor().rs;
    plant.step(0.0f, 0.0f, 0.0f, 0.1f * tau);
    EXPECT_LT(std::fabs(plant.state().id), 5.0f);
    EXPECT_LT(std::fabs(plant.state().iq), 3.0f);
}

TEST(PmsmPlant, StandstillDAxisResponse) {
    PmsmPlant plant(defaultMotor());
    const float dt = 100e-6f;
    for (int i = 0; i < 1000; ++i) {
        plant.step(10.0f, 0.0f, 0.0f, dt);
    }
    EXPECT_GT(plant.state().id, 0.0f);
    EXPECT_NEAR(plant.state().iq, 0.0f, 0.5f);
}

TEST(PmsmPlant, StandstillQAxisResponse) {
    PmsmPlant plant(defaultMotor());
    const float dt = 100e-6f;
    for (int i = 0; i < 1000; ++i) {
        plant.step(0.0f, 10.0f, 0.0f, dt);
    }
    EXPECT_GT(plant.state().iq, 0.0f);
}

TEST(PmsmPlant, BackEmfAtNonzeroSpeed) {
    PmsmPlant plant(defaultMotor());
    PmsmState st;
    st.omega_m = 100.0f;
    st.omega_e = st.omega_m * plant.parameters().pole_pairs;
    plant.reset(st);
    const float iq_before = plant.state().iq;
    plant.step(0.0f, 0.0f, 0.0f, 1e-3f);
    EXPECT_NE(plant.state().iq, iq_before);
}

TEST(PmsmPlant, SpmTorqueVersusIq) {
    PmsmPlant plant(defaultMotor());
    PmsmState st;
    st.iq = 10.0f;
    plant.reset(st);
    const float expected = 1.5f * plant.parameters().pole_pairs * plant.parameters().psi_f * 10.0f;
    EXPECT_NEAR(plant.state().torque_em, expected, 1e-3f);
}

TEST(PmsmPlant, MechanicalAcceleration) {
    PmsmPlant plant(defaultMotor());
    const float omega0 = plant.state().omega_m;
    for (int i = 0; i < 500; ++i) {
        plant.step(0.0f, 20.0f, 0.0f, 100e-6f);
    }
    EXPECT_GT(plant.state().omega_m, omega0);
}

TEST(PmsmPlant, ElectricalSpeedEqualsPolePairsTimesMechanical) {
    PmsmPlant plant(defaultMotor());
    PmsmState st;
    st.omega_m = 50.0f;
    st.omega_e = st.omega_m * plant.parameters().pole_pairs;
    plant.reset(st);
    EXPECT_NEAR(plant.state().omega_e, plant.state().omega_m * plant.parameters().pole_pairs, 1e-6f);
}

TEST(PmsmPlant, ElectricalAngleConsistency) {
    PmsmPlant plant(defaultMotor());
    PmsmState st;
    st.theta_e = 0.5f;
    st.omega_e = 100.0f;
    plant.reset(st);
    const float theta0 = plant.state().theta_e;
    plant.step(0.0f, 0.0f, 0.0f, 1e-3f);
    EXPECT_GE(plant.state().theta_e, 0.0f);
    EXPECT_LT(plant.state().theta_e, kTwoPi);
    EXPECT_NE(plant.state().theta_e, theta0);
}

TEST(PmsmPlant, HighResolutionReferenceComparison) {
    PmsmPlant plant(defaultMotor());
    const float dt = 1e-5f;
    float id = 0.0f;
    float iq = 0.0f;
    float theta_e = 0.0f;
    float omega_e = 0.0f;
    for (int i = 0; i < 100; ++i) {
        const float vd = 5.0f;
        const float vq = 0.0f;
        const float did = (vd - defaultMotor().rs * id + omega_e * defaultMotor().lq * iq) / defaultMotor().ld;
        const float diq =
            (vq - defaultMotor().rs * iq - omega_e * defaultMotor().ld * id - omega_e * defaultMotor().psi_f) /
            defaultMotor().lq;
        id += did * dt;
        iq += diq * dt;
        theta_e = wrapAngle0TwoPi(theta_e + omega_e * dt);
        plant.step(vd, vq, 0.0f, dt);
    }
    EXPECT_NEAR(plant.state().id, id, 0.05f);
    EXPECT_NEAR(plant.state().iq, iq, 0.05f);
}

}  // namespace
