#include <InverterCodegen/RteQuantity.h>
#include <NodeAPI/NodeAPI.h>
#include <gtest/gtest.h>
#include <math.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>

namespace {
struct Settings {
    float drive = 50.0f;
    float regen = 10.0f;
    float neutral = 0.2f;
    float neutral_rpm = 1000.0f;
    float start_rpm = 50.0f;
    float regen_rpm = 300.0f;
};
struct Result { float iq, neutral, regen; };

// Compile the actual node body with the same quantity types used by codegen.
Result map(float A, float RpmMech, float RpmElec, bool Valid = true,
           Settings settings = {}) {
    const auto MaxAmps = rte::Amperes(settings.drive);
    const auto RegenAmps = rte::Amperes(settings.regen);
    const float NeutralMax = settings.neutral;
    const float NeutralFullRpm = settings.neutral_rpm;
    const float RegenStartRpm = settings.start_rpm;
    const float RegenFullRpm = settings.regen_rpm;
    auto Iq = rte::Amperes(123.0f);
    float Neutral = 123.0f;
    auto RegenAvailable = rte::Amperes(123.0f);
#include "../../../Assets/NodeTemplates/Control.BlendedThrottle/inline.cpp"
    return {Iq.in(au::amperes), Neutral, RegenAvailable.in(au::amperes)};
}
} // namespace

TEST(BlendedThrottle, StandstillHasNoBrakingOrCreepAndKeepsFullDriveRange) {
    for (float rpm : {-50.0f, -1.0f, 0.0f, 1.0f, 50.0f}) {
        EXPECT_FLOAT_EQ(map(0.0f, rpm, rpm * 5).iq, 0.0f);
        EXPECT_FLOAT_EQ(map(0.0f, rpm, rpm * 5).neutral, 0.0f);
        EXPECT_FLOAT_EQ(map(0.0f, rpm, rpm * 5).regen, 0.0f);
        EXPECT_FLOAT_EQ(map(0.5f, rpm, rpm * 5).iq, 25.0f);
        EXPECT_FLOAT_EQ(map(1.0f, rpm, rpm * 5).iq, 50.0f);
    }
}

TEST(BlendedThrottle, SpeedMovesNeutralAndFadesRegenByMagnitude) {
    for (float sign : {-1.0f, 1.0f}) {
        EXPECT_NEAR(map(0, sign * 175, sign * 875).regen, 5, 1e-5f);
        EXPECT_NEAR(map(0, sign * 175, sign * 875).iq, -sign * 5, 1e-5f);
        EXPECT_NEAR(map(0, sign * 525, sign * 2625).neutral, 0.1f, 1e-6f);
        EXPECT_FLOAT_EQ(map(0, sign * 1000, sign * 5000).neutral, 0.2f);
        EXPECT_FLOAT_EQ(map(0, sign * 5000, sign * 25000).neutral, 0.2f);
        EXPECT_FLOAT_EQ(map(0, sign * 5000, sign * 25000).regen, 10);
    }
}

TEST(BlendedThrottle, PedalBlendsContinuouslyAndNeverReversesDriveDirection) {
    EXPECT_FLOAT_EQ(map(0, 1000, 5000).iq, -10);
    EXPECT_FLOAT_EQ(map(0.1f, 1000, 5000).iq, -5);
    EXPECT_FLOAT_EQ(map(0.2f, 1000, 5000).iq, 0);
    EXPECT_NEAR(map(0.6f, 1000, 5000).iq, 25, 1e-5f);
    EXPECT_FLOAT_EQ(map(1, 1000, 5000).iq, 50);
    // Electrical direction can differ from raw mechanical encoder direction.
    EXPECT_FLOAT_EQ(map(0, 1000, -5000).iq, 10);
    EXPECT_FLOAT_EQ(map(1, -1000, -5000).iq, 50);
    EXPECT_FLOAT_EQ(map(0, 1000, 0).iq, 0);
    for (float rpm : {51.0f, 175.0f, 300.0f, 525.0f, 1000.0f, 5000.0f}) {
        const float neutral = map(0, rpm, rpm * 5).neutral;
        EXPECT_FLOAT_EQ(map(neutral, rpm, rpm * 5).iq, 0);
        EXPECT_NEAR(map(neutral - 1e-7f, rpm, rpm * 5).iq, 0, 0.001f);
        EXPECT_NEAR(map(neutral + 1e-7f, rpm, rpm * 5).iq, 0, 0.001f);
    }
}

TEST(BlendedThrottle, InputBoundsAndConfigChangesApplyOnTheSameStep) {
    EXPECT_FLOAT_EQ(map(-1, 1000, 5000).iq, -10);
    EXPECT_FLOAT_EQ(map(2, 1000, 5000).iq, 50);
    Settings settings;
    settings.drive = 30;
    settings.regen = 4;
    settings.neutral = 0.3f;
    EXPECT_FLOAT_EQ(map(1, 1000, 5000, true, settings).iq, 30);
    EXPECT_FLOAT_EQ(map(0, 1000, 5000, true, settings).iq, -4);
    EXPECT_FLOAT_EQ(map(0.3f, 1000, 5000, true, settings).iq, 0);
    settings.regen = 0;
    EXPECT_FLOAT_EQ(map(0, 1000, 5000, true, settings).iq, 0);
    settings.neutral = 0;
    EXPECT_FLOAT_EQ(map(0.5f, 1000, 5000, true, settings).iq, 15);
    // Repeated calls have no accumulated ramp state.
    for (int i = 0; i < 20; ++i) {
        EXPECT_FLOAT_EQ(map(1, 1000, 5000).iq, 50);
        EXPECT_FLOAT_EQ(map(0, 1000, 5000).iq, -10);
    }
}

TEST(BlendedThrottle, InvalidInputsAndSettingsRequestZero) {
    const auto zero = [](Result r) {
        EXPECT_FLOAT_EQ(r.iq, 0);
        EXPECT_FLOAT_EQ(r.neutral, 0);
        EXPECT_FLOAT_EQ(r.regen, 0);
    };
    zero(map(1, 1000, 5000, false));
    for (float bad : {std::numeric_limits<float>::quiet_NaN(),
                      std::numeric_limits<float>::infinity(),
                      -std::numeric_limits<float>::infinity()}) {
        zero(map(bad, 1000, 5000));
        zero(map(1, bad, 5000));
        zero(map(1, 1000, bad));
        for (auto member : std::array{&Settings::drive, &Settings::regen,
                 &Settings::neutral, &Settings::neutral_rpm,
                 &Settings::start_rpm, &Settings::regen_rpm}) {
            Settings s;
            s.*member = bad;
            zero(map(1, 1000, 5000, true, s));
        }
    }
    for (auto member : std::array{&Settings::drive, &Settings::regen,
                                  &Settings::neutral, &Settings::start_rpm}) {
        Settings s;
        s.*member = -1;
        zero(map(1, 1000, 5000, true, s));
    }
    Settings s;
    s.neutral = 1;
    zero(map(1, 1000, 5000, true, s));
    s = {};
    s.neutral_rpm = s.start_rpm;
    zero(map(1, 1000, 5000, true, s));
    s = {};
    s.regen_rpm = s.start_rpm - 1;
    zero(map(1, 1000, 5000, true, s));
}

TEST(BlendedThrottle, ForwardCurveIsMonotonicBoundedAndRegenOpposesRotation) {
    for (float rpm : {-5000.0f, -1000.0f, -175.0f, -50.0f, 0.0f,
                       50.0f, 175.0f, 1000.0f, 5000.0f}) {
        float previous = -10;
        for (int i = 0; i <= 1000; ++i) {
            const float pedal = float(i) / 1000;
            const auto r = map(pedal, rpm, rpm * 5);
            EXPECT_GE(r.iq, -10);
            EXPECT_LE(r.iq, 50);
            if (pedal < r.neutral) { EXPECT_LE(r.iq * rpm, 0); }
            if (rpm >= 0) { EXPECT_GE(r.iq, previous); }
            previous = r.iq;
        }
    }
}

TEST(BlendedThrottle, DemoEmbedsTestedNodeAndUsesExplicitSpeedBridges) {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path()
        .parent_path().parent_path();
    std::ifstream file(root / "Assets/Examples/foc_demo.json");
    ASSERT_TRUE(file.is_open());
    const auto graph = NodeAPI::LoadFromJson(std::string(
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()));
    NodeAPI::Graph templates;
    const auto loaded = NodeAPI::LoadNodeTypesFromDirectory(
        templates, root / "Assets/NodeTemplates");
    ASSERT_TRUE(loaded.ok);
    const auto expected = templates.FindNodeType("Control.BlendedThrottle");
    const auto actual = graph.FindNodeType("Control.BlendedThrottle");
    ASSERT_TRUE(expected);
    ASSERT_TRUE(actual);
    EXPECT_EQ(actual->inlineCode, expected->inlineCode);
    EXPECT_EQ(actual->inputPorts.size(), expected->inputPorts.size());
    EXPECT_EQ(actual->parameterTypes.size(), expected->parameterTypes.size());
    bool mech = false, elec = false, iq = false;
    for (const auto& bridge : graph.GetBridges()) {
        if (bridge.consumer.nodeId == "ThrottleIq") {
            EXPECT_EQ(bridge.producer.nodeId, "ControlSpeed");
            EXPECT_EQ(bridge.producer.portName, bridge.consumer.portName);
            mech |= bridge.consumer.portName == "RpmMech";
            elec |= bridge.consumer.portName == "RpmElec";
        }
        if (bridge.producer.nodeId == "ThrottleIq") {
            EXPECT_EQ(bridge.consumer.nodeId, "IqGate");
            EXPECT_EQ(bridge.consumer.portName, "Val");
            iq = true;
        }
    }
    EXPECT_TRUE(mech && elec && iq);
    for (const auto& node : graph.GetNodes()) {
        EXPECT_NE(node.id, "SlewIq");
        if (node.id == "ThrottleIq") {
            EXPECT_EQ(node.type, "Control.BlendedThrottle");
            EXPECT_EQ(node.parameterInputs.size(), 6u);
        }
    }
}
