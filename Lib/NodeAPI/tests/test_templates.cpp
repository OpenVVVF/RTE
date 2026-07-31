#include <NodeAPI/NodeAPI.h>

#include <gtest/gtest.h>

#include <filesystem>

using namespace NodeAPI;

namespace {

std::filesystem::path GetTemplatesDirectory() {
    const std::filesystem::path thisFile = __FILE__;
    return thisFile.parent_path().parent_path() / ".." / ".." / "Assets" / "NodeTemplates";
}

}  // namespace

TEST(NodeTemplates, LoadsPhaseCurrentsTemplate) {
    Graph graph;
    const auto result = LoadNodeTypesFromDirectory(graph, GetTemplatesDirectory());

    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.errors.size(), 0u);
    EXPECT_GE(result.filesLoaded, 1u);
    EXPECT_GE(result.typesLoaded, 1u);

    const auto type = graph.FindNodeType("Sensors.PhaseCurrents");
    ASSERT_TRUE(type.has_value());
    EXPECT_EQ(type->displayName, "Phase Currents");
    EXPECT_EQ(type->domain, "");
    EXPECT_EQ(type->inputPorts.size(), 0u);
    EXPECT_EQ(type->outputPorts.size(), 3u);

    const auto ia = type->FindOutputPort("I_A");
    ASSERT_TRUE(ia.has_value());
    EXPECT_EQ(ia->type.quantity, Quantity::Current);
    EXPECT_EQ(ia->type.frame, Frame::Scalar);
    EXPECT_EQ(ia->type.dtype, DType::F32);

    const auto ib = type->FindOutputPort("I_B");
    ASSERT_TRUE(ib.has_value());
    EXPECT_EQ(ib->type.quantity, Quantity::Current);
    EXPECT_EQ(ib->type.frame, Frame::Scalar);
    EXPECT_EQ(ib->type.dtype, DType::F32);

    const auto ic = type->FindOutputPort("I_C");
    ASSERT_TRUE(ic.has_value());
    EXPECT_EQ(ic->type.quantity, Quantity::Current);
    EXPECT_EQ(ic->type.frame, Frame::Scalar);
    EXPECT_EQ(ic->type.dtype, DType::F32);

    const auto invertPolarity = type->FindParameterType("InvertPolarity");
    ASSERT_TRUE(invertPolarity.has_value());
    EXPECT_EQ(invertPolarity->quantity, Quantity::Boolean);

    EXPECT_FALSE(type->inlineCode.empty());
    EXPECT_NE(type->inlineCode.find("platform_get_phase_currents"), std::string::npos);
    EXPECT_NE(type->inlineCode.find("InvertPolarity"), std::string::npos);
}

TEST(NodeTemplates, LoadsCodeFromSeparateFiles) {
    Graph graph;
    const auto result = LoadNodeTypesFromDirectory(graph, GetTemplatesDirectory());
    ASSERT_TRUE(result.ok);

    const auto clarke = graph.FindNodeType("Transforms.Clarke");
    ASSERT_TRUE(clarke.has_value());
    EXPECT_NE(clarke->inlineCode.find("I_Alpha = I_A;"), std::string::npos);
}

TEST(NodeTemplates, AllShippedMetadataHasDescriptions) {
    Graph graph;
    const auto result =
        LoadNodeTypesFromDirectory(graph, GetTemplatesDirectory());
    ASSERT_TRUE(result.ok);

    for (const auto& type : graph.GetNodeTypes()) {
        SCOPED_TRACE(type.id);
        EXPECT_FALSE(type.description.empty());
        for (const auto& port : type.inputPorts) {
            SCOPED_TRACE("input port: " + port.name);
            EXPECT_FALSE(port.description.empty());
        }
        for (const auto& port : type.outputPorts) {
            SCOPED_TRACE("output port: " + port.name);
            EXPECT_FALSE(port.description.empty());
        }
        for (const auto& [name, wireType] : type.parameterTypes) {
            (void)wireType;
            SCOPED_TRACE("property: " + name);
            const auto description =
                type.FindParameterDescription(name);
            ASSERT_TRUE(description.has_value());
            EXPECT_FALSE(description->empty());
        }
    }
}

TEST(NodeTemplates, RejectsMissingDirectory) {
    Graph graph;
    const auto result = LoadNodeTypesFromDirectory(graph, "/does/not/exist");
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.filesLoaded, 0u);
    EXPECT_EQ(result.typesLoaded, 0u);
    EXPECT_EQ(result.errors.size(), 1u);
}

TEST(NodeTemplates, ForcedDomainOverridesInstanceDomain) {
    Graph graph;
    const auto result = LoadNodeTypesFromDirectory(graph, GetTemplatesDirectory());
    ASSERT_TRUE(result.ok);

    // Try to place it in the wrong domain.
    ASSERT_TRUE(graph.AddNode(Node{
        .id = "pwm",
        .type = "Actuators.PwmOut",
        .domain = "app_loop",
    }));

    const auto node = graph.FindNode("pwm");
    ASSERT_TRUE(node.has_value());
    EXPECT_EQ(node->domain, "tim_isr");
}

TEST(NodeTemplates, LoadsControlBlocks) {
    Graph graph;
    const auto result = LoadNodeTypesFromDirectory(graph, GetTemplatesDirectory());
    ASSERT_TRUE(result.ok);

    const auto pi = graph.FindNodeType("Control.Pi");
    ASSERT_TRUE(pi.has_value());
    EXPECT_EQ(pi->inputPorts.size(), 2u);
    EXPECT_EQ(pi->outputPorts.size(), 1u);
    EXPECT_NE(pi->inlineCode.find("Integral += error"), std::string::npos);

    const auto clarke = graph.FindNodeType("Transforms.Clarke");
    ASSERT_TRUE(clarke.has_value());
    EXPECT_EQ(clarke->inputPorts.size(), 3u);
    EXPECT_EQ(clarke->outputPorts.size(), 2u);
    EXPECT_EQ(clarke->FindInputPort("I_A")->type.frame, Frame::Scalar);
    EXPECT_EQ(clarke->FindInputPort("I_B")->type.frame, Frame::Scalar);
    EXPECT_EQ(clarke->FindInputPort("I_C")->type.frame, Frame::Scalar);
    EXPECT_EQ(clarke->FindOutputPort("I_Alpha")->type.frame, Frame::Scalar);
    EXPECT_EQ(clarke->FindOutputPort("I_Beta")->type.frame, Frame::Scalar);

    const auto park = graph.FindNodeType("Transforms.Park");
    ASSERT_TRUE(park.has_value());
    EXPECT_EQ(park->inputPorts.size(), 3u);
    EXPECT_EQ(park->outputPorts.size(), 2u);
    EXPECT_EQ(park->FindInputPort("I_Alpha")->type.frame, Frame::Scalar);
    EXPECT_EQ(park->FindInputPort("I_Beta")->type.frame, Frame::Scalar);
    EXPECT_EQ(park->FindOutputPort("I_D")->type.frame, Frame::Scalar);
    EXPECT_EQ(park->FindOutputPort("I_Q")->type.frame, Frame::Scalar);

    const auto svpwm = graph.FindNodeType("Transforms.Svpwm");
    ASSERT_TRUE(svpwm.has_value());
    EXPECT_EQ(svpwm->inputPorts.size(), 3u);
    EXPECT_EQ(svpwm->outputPorts.size(), 3u);

    const auto sincos = graph.FindNodeType("Transforms.SinCos");
    ASSERT_TRUE(sincos.has_value());
    EXPECT_EQ(sincos->inputPorts.size(), 1u);
    EXPECT_EQ(sincos->outputPorts.size(), 2u);

    const auto pwm = graph.FindNodeType("Actuators.PwmOut");
    ASSERT_TRUE(pwm.has_value());
    EXPECT_EQ(pwm->inputPorts.size(), 3u);
    EXPECT_TRUE(pwm->outputPorts.empty());
    EXPECT_TRUE(pwm->FindInputPort("Duty_A").has_value());

    const auto encoder = graph.FindNodeType("Sensors.Encoder");
    ASSERT_TRUE(encoder.has_value());
    EXPECT_EQ(encoder->inputPorts.size(), 0u);
    EXPECT_EQ(encoder->outputPorts.size(), 2u);
}

TEST(NodeTemplates, SkipsDuplicateNodeType) {
    Graph graph;
    const auto first = LoadNodeTypesFromDirectory(graph, GetTemplatesDirectory());
    ASSERT_TRUE(first.ok);
    ASSERT_GE(first.typesLoaded, 1u);

    const auto second = LoadNodeTypesFromDirectory(graph, GetTemplatesDirectory());
    EXPECT_FALSE(second.ok);
    EXPECT_EQ(second.typesLoaded, 0u);
    EXPECT_EQ(second.errors.size(), first.typesLoaded);
}
