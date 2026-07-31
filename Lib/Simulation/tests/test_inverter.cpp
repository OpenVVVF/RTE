#include <cmath>
#include <gtest/gtest.h>

#include "simulation/TwoLevelInverter.h"

using namespace simulation;

namespace {

constexpr float kVdc = 540.0f;
constexpr float kActiveMag = (2.0f / 3.0f) * kVdc;
constexpr float kTol = 1e-3f;

TEST(TwoLevelInverter, NullStatesProduceZeroVoltage) {
    for (const auto state : {SwitchingState::S000, SwitchingState::S111}) {
        const auto v = voltageAlphaBetaFromState(kVdc, state);
        EXPECT_NEAR(v.alpha, 0.0f, kTol);
        EXPECT_NEAR(v.beta, 0.0f, kTol);
    }
}

TEST(TwoLevelInverter, ActiveVectorsEqualMagnitude) {
    const std::array<SwitchingState, 6> active = {SwitchingState::S100, SwitchingState::S110, SwitchingState::S010,
                                                  SwitchingState::S011, SwitchingState::S001, SwitchingState::S101};
    for (const auto state : active) {
        const auto v = voltageAlphaBetaFromState(kVdc, state);
        const float mag = std::hypot(v.alpha, v.beta);
        EXPECT_NEAR(mag, kActiveMag, 1e-2f) << "state=" << static_cast<int>(state);
    }
}

TEST(TwoLevelInverter, AdjacentVectorsSeparatedBy60Degrees) {
    const std::array<SwitchingState, 6> active = {SwitchingState::S100, SwitchingState::S110, SwitchingState::S010,
                                                  SwitchingState::S011, SwitchingState::S001, SwitchingState::S101};
    for (std::size_t i = 0; i < active.size(); ++i) {
        const auto v0 = voltageAlphaBetaFromState(kVdc, active[i]);
        const auto v1 = voltageAlphaBetaFromState(kVdc, active[(i + 1) % active.size()]);
        const float a0 = std::atan2(v0.beta, v0.alpha);
        const float a1 = std::atan2(v1.beta, v1.alpha);
        float delta = a1 - a0;
        if (delta < 0.0f) {
            delta += kTwoPi;
        }
        EXPECT_NEAR(delta, kPi / 3.0f, 0.05f);
    }
}

TEST(TwoLevelInverter, OppositeVectorsAreOpposite) {
    const auto v1 = voltageAlphaBetaFromState(kVdc, SwitchingState::S100);
    const auto v4 = voltageAlphaBetaFromState(kVdc, SwitchingState::S011);
    EXPECT_NEAR(v1.alpha, -v4.alpha, 1e-2f);
    EXPECT_NEAR(v1.beta, -v4.beta, 1e-2f);
}

TEST(TwoLevelInverter, LineToLineConsistentWithSwitchStates) {
    const auto cmd = switchingStateToCommand(SwitchingState::S100);
    const float vab = kVdc * (static_cast<float>(cmd.sa) - static_cast<float>(cmd.sb));
    const float vbc = kVdc * (static_cast<float>(cmd.sb) - static_cast<float>(cmd.sc));
    const AlphaBeta v = voltageAlphaBetaFromState(kVdc, SwitchingState::S100);
    const float reconstructed_vab = 1.5f * v.alpha;
    EXPECT_NEAR(reconstructed_vab, vab, 1e-2f);
    (void)vbc;
}

TEST(TwoLevelInverter, AllEightStatesValid) {
    for (int i = 0; i < 8; ++i) {
        EXPECT_TRUE(isValidSwitchingState(static_cast<SwitchingState>(i)));
    }
}

TEST(TwoLevelInverter, CompareWithRteSvpwmLinearLimit) {
    // RTE math.svpwm uses Vdc/sqrt(3) as max line-neutral magnitude; active vectors here are 2/3 Vdc.
    const float rte_limit = kVdc / kSqrt3;
    const auto v = voltageAlphaBetaFromState(kVdc, SwitchingState::S100);
    const float mag = std::hypot(v.alpha, v.beta);
    EXPECT_GT(mag, rte_limit);
    EXPECT_NEAR(mag, kActiveMag, 1e-2f);
}

}  // namespace
