#include <cmath>
#include <gtest/gtest.h>

#include "AdrcMpccHybrid.h"
#include "LinearAdrc.h"
#include "simulation/BenchMotor.h"

using hybrid_adrc_mpcc::AdrcMpccHybrid;
using hybrid_adrc_mpcc::HybridInputs;
using hybrid_adrc_mpcc::HybridParams;
using hybrid_adrc_mpcc::LinearAdrc;
using hybrid_adrc_mpcc::LinearAdrcParams;

TEST(LinearAdrc, ObserverMovesTowardStep) {
    LinearAdrcParams p;
    p.dt = 200e-6f;
    p.b0 = 1.0f / 120e-6f;
    p.omega_o = 3000.0f;
    LinearAdrc eso(p);
    eso.reset(0.0f);

    float y = 0.0f;
    float u = 0.0f;
    for (int k = 0; k < 500; ++k) {
        if (k == 50) {
            u = 1.0f;  // step voltage
        }
        // Simple integrator plant: dy/dt = b0*u  (f=0)
        y += p.dt * (p.b0 * u);
        eso.updateObserver(y, u);
    }
    EXPECT_NEAR(eso.z1(), y, 0.2f);
    EXPECT_NEAR(eso.z2(), 0.0f, 500.0f);  // disturbance near 0 for matched plant
}

TEST(AdrcMpccHybrid, ProducesValidSwitchingState) {
    HybridParams hp;
    hp.motor = simulation::makeGen6BenchMotor();
    hp.ts = 200e-6f;
    hp.omega_o = 2500.0f;
    AdrcMpccHybrid ctrl(hp);
    ctrl.reset();

    HybridInputs in;
    in.id = 0.0f;
    in.iq = 0.0f;
    in.id_ref = 0.0f;
    in.iq_ref = 3.0f;
    in.theta_e = 0.1f;
    in.vdc = 48.0f;
    const auto out = ctrl.update(in);
    EXPECT_TRUE(out.valid);
    EXPECT_GE(static_cast<int>(out.switching_state), 0);
    EXPECT_LE(static_cast<int>(out.switching_state), 7);
    EXPECT_GE(out.duty_active, 0.0f);
    EXPECT_LE(out.duty_active, 1.0f);
}

TEST(AdrcMpccHybrid, RejectsBadVdc) {
    HybridParams hp;
    hp.motor = simulation::makeGen6BenchMotor();
    AdrcMpccHybrid ctrl(hp);
    HybridInputs in;
    in.vdc = 0.0f;
    in.iq_ref = 1.0f;
    const auto out = ctrl.update(in);
    EXPECT_FALSE(out.valid);
}
