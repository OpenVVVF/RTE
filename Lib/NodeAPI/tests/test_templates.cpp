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

    const auto type = graph.FindNodeType("hw.adc.phase_currents");
    ASSERT_TRUE(type.has_value());
    EXPECT_EQ(type->displayName, "Phase Currents (ISR)");
    EXPECT_TRUE(type->isEntryPoint);
    EXPECT_EQ(type->maxInstances, 1u);
    EXPECT_EQ(type->inputPorts.size(), 0u);
    EXPECT_EQ(type->outputPorts.size(), 4u);

    const auto iu = type->FindOutputPort("iu_a");
    ASSERT_TRUE(iu.has_value());
    EXPECT_EQ(iu->type.quantity, Quantity::Current);
    EXPECT_EQ(iu->type.frame, Frame::Scalar);
    EXPECT_EQ(iu->type.dtype, DType::F32);

    EXPECT_TRUE(type->FindOutputPort("iv_a").has_value());
    EXPECT_TRUE(type->FindOutputPort("iw_measured_a").has_value());
    EXPECT_TRUE(type->FindOutputPort("iw_calculated_a").has_value());

    EXPECT_FALSE(type->inlineCode.empty());
    EXPECT_NE(type->inlineCode.find("ADC_VREF"), std::string::npos);
    EXPECT_NE(type->inlineCode.find("iw_calculated_a = -(iu_a + iv_a)"), std::string::npos);
}

TEST(NodeTemplates, RejectsMissingDirectory) {
    Graph graph;
    const auto result = LoadNodeTypesFromDirectory(graph, "/does/not/exist");
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.filesLoaded, 0u);
    EXPECT_EQ(result.typesLoaded, 0u);
    EXPECT_EQ(result.errors.size(), 1u);
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
