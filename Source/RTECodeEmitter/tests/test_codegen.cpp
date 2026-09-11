#include <InverterCodegen/CodeGenerator.h>
#include <NodeAPI/NodeAPI.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

using NodeAPI::Bridge;
using NodeAPI::DType;
using NodeAPI::Frame;
using NodeAPI::Graph;
using NodeAPI::Node;
using NodeAPI::NodeType;
using NodeAPI::Port;
using NodeAPI::PortDirection;
using NodeAPI::PortRef;
using NodeAPI::Quantity;
using NodeAPI::WireType;

const WireType kScalarDimensionless{.quantity = Quantity::Dimensionless,
                                    .frame = Frame::Scalar,
                                    .dtype = DType::F32};

NodeType ConstantType() {
    return NodeType{
        .id = "test.constant",
        .displayName = "Constant",
        .outputPorts = {Port{.name = "out",
                             .direction = PortDirection::Output,
                             .type = kScalarDimensionless}},
        .inlineCode = "out = 1.0f;",
    };
}

NodeType GainType() {
    return NodeType{
        .id = "test.gain",
        .displayName = "Gain",
        .inputPorts = {Port{.name = "in",
                            .direction = PortDirection::Input,
                            .type = kScalarDimensionless}},
        .outputPorts = {Port{.name = "out",
                             .direction = PortDirection::Output,
                             .type = kScalarDimensionless}},
        .parameterTypes = {{"gain", kScalarDimensionless}},
        .inlineCode = "out = gain * in;",
    };
}

/* Graph with one producer node in "app_loop" and one consumer node in
 * "tim_isr"; bridges added by the individual tests. */
Graph MakeBridgeGraph() {
    Graph graph;
    graph.AddNodeType(ConstantType());
    graph.AddNodeType(GainType());
    graph.AddNode(Node{.id = "src", .type = "test.constant", .domain = "app_loop"});
    graph.AddNode(Node{.id = "dst",
                       .type = "test.gain",
                       .domain = "tim_isr",
                       .parameters = {{"gain", "1.0"}}});
    return graph;
}

Bridge MakeBridge(std::string id) {
    return Bridge{
        .id = std::move(id),
        .type = kScalarDimensionless,
        .producer = PortRef{.nodeId = "src", .portName = "out"},
        .consumer = PortRef{.nodeId = "dst", .portName = "in"},
    };
}

std::string ReadFileText(const std::filesystem::path& path) {
    std::ifstream file(path);
    return std::string((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
}

size_t CountOccurrences(const std::string& haystack, const std::string& needle) {
    size_t count = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

}  // namespace

TEST(CodeGenerator, RejectsUnusableBridgeId) {
    // "weird.id" would leak a '.' through Capitalize() into the C++ symbol.
    Graph graph = MakeBridgeGraph();
    ASSERT_TRUE(graph.AddBridge(MakeBridge("weird.id")));

    const auto outDir = std::filesystem::temp_directory_path() / "rte_codegen_bad_bridge_id";
    std::filesystem::remove_all(outDir);
    std::string error;
    InverterCodegen::CodeGenerator generator(graph);
    EXPECT_FALSE(generator.Generate(outDir.string(), error));
    EXPECT_NE(error.find("weird.id"), std::string::npos) << error;
    std::filesystem::remove_all(outDir);
}

TEST(CodeGenerator, RejectsCollidingBridgeIds) {
    // "my-bridge" and "my_bridge" both capitalize to "BridgeMyBridge".
    Graph graph = MakeBridgeGraph();
    graph.AddNode(Node{.id = "src2", .type = "test.constant", .domain = "app_loop"});
    graph.AddNode(Node{.id = "dst2",
                       .type = "test.gain",
                       .domain = "tim_isr",
                       .parameters = {{"gain", "1.0"}}});
    ASSERT_TRUE(graph.AddBridge(MakeBridge("my-bridge")));
    ASSERT_TRUE(graph.AddBridge(Bridge{
        .id = "my_bridge",
        .type = kScalarDimensionless,
        .producer = PortRef{.nodeId = "src2", .portName = "out"},
        .consumer = PortRef{.nodeId = "dst2", .portName = "in"},
    }));

    const auto outDir = std::filesystem::temp_directory_path() / "rte_codegen_bridge_collision";
    std::filesystem::remove_all(outDir);
    std::string error;
    InverterCodegen::CodeGenerator generator(graph);
    EXPECT_FALSE(generator.Generate(outDir.string(), error));
    EXPECT_NE(error.find("my-bridge"), std::string::npos) << error;
    EXPECT_NE(error.find("my_bridge"), std::string::npos) << error;
    std::filesystem::remove_all(outDir);
}

TEST(CodeGenerator, AcceptsBridgeIdWithWordSeparators) {
    Graph graph = MakeBridgeGraph();
    ASSERT_TRUE(graph.AddBridge(MakeBridge("throttle-cmd")));

    const auto outDir = std::filesystem::temp_directory_path() / "rte_codegen_bridge_ok";
    std::filesystem::remove_all(outDir);
    std::string error;
    InverterCodegen::CodeGenerator generator(graph);
    ASSERT_TRUE(generator.Generate(outDir.string(), error)) << error;
    const std::string header = ReadFileText(outDir / "bridges_generated.h");
    EXPECT_NE(header.find("BridgeThrottleCmd"), std::string::npos);
    std::filesystem::remove_all(outDir);
}

TEST(CodeGenerator, RejectsHexParameterLiteral) {
    // "0x10" must not splice as a hex literal (0x10f compiled as 271 before).
    Graph graph;
    graph.AddNodeType(ConstantType());
    graph.AddNodeType(NodeType{
        .id = "test.sink",
        .displayName = "Sink",
        .parameterTypes = {{"level", kScalarDimensionless}},
        .inlineCode = "(void)level;",
    });
    graph.AddNode(Node{.id = "n",
                       .type = "test.sink",
                       .domain = "app_loop",
                       .parameters = {{"level", "0x10"}}});

    const auto outDir = std::filesystem::temp_directory_path() / "rte_codegen_hex_param";
    std::filesystem::remove_all(outDir);
    std::string error;
    InverterCodegen::CodeGenerator generator(graph);
    EXPECT_FALSE(generator.Generate(outDir.string(), error));
    EXPECT_NE(error.find("0x10"), std::string::npos) << error;
    std::filesystem::remove_all(outDir);
}

TEST(CodeGenerator, RejectsNonNumericParameterLiteral) {
    Graph graph;
    graph.AddNodeType(ConstantType());
    graph.AddNodeType(NodeType{
        .id = "test.sink",
        .displayName = "Sink",
        .parameterTypes = {{"level", kScalarDimensionless}},
        .inlineCode = "(void)level;",
    });
    graph.AddNode(Node{.id = "n",
                       .type = "test.sink",
                       .domain = "app_loop",
                       .parameters = {{"level", "banana"}}});

    const auto outDir = std::filesystem::temp_directory_path() / "rte_codegen_bad_param";
    std::filesystem::remove_all(outDir);
    std::string error;
    InverterCodegen::CodeGenerator generator(graph);
    EXPECT_FALSE(generator.Generate(outDir.string(), error));
    EXPECT_NE(error.find("banana"), std::string::npos) << error;
    std::filesystem::remove_all(outDir);
}

TEST(CodeGenerator, CanonicalizesParameterLiterals) {
    // Integer spellings get a decimal point ("10f" would not compile);
    // explicit inf/nan spellings map to the INFINITY/NAN macros.
    Graph graph;
    graph.AddNodeType(ConstantType());
    graph.AddNodeType(NodeType{
        .id = "test.sink",
        .displayName = "Sink",
        .parameterTypes = {{"int_like", kScalarDimensionless},
                           {"plain", kScalarDimensionless},
                           {"sci", kScalarDimensionless},
                           {"inf_pos", kScalarDimensionless},
                           {"inf_neg", kScalarDimensionless},
                           {"not_a_number", kScalarDimensionless}},
        .inlineCode = "(void)int_like; (void)plain; (void)sci;"
                      "(void)inf_pos; (void)inf_neg; (void)not_a_number;",
    });
    graph.AddNode(Node{.id = "n",
                       .type = "test.sink",
                       .domain = "app_loop",
                       .parameters = {{"int_like", "10"},
                                      {"plain", "0.5"},
                                      {"sci", "1.5e-3"},
                                      {"inf_pos", "inf"},
                                      {"inf_neg", "-INFINITY"},
                                      {"not_a_number", "nan"}}});

    const auto outDir = std::filesystem::temp_directory_path() / "rte_codegen_canonical_params";
    std::filesystem::remove_all(outDir);
    std::string error;
    InverterCodegen::CodeGenerator generator(graph);
    ASSERT_TRUE(generator.Generate(outDir.string(), error)) << error;

    const std::string source = ReadFileText(outDir / "domain_app_loop_generated.cpp");
    EXPECT_NE(source.find("state.n.int_like = 10.0f;"), std::string::npos) << source;
    EXPECT_NE(source.find("state.n.plain = 0.5f;"), std::string::npos) << source;
    EXPECT_NE(source.find("state.n.sci = 1.5e-3f;"), std::string::npos) << source;
    EXPECT_NE(source.find("state.n.inf_pos = INFINITY;"), std::string::npos) << source;
    EXPECT_NE(source.find("state.n.inf_neg = (-INFINITY);"), std::string::npos) << source;
    EXPECT_NE(source.find("state.n.not_a_number = NAN;"), std::string::npos) << source;
    // The generated source provides the macros.
    EXPECT_NE(source.find("#include <math.h>"), std::string::npos) << source;
    std::filesystem::remove_all(outDir);
}

TEST(CodeGenerator, HoistsClassBasedTypesAcrossDomains) {
    // A class-based node type used in two timing domains must emit its class
    // declaration and definition exactly once, not once per domain.
    Graph graph;
    graph.AddNodeType(ConstantType());
    graph.AddNodeType(NodeType{
        .id = "test.helper",
        .displayName = "Helper",
        .inputPorts = {Port{.name = "in",
                            .direction = PortDirection::Input,
                            .type = kScalarDimensionless}},
        .inlineCode = "(void)in; (void)instance;",
        .constructorCode = "(void)instance;",
        .classHeader = "class Helper {\npublic:\n    float Step(float x);\n};",
        .classDefinition = "float Helper::Step(float x) { return x * 2.0f; }",
    });
    graph.AddNode(Node{.id = "src_a", .type = "test.constant", .domain = "app_loop"});
    graph.AddNode(Node{.id = "src_b", .type = "test.constant", .domain = "tim_isr"});
    graph.AddNode(Node{.id = "n_a", .type = "test.helper", .domain = "app_loop"});
    graph.AddNode(Node{.id = "n_b", .type = "test.helper", .domain = "tim_isr"});
    ASSERT_TRUE(graph.Connect(NodeAPI::Connection{
        .id = "c1",
        .from = PortRef{.nodeId = "src_a", .portName = "out"},
        .to = PortRef{.nodeId = "n_a", .portName = "in"},
    }));
    ASSERT_TRUE(graph.Connect(NodeAPI::Connection{
        .id = "c2",
        .from = PortRef{.nodeId = "src_b", .portName = "out"},
        .to = PortRef{.nodeId = "n_b", .portName = "in"},
    }));

    const auto outDir = std::filesystem::temp_directory_path() / "rte_codegen_class_hoist";
    std::filesystem::remove_all(outDir);
    std::string error;
    InverterCodegen::CodeGenerator generator(graph);
    ASSERT_TRUE(generator.Generate(outDir.string(), error)) << error;

    // Shared files exist and carry the class exactly once.
    const std::string classHeader = ReadFileText(outDir / "node_types_generated.h");
    const std::string classSource = ReadFileText(outDir / "node_types_generated.cpp");
    EXPECT_EQ(CountOccurrences(classHeader, "class Helper"), 1u);
    EXPECT_EQ(CountOccurrences(classSource, "Helper::Step"), 1u);

    // Domain headers reference the shared declaration but do not re-emit it.
    for (const auto* domain : {"app_loop", "tim_isr"}) {
        const std::string text =
            ReadFileText(outDir / ("domain_" + std::string(domain) + "_generated.h"));
        EXPECT_NE(text.find("#include \"node_types_generated.h\""), std::string::npos)
            << domain;
        EXPECT_EQ(text.find("class Helper"), std::string::npos) << domain;
        const std::string source =
            ReadFileText(outDir / ("domain_" + std::string(domain) + "_generated.cpp"));
        EXPECT_EQ(source.find("Helper::Step"), std::string::npos) << domain;
    }
    std::filesystem::remove_all(outDir);
}
