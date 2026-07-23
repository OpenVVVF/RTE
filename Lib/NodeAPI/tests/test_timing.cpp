#include <NodeAPI/NodeAPI.h>

#include <gtest/gtest.h>

using namespace NodeAPI;
using NodeAPI::Timing::Validator;

namespace {

NodeType MakeValueType() {
    return NodeType{
        .id = "constant.value",
        .inputPorts = {},
        .outputPorts = {Port{.name = "out",
                             .direction = PortDirection::Output,
                             .type = WireType{.quantity = Quantity::Dimensionless,
                                              .frame = Frame::Scalar,
                                              .dtype = DType::F32}}},
    };
}

NodeType MakeDisplayType() {
    return NodeType{
        .id = "display.value",
        .inputPorts = {Port{.name = "in",
                            .direction = PortDirection::Input,
                            .type = WireType{.quantity = Quantity::Dimensionless,
                                             .frame = Frame::Scalar,
                                             .dtype = DType::F32}}},
        .outputPorts = {},
    };
}

Graph MakeGraphWithDomain(const std::string& domain) {
    Graph graph;
    graph.AddNodeType(MakeValueType());
    graph.AddNodeType(MakeDisplayType());
    graph.AddNode(Node{.id = "source", .type = "constant.value", .domain = domain});
    graph.AddNode(Node{.id = "sink", .type = "display.value", .domain = domain});
    graph.Connect(Connection{
        .id = "c1",
        .from = PortRef{.nodeId = "source", .portName = "out"},
        .to = PortRef{.nodeId = "sink", .portName = "in"},
    });
    return graph;
}

}  // namespace

TEST(Timing, ValidSingleDomainPasses) {
    Graph graph = MakeGraphWithDomain("app_loop");
    Validator validator;
    const auto result = validator.Validate(graph);
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(result.errors.empty());
}

TEST(Timing, MissingDomainFails) {
    Graph graph;
    graph.AddNodeType(MakeValueType());
    graph.AddNodeType(MakeDisplayType());
    graph.AddNode(Node{.id = "source", .type = "constant.value", .domain = ""});
    graph.AddNode(Node{.id = "sink", .type = "display.value", .domain = "app_loop"});

    Validator validator;
    const auto result = validator.Validate(graph);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errors.size(), 1u);
    EXPECT_NE(result.errors[0].find("has no timing domain"), std::string::npos);
}

TEST(Timing, CrossDomainConnectionFails) {
    Graph graph;
    graph.AddNodeType(MakeValueType());
    graph.AddNodeType(MakeDisplayType());
    graph.AddNode(Node{.id = "adc", .type = "constant.value", .domain = "adc_sample"});
    graph.AddNode(Node{.id = "app", .type = "display.value", .domain = "app_loop"});
    graph.Connect(Connection{
        .id = "c1",
        .from = PortRef{.nodeId = "adc", .portName = "out"},
        .to = PortRef{.nodeId = "app", .portName = "in"},
    });

    Validator validator;
    const auto result = validator.Validate(graph);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errors.size(), 1u);
    EXPECT_NE(result.errors[0].find("cross-timestep connections are not allowed"), std::string::npos);
}

TEST(Timing, CycleFails) {
    const WireType scalar = WireType{.quantity = Quantity::Dimensionless,
                                     .frame = Frame::Scalar,
                                     .dtype = DType::F32};
    NodeType passthrough{
        .id = "passthrough",
        .inputPorts = {Port{.name = "in", .direction = PortDirection::Input, .type = scalar}},
        .outputPorts = {Port{.name = "out", .direction = PortDirection::Output, .type = scalar}},
    };

    Graph graph;
    graph.AddNodeType(passthrough);
    graph.AddNode(Node{.id = "a", .type = "passthrough", .domain = "app_loop"});
    graph.AddNode(Node{.id = "b", .type = "passthrough", .domain = "app_loop"});
    graph.AddNode(Node{.id = "c", .type = "passthrough", .domain = "app_loop"});

    graph.Connect(Connection{
        .id = "e1",
        .from = PortRef{.nodeId = "a", .portName = "out"},
        .to = PortRef{.nodeId = "b", .portName = "in"},
    });
    graph.Connect(Connection{
        .id = "e2",
        .from = PortRef{.nodeId = "b", .portName = "out"},
        .to = PortRef{.nodeId = "c", .portName = "in"},
    });
    graph.Connect(Connection{
        .id = "e3",
        .from = PortRef{.nodeId = "c", .portName = "out"},
        .to = PortRef{.nodeId = "a", .portName = "in"},
    });

    Validator validator;
    const auto result = validator.Validate(graph);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errors.size(), 1u);
    EXPECT_NE(result.errors[0].find("directed cycle"), std::string::npos);
}

TEST(Timing, EntryPointCannotHaveIncomingConnection) {
    const WireType scalar = WireType{.quantity = Quantity::Dimensionless,
                                     .frame = Frame::Scalar,
                                     .dtype = DType::F32};
    NodeType entryPoint{
        .id = "adc.trigger",
        .inputPorts = {Port{.name = "reset", .direction = PortDirection::Input, .type = scalar}},
        .outputPorts = {Port{.name = "out", .direction = PortDirection::Output, .type = scalar}},
        .isEntryPoint = true,
    };
    NodeType consumer{
        .id = "consumer",
        .inputPorts = {Port{.name = "in", .direction = PortDirection::Input, .type = scalar}},
        .outputPorts = {Port{.name = "out", .direction = PortDirection::Output, .type = scalar}},
    };

    Graph graph;
    graph.AddNodeType(entryPoint);
    graph.AddNodeType(consumer);
    graph.AddNode(Node{.id = "adc", .type = "adc.trigger", .domain = "adc_sample"});
    graph.AddNode(Node{.id = "proc", .type = "consumer", .domain = "adc_sample"});

    // ADC entry point feeding the consumer is fine.
    graph.Connect(Connection{
        .id = "e1",
        .from = PortRef{.nodeId = "adc", .portName = "out"},
        .to = PortRef{.nodeId = "proc", .portName = "in"},
    });

    Validator validator;
    EXPECT_TRUE(validator.Validate(graph).ok);

    // Connecting back to the entry point's input is not allowed.
    graph.Connect(Connection{
        .id = "e2",
        .from = PortRef{.nodeId = "proc", .portName = "out"},
        .to = PortRef{.nodeId = "adc", .portName = "reset"},
    });

    const auto result = validator.Validate(graph);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errors.size(), 1u);
    EXPECT_NE(result.errors[0].find("entry-point node 'adc'"), std::string::npos);
}

TEST(Timing, CrossDomainBridgeAllowed) {
    Graph graph;
    graph.AddNodeType(MakeValueType());
    graph.AddNodeType(MakeDisplayType());
    graph.AddNode(Node{.id = "adc", .type = "constant.value", .domain = "adc_sample"});
    graph.AddNode(Node{.id = "app", .type = "display.value", .domain = "app_loop"});

    const WireType scalar = WireType{.quantity = Quantity::Dimensionless,
                                     .frame = Frame::Scalar,
                                     .dtype = DType::F32};
    graph.AddBridge(Bridge{
        .id = "b1",
        .type = scalar,
        .producer = PortRef{.nodeId = "adc", .portName = "out"},
        .consumer = PortRef{.nodeId = "app", .portName = "in"},
    });

    Validator validator;
    EXPECT_TRUE(validator.Validate(graph).ok);
}

TEST(Timing, SameDomainBridgeRejected) {
    Graph graph;
    graph.AddNodeType(MakeValueType());
    graph.AddNodeType(MakeDisplayType());
    graph.AddNode(Node{.id = "source", .type = "constant.value", .domain = "app_loop"});
    graph.AddNode(Node{.id = "sink", .type = "display.value", .domain = "app_loop"});

    const WireType scalar = WireType{.quantity = Quantity::Dimensionless,
                                     .frame = Frame::Scalar,
                                     .dtype = DType::F32};
    graph.AddBridge(Bridge{
        .id = "b1",
        .type = scalar,
        .producer = PortRef{.nodeId = "source", .portName = "out"},
        .consumer = PortRef{.nodeId = "sink", .portName = "in"},
    });

    Validator validator;
    const auto result = validator.Validate(graph);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errors.size(), 1u);
    EXPECT_NE(result.errors[0].find("use a connection for same-domain links"), std::string::npos);
}

TEST(Timing, EntryPointCannotBeBridgeConsumer) {
    const WireType scalar = WireType{.quantity = Quantity::Dimensionless,
                                     .frame = Frame::Scalar,
                                     .dtype = DType::F32};
    NodeType entryPoint{
        .id = "adc.trigger",
        .inputPorts = {Port{.name = "reset", .direction = PortDirection::Input, .type = scalar}},
        .outputPorts = {Port{.name = "out", .direction = PortDirection::Output, .type = scalar}},
        .isEntryPoint = true,
    };
    NodeType producer{
        .id = "producer",
        .inputPorts = {},
        .outputPorts = {Port{.name = "out", .direction = PortDirection::Output, .type = scalar}},
    };

    Graph graph;
    graph.AddNodeType(entryPoint);
    graph.AddNodeType(producer);
    graph.AddNode(Node{.id = "src", .type = "producer", .domain = "app_loop"});
    graph.AddNode(Node{.id = "adc", .type = "adc.trigger", .domain = "adc_sample"});
    graph.AddBridge(Bridge{
        .id = "b1",
        .type = scalar,
        .producer = PortRef{.nodeId = "src", .portName = "out"},
        .consumer = PortRef{.nodeId = "adc", .portName = "reset"},
    });

    Validator validator;
    const auto result = validator.Validate(graph);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errors.size(), 1u);
    EXPECT_NE(result.errors[0].find("entry-point node 'adc'"), std::string::npos);
}
