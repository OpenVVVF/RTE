#include <NodeAPI/NodeAPI.h>

#include <gtest/gtest.h>

using namespace NodeAPI;

namespace {

NodeType MakeValueType() {
    return NodeType{
        .id = "constant.value",
        .displayName = "Value",
        .inputPorts = {},
        .outputPorts = {Port{.name = "out",
                             .direction = PortDirection::Output,
                             .type = WireType{.quantity = Quantity::Dimensionless,
                                              .frame = Frame::Scalar,
                                              .dtype = DType::F32}}},
        .inlineCode = "return 0.5f;",
    };
}

NodeType MakeVoltageType() {
    return NodeType{
        .id = "constant.voltage",
        .displayName = "Voltage",
        .inputPorts = {},
        .outputPorts = {Port{.name = "out",
                             .direction = PortDirection::Output,
                             .type = WireType{.quantity = Quantity::Voltage,
                                              .frame = Frame::Scalar,
                                              .dtype = DType::F32}}},
        .inlineCode = "return 48.0f;",
    };
}

NodeType MakeDisplayType() {
    return NodeType{
        .id = "display.value",
        .displayName = "Display",
        .inputPorts = {Port{.name = "in",
                            .direction = PortDirection::Input,
                            .type = WireType{.quantity = Quantity::Dimensionless,
                                             .frame = Frame::Scalar,
                                             .dtype = DType::F32}}},
        .outputPorts = {},
    };
}

NodeType MakeCurrentSinkType() {
    return NodeType{
        .id = "load.current",
        .displayName = "Current Sink",
        .inputPorts = {Port{.name = "in",
                            .direction = PortDirection::Input,
                            .type = WireType{.quantity = Quantity::Current,
                                             .frame = Frame::Scalar,
                                             .dtype = DType::F32}}},
        .outputPorts = {},
    };
}

NodeType MakePiControllerType() {
    return NodeType{
        .id = "control.pi",
        .displayName = "PI Controller",
        .description = "Controls an error with proportional and integral action.",
        .inputPorts = {Port{.name = "error",
                            .direction = PortDirection::Input,
                            .type = WireType{.quantity = Quantity::Dimensionless,
                                             .frame = Frame::Scalar,
                                             .dtype = DType::F32},
                            .description = "Setpoint minus measurement."}},
        .outputPorts = {Port{.name = "out",
                             .direction = PortDirection::Output,
                             .type = WireType{.quantity = Quantity::Dimensionless,
                                              .frame = Frame::Scalar,
                                              .dtype = DType::F32},
                             .description = "Controller output."}},
        .parameterTypes = {{"Kp", WireType{}}},
        .parameterDescriptions = {{"Kp", "Proportional gain."}},
        .inlineCode = "return params.kp * error + integrator;",
        .constructorCode = "integrator = 0.0f;",
        .classHeader = "class PiController { float integrator; public: float Step(float error); };",
        .classDefinition = "float PiController::Step(float error) { ... }",
    };
}

Graph MakeDemoGraph() {
    Graph graph;
    graph.AddNodeType(MakeValueType());
    graph.AddNodeType(MakeDisplayType());
    graph.AddNode(Node{.id = "source", .type = "constant.value", .domain = "app_loop"});
    graph.AddNode(Node{.id = "sink", .type = "display.value", .domain = "app_loop"});
    graph.Connect(Connection{
        .id = "c1",
        .from = PortRef{.nodeId = "source", .portName = "out"},
        .to = PortRef{.nodeId = "sink", .portName = "in"},
    });
    return graph;
}

}  // namespace

TEST(WireType, UnitLabels) {
    EXPECT_EQ(GetUnitLabel(Quantity::Voltage), "V");
    EXPECT_EQ(GetUnitLabel(Quantity::Current), "A");
    EXPECT_EQ(GetUnitLabel(Quantity::Temperature), "degC");
    EXPECT_EQ(GetUnitLabel(Quantity::Dimensionless), "");
}

TEST(WireType, RoundTripStrings) {
    for (const auto q : {Quantity::Voltage, Quantity::Current, Quantity::AngularVelocity,
                         Quantity::Torque, Quantity::Temperature, Quantity::Dimensionless,
                         Quantity::Boolean}) {
        EXPECT_EQ(QuantityFromString(ToString(q)), q);
    }
    for (const auto f : {Frame::Scalar, Frame::Abc, Frame::AlphaBeta, Frame::Dq}) {
        EXPECT_EQ(FrameFromString(ToString(f)), f);
    }
    EXPECT_EQ(DTypeFromString(ToString(DType::F32)), DType::F32);
}

TEST(NodeType, CodePiecesRoundTrip) {
    const auto type = MakePiControllerType();
    EXPECT_EQ(type.inlineCode, "return params.kp * error + integrator;");
    EXPECT_FALSE(type.classHeader.empty());
    EXPECT_FALSE(type.classDefinition.empty());
}

TEST(Graph, AddAndFindNodeType) {
    Graph graph;
    EXPECT_TRUE(graph.AddNodeType(MakeValueType()));
    EXPECT_TRUE(graph.FindNodeType("constant.value").has_value());
    EXPECT_FALSE(graph.FindNodeType("missing").has_value());
}

TEST(Graph, RejectDuplicateNodeTypeId) {
    Graph graph;
    EXPECT_TRUE(graph.AddNodeType(MakeValueType()));
    EXPECT_FALSE(graph.AddNodeType(MakeValueType()));
}

TEST(Graph, AddNodeRequiresKnownType) {
    Graph graph;
    EXPECT_FALSE(graph.AddNode(Node{.id = "n", .type = "unknown", .domain = "app_loop"}));
}

TEST(Graph, SetNodeParameters) {
    Graph graph = MakeDemoGraph();

    EXPECT_TRUE(graph.SetNodeParameters("source", {{"Gain", "2.5"}, {"Key", "cfg_gain"}}));
    const auto node = graph.FindNode("source");
    ASSERT_TRUE(node.has_value());
    EXPECT_EQ(node->parameters.at("Gain"), "2.5");
    EXPECT_EQ(node->parameters.at("Key"), "cfg_gain");

    // Replaces the whole map rather than merging.
    EXPECT_TRUE(graph.SetNodeParameters("source", {{"Gain", "1.0"}}));
    EXPECT_EQ(graph.FindNode("source")->parameters.size(), 1u);

    EXPECT_FALSE(graph.SetNodeParameters("missing", {{"Gain", "1.0"}}));
}

TEST(Graph, SetNodeExcludeFromCompile) {
    Graph graph = MakeDemoGraph();

    EXPECT_FALSE(graph.FindNode("source")->excludeFromCompile);
    EXPECT_TRUE(graph.SetNodeExcludeFromCompile("source", true));
    EXPECT_TRUE(graph.FindNode("source")->excludeFromCompile);
    EXPECT_TRUE(graph.SetNodeExcludeFromCompile("source", false));
    EXPECT_FALSE(graph.FindNode("source")->excludeFromCompile);
    EXPECT_FALSE(graph.SetNodeExcludeFromCompile("missing", true));
}

TEST(Serialization, SchemaVersionWrittenAndChecked) {
    Graph graph = MakeDemoGraph();
    const std::string json = SaveToJson(graph);

    // Freshly saved graphs carry the current schema version and pass cleanly.
    EXPECT_NE(json.find("\"schemaVersion\": 1"), std::string::npos);
    EXPECT_EQ(CheckGraphSchema(json).status, SchemaStatus::kCurrent);

    // Pre-versioning files (no schemaVersion) are legacy: no warning.
    const std::string legacy = R"({"name":"g","nodeTypes":[],"nodes":[],"connections":[]})";
    EXPECT_EQ(CheckGraphSchema(legacy).status, SchemaStatus::kCurrent);
    EXPECT_TRUE(CheckGraphSchema(legacy).warning.empty());

    // schemaVersion 0 is treated as legacy too.
    const std::string zero = R"({"schemaVersion": 0})";
    EXPECT_EQ(CheckGraphSchema(zero).status, SchemaStatus::kCurrent);

    // Older files load with a warning; newer files warn louder.
    const auto olderStatus = CheckSchemaVersion(1, 2, "graph");
    EXPECT_EQ(olderStatus.status, SchemaStatus::kOlder);
    EXPECT_FALSE(olderStatus.warning.empty());

    const auto newer = CheckGraphSchema(R"({"schemaVersion": 999})");
    EXPECT_EQ(newer.status, SchemaStatus::kNewer);
    EXPECT_NE(newer.warning.find("NEWER"), std::string::npos);

    // Garbage input never throws and reports nothing.
    EXPECT_EQ(CheckGraphSchema("not json").status, SchemaStatus::kCurrent);
}

TEST(Serialization, ExcludeFromCompileRoundTrip) {
    Graph graph = MakeDemoGraph();
    ASSERT_TRUE(graph.SetNodeExcludeFromCompile("source", true));

    Graph loaded;
    LoadIntoGraph(loaded, SaveToJson(graph));

    const auto excluded = loaded.FindNode("source");
    ASSERT_TRUE(excluded.has_value());
    EXPECT_TRUE(excluded->excludeFromCompile);
    // Nodes without the flag default to false and stay absent from the JSON.
    const auto normal = loaded.FindNode("sink");
    ASSERT_TRUE(normal.has_value());
    EXPECT_FALSE(normal->excludeFromCompile);
    EXPECT_EQ(SaveToJson(loaded).find("excludeFromCompile"),
              SaveToJson(graph).find("excludeFromCompile"));
}

TEST(Graph, ComputeExcludedNodes) {
    // Chain: a -> b -> c, plus standalone d (single exclusion) and e (clean).
    NodeType passThrough{
        .id = "test.passthrough",
        .displayName = "Pass",
        .inputPorts = {Port{.name = "in",
                            .direction = PortDirection::Input,
                            .type = WireType{.quantity = Quantity::Dimensionless,
                                             .frame = Frame::Scalar,
                                             .dtype = DType::F32}}},
        .outputPorts = {Port{.name = "out",
                             .direction = PortDirection::Output,
                             .type = WireType{.quantity = Quantity::Dimensionless,
                                              .frame = Frame::Scalar,
                                              .dtype = DType::F32}}},
    };

    Graph graph;
    graph.AddNodeType(passThrough);
    graph.AddNodeType(MakeDisplayType());
    graph.AddNode(Node{.id = "a", .type = "test.passthrough", .domain = "app_loop"});
    graph.AddNode(Node{.id = "b", .type = "test.passthrough", .domain = "app_loop"});
    graph.AddNode(Node{.id = "c", .type = "display.value", .domain = "app_loop"});
    graph.AddNode(Node{.id = "d", .type = "display.value", .domain = "app_loop"});
    graph.AddNode(Node{.id = "e", .type = "display.value", .domain = "app_loop"});
    graph.Connect(Connection{.id = "c1",
                             .from = PortRef{.nodeId = "a", .portName = "out"},
                             .to = PortRef{.nodeId = "b", .portName = "in"}});
    graph.Connect(Connection{.id = "c2",
                             .from = PortRef{.nodeId = "b", .portName = "out"},
                             .to = PortRef{.nodeId = "c", .portName = "in"}});
    graph.Connect(Connection{.id = "c3",
                             .from = PortRef{.nodeId = "a", .portName = "out"},
                             .to = PortRef{.nodeId = "e", .portName = "in"}});

    ASSERT_TRUE(graph.SetNodeExcludeFromCompileRecursive("a", true));
    ASSERT_TRUE(graph.SetNodeExcludeFromCompile("d", true));

    const auto excluded = graph.ComputeExcludedNodes();

    // The recursive flag pulls in the whole downstream chain (a, b, c, e).
    EXPECT_EQ(excluded.at("a"), "a");
    EXPECT_EQ(excluded.at("b"), "a");
    EXPECT_EQ(excluded.at("c"), "a");
    EXPECT_EQ(excluded.at("e"), "a");
    // Directly flagged nodes map to themselves.
    EXPECT_EQ(excluded.at("d"), "d");
    EXPECT_EQ(excluded.size(), 5u);

    // Clearing the flag releases the chain.
    ASSERT_TRUE(graph.SetNodeExcludeFromCompileRecursive("a", false));
    const auto after = graph.ComputeExcludedNodes();
    EXPECT_EQ(after.size(), 1u);
    EXPECT_EQ(after.count("d"), 1u);

    EXPECT_FALSE(graph.SetNodeExcludeFromCompileRecursive("missing", true));
}

TEST(Serialization, RecursiveExclusionRoundTrip) {
    Graph graph = MakeDemoGraph();
    ASSERT_TRUE(graph.SetNodeExcludeFromCompileRecursive("source", true));

    Graph loaded;
    LoadIntoGraph(loaded, SaveToJson(graph));

    const auto node = loaded.FindNode("source");
    ASSERT_TRUE(node.has_value());
    EXPECT_TRUE(node->excludeFromCompileRecursive);
    EXPECT_FALSE(loaded.FindNode("sink")->excludeFromCompileRecursive);

    // zeroInputs is an emitter-computed artifact and must not be serialized.
    ASSERT_TRUE(graph.SetNodeZeroInputs("sink", {"in"}));
    EXPECT_EQ(SaveToJson(graph).find("zeroInputs"), std::string::npos);
}

TEST(Graph, SetNodeDomain) {
    Graph graph = MakeDemoGraph();

    EXPECT_TRUE(graph.SetNodeDomain("source", "isr_pwm"));
    EXPECT_EQ(graph.FindNode("source")->domain, "isr_pwm");

    // Empty string unassigns the domain.
    EXPECT_TRUE(graph.SetNodeDomain("source", ""));
    EXPECT_TRUE(graph.FindNode("source")->domain.empty());

    EXPECT_FALSE(graph.SetNodeDomain("missing", "isr_pwm"));
}

TEST(Graph, SetNodeParameterInputs) {
    Graph graph;
    NodeType type = MakeValueType();
    type.parameterTypes = {{"Gain", WireType{}}};
    EXPECT_TRUE(graph.AddNodeType(type));
    EXPECT_TRUE(graph.AddNode(Node{.id = "n", .type = "constant.value", .domain = "app_loop"}));

    EXPECT_TRUE(graph.SetNodeParameterInputs("n", {"Gain"}));
    const auto node = graph.FindNode("n");
    ASSERT_TRUE(node.has_value());
    EXPECT_EQ(node->parameterInputs.size(), 1u);
    EXPECT_EQ(node->parameterInputs.front(), "Gain");

    // A flagged parameter resolves to a synthesized input port.
    const auto port = graph.FindPort(PortRef{.nodeId = "n", .portName = "Gain"});
    ASSERT_TRUE(port.has_value());
    EXPECT_EQ(port->direction, PortDirection::Input);

    EXPECT_TRUE(graph.SetNodeParameterInputs("n", {}));
    EXPECT_TRUE(graph.FindNode("n")->parameterInputs.empty());

    EXPECT_FALSE(graph.SetNodeParameterInputs("missing", {"Gain"}));
}

TEST(Graph, MaxInstancesEnforced) {
    Graph graph;
    NodeType limited = MakeValueType();
    limited.maxInstances = 2;
    EXPECT_TRUE(graph.AddNodeType(limited));

    EXPECT_TRUE(graph.AddNode(Node{.id = "a", .type = "constant.value", .domain = "app_loop"}));
    EXPECT_TRUE(graph.AddNode(Node{.id = "b", .type = "constant.value", .domain = "app_loop"}));
    EXPECT_FALSE(graph.AddNode(Node{.id = "c", .type = "constant.value", .domain = "app_loop"}));
}

TEST(Graph, AddAndFindNode) {
    Graph graph = MakeDemoGraph();

    auto node = graph.FindNode("source");
    ASSERT_TRUE(node.has_value());
    EXPECT_EQ(node->type, "constant.value");
    EXPECT_EQ(node->domain, "app_loop");

    EXPECT_FALSE(graph.FindNode("missing").has_value());
}

TEST(Graph, RejectDuplicateNodeId) {
    Graph graph = MakeDemoGraph();
    EXPECT_FALSE(graph.AddNode(Node{.id = "source", .type = "display.value", .domain = "app_loop"}));
}

TEST(Graph, RejectEmptyNodeId) {
    Graph graph;
    graph.AddNodeType(MakeValueType());
    EXPECT_FALSE(graph.AddNode(Node{.id = "", .type = "constant.value", .domain = "app_loop"}));
}

TEST(Graph, RemoveNodeCleansConnections) {
    Graph graph = MakeDemoGraph();
    EXPECT_EQ(graph.GetConnections().size(), 1u);

    EXPECT_TRUE(graph.RemoveNode("source"));
    EXPECT_EQ(graph.GetConnections().size(), 0u);
}

TEST(Graph, TypeCheckMatchingPorts) {
    Graph graph = MakeDemoGraph();

    Connection c{
        .id = "c2",
        .from = PortRef{.nodeId = "source", .portName = "out"},
        .to = PortRef{.nodeId = "sink", .portName = "in"},
    };

    EXPECT_TRUE(graph.TypeCheck(c));
    EXPECT_TRUE(graph.Connect(c));
}

TEST(Graph, TypeCheckImplicitExtraction) {
    /* A dimensionless scalar input accepts a voltage/current scalar output
     * (implicit unit extraction at the codegen binding site). */
    Graph graph;
    graph.AddNodeType(MakeVoltageType());
    graph.AddNodeType(MakeDisplayType());
    graph.AddNode(Node{.id = "voltage", .type = "constant.voltage", .domain = "app_loop"});
    graph.AddNode(Node{.id = "sink", .type = "display.value", .domain = "app_loop"});

    Connection c{
        .id = "c1",
        .from = PortRef{.nodeId = "voltage", .portName = "out"},
        .to = PortRef{.nodeId = "sink", .portName = "in"},
    };

    EXPECT_TRUE(graph.TypeCheck(c));
    EXPECT_TRUE(graph.Connect(c));
}

TEST(Graph, TypeCheckImplicitInjection) {
    /* A physical-quantity scalar input accepts a dimensionless scalar output
     * (implicit unit injection at the codegen binding site). */
    Graph graph;
    graph.AddNodeType(NodeType{
        .id = "constant.value",
        .displayName = "Value",
        .outputPorts = {Port{.name = "out",
                             .direction = PortDirection::Output,
                             .type = WireType{.quantity = Quantity::Dimensionless,
                                              .frame = Frame::Scalar,
                                              .dtype = DType::F32}}},
    });
    graph.AddNodeType(MakeCurrentSinkType());
    graph.AddNode(Node{.id = "value", .type = "constant.value", .domain = "app_loop"});
    graph.AddNode(Node{.id = "sink", .type = "load.current", .domain = "app_loop"});

    Connection c{
        .id = "c1",
        .from = PortRef{.nodeId = "value", .portName = "out"},
        .to = PortRef{.nodeId = "sink", .portName = "in"},
    };

    EXPECT_TRUE(graph.TypeCheck(c));
    EXPECT_TRUE(graph.Connect(c));
}

TEST(Graph, TypeCheckRejectsBooleanInjection) {
    /* Boolean ports never participate in implicit conversions. */
    Graph graph;
    graph.AddNodeType(NodeType{
        .id = "constant.value",
        .displayName = "Value",
        .outputPorts = {Port{.name = "out",
                             .direction = PortDirection::Output,
                             .type = WireType{.quantity = Quantity::Dimensionless,
                                              .frame = Frame::Scalar,
                                              .dtype = DType::F32}}},
    });
    graph.AddNodeType(NodeType{
        .id = "load.flag",
        .displayName = "Flag Sink",
        .inputPorts = {Port{.name = "in",
                            .direction = PortDirection::Input,
                            .type = WireType{.quantity = Quantity::Boolean,
                                             .frame = Frame::Scalar,
                                             .dtype = DType::F32}}},
    });
    graph.AddNode(Node{.id = "value", .type = "constant.value", .domain = "app_loop"});
    graph.AddNode(Node{.id = "sink", .type = "load.flag", .domain = "app_loop"});

    Connection c{
        .id = "c1",
        .from = PortRef{.nodeId = "value", .portName = "out"},
        .to = PortRef{.nodeId = "sink", .portName = "in"},
    };

    EXPECT_FALSE(graph.TypeCheck(c));
    EXPECT_FALSE(graph.Connect(c));
}

TEST(Graph, TypeCheckRejectsNonConvertibleMismatch) {
    /* voltage -> current is still a hard type error (no implicit rule). */
    Graph graph;
    graph.AddNodeType(MakeVoltageType());
    graph.AddNodeType(MakeCurrentSinkType());
    graph.AddNode(Node{.id = "voltage", .type = "constant.voltage", .domain = "app_loop"});
    graph.AddNode(Node{.id = "sink", .type = "load.current", .domain = "app_loop"});

    Connection c{
        .id = "c1",
        .from = PortRef{.nodeId = "voltage", .portName = "out"},
        .to = PortRef{.nodeId = "sink", .portName = "in"},
    };

    EXPECT_FALSE(graph.TypeCheck(c));
    EXPECT_FALSE(graph.Connect(c));
}

TEST(Graph, RejectBadDirection) {
    Graph graph = MakeDemoGraph();

    Connection c{
        .id = "c2",
        .from = PortRef{.nodeId = "sink", .portName = "in"},
        .to = PortRef{.nodeId = "source", .portName = "out"},
    };

    EXPECT_FALSE(graph.Connect(c));
}

TEST(Graph, RejectDuplicateConnectionId) {
    Graph graph = MakeDemoGraph();

    Connection c{
        .id = "c1",
        .from = PortRef{.nodeId = "source", .portName = "out"},
        .to = PortRef{.nodeId = "sink", .portName = "in"},
    };
    EXPECT_FALSE(graph.Connect(c));
}

TEST(Graph, FindPortLooksUpNodeType) {
    Graph graph = MakeDemoGraph();
    const auto port = graph.FindPort(PortRef{.nodeId = "source", .portName = "out"});
    ASSERT_TRUE(port.has_value());
    EXPECT_EQ(port->direction, PortDirection::Output);
    EXPECT_EQ(port->type.quantity, Quantity::Dimensionless);
}

TEST(Serialization, RoundTrip) {
    Graph graph;
    graph.SetName("demo");
    graph.AddNodeType(MakePiControllerType());
    graph.AddNodeType(MakeValueType());
    graph.AddNode(Node{.id = "source",
                       .type = "constant.value",
                       .displayName = "Throttle",
                       .domain = "app_loop",
                       .position = {10.0, 20.0},
                       .parameters = {{"value", "0.5"}, {"label", "throttle"}}});
    graph.AddNode(Node{.id = "pi", .type = "control.pi", .domain = "app_loop"});
    graph.Connect(Connection{
        .id = "c1",
        .from = PortRef{.nodeId = "source", .portName = "out"},
        .to = PortRef{.nodeId = "pi", .portName = "error"},
    });

    const std::string json = SaveToJson(graph);
    const Graph loaded = LoadFromJson(json);

    EXPECT_EQ(loaded.GetName(), "demo");
    EXPECT_EQ(loaded.GetNodeTypes().size(), 2u);
    EXPECT_EQ(loaded.GetNodes().size(), 2u);
    EXPECT_EQ(loaded.GetConnections().size(), 1u);

    const auto source = loaded.FindNode("source");
    ASSERT_TRUE(source.has_value());
    EXPECT_EQ(source->domain, "app_loop");
    EXPECT_EQ(source->position.x, 10.0);
    ASSERT_EQ(source->parameters.size(), 2u);
    EXPECT_EQ(source->parameters.at("value"), "0.5");
    EXPECT_EQ(source->parameters.at("label"), "throttle");

    const auto piType = loaded.FindNodeType("control.pi");
    ASSERT_TRUE(piType.has_value());
    EXPECT_EQ(piType->description,
              "Controls an error with proportional and integral action.");
    EXPECT_EQ(piType->FindInputPort("error")->description,
              "Setpoint minus measurement.");
    EXPECT_EQ(piType->FindOutputPort("out")->description,
              "Controller output.");
    ASSERT_TRUE(piType->FindParameterDescription("Kp").has_value());
    EXPECT_EQ(*piType->FindParameterDescription("Kp"),
              "Proportional gain.");
    EXPECT_EQ(piType->inlineCode, "return params.kp * error + integrator;");
    EXPECT_EQ(piType->constructorCode, "integrator = 0.0f;");
    EXPECT_FALSE(piType->classHeader.empty());

    const auto c = loaded.FindConnection("c1");
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(c->from.portName, "out");
    EXPECT_EQ(c->to.nodeId, "pi");
}

TEST(Bridge, AddAndFind) {
    Graph graph;
    graph.AddNodeType(MakeValueType());
    graph.AddNodeType(MakeDisplayType());
    graph.AddNode(Node{.id = "source", .type = "constant.value", .domain = "adc_sample"});
    graph.AddNode(Node{.id = "sink", .type = "display.value", .domain = "app_loop"});

    const WireType scalar = WireType{.quantity = Quantity::Dimensionless,
                                     .frame = Frame::Scalar,
                                     .dtype = DType::F32};
    EXPECT_TRUE(graph.AddBridge(Bridge{
        .id = "b1",
        .type = scalar,
        .producer = PortRef{.nodeId = "source", .portName = "out"},
        .consumer = PortRef{.nodeId = "sink", .portName = "in"},
    }));
    EXPECT_EQ(graph.GetBridges().size(), 1u);

    const auto bridge = graph.FindBridge("b1");
    ASSERT_TRUE(bridge.has_value());
    EXPECT_EQ(bridge->producer.nodeId, "source");
    EXPECT_EQ(bridge->consumer.nodeId, "sink");
    EXPECT_EQ(bridge->type, scalar);
}

TEST(Bridge, RejectBadDirection) {
    Graph graph;
    graph.AddNodeType(MakeValueType());
    graph.AddNodeType(MakeDisplayType());
    graph.AddNode(Node{.id = "source", .type = "constant.value", .domain = "adc_sample"});
    graph.AddNode(Node{.id = "sink", .type = "display.value", .domain = "app_loop"});

    const WireType scalar = WireType{.quantity = Quantity::Dimensionless,
                                     .frame = Frame::Scalar,
                                     .dtype = DType::F32};
    EXPECT_FALSE(graph.AddBridge(Bridge{
        .id = "b1",
        .type = scalar,
        .producer = PortRef{.nodeId = "sink", .portName = "in"},
        .consumer = PortRef{.nodeId = "source", .portName = "out"},
    }));
}

TEST(Bridge, ImplicitExtractionBridge) {
    /* A dimensionless bridge accepts a voltage/current producer (extracted
     * at the store) as long as the consumer is dimensionless too. */
    Graph graph;
    graph.AddNodeType(MakeVoltageType());
    graph.AddNodeType(MakeDisplayType());
    graph.AddNode(Node{.id = "voltage", .type = "constant.voltage", .domain = "adc_sample"});
    graph.AddNode(Node{.id = "sink", .type = "display.value", .domain = "app_loop"});

    const WireType scalar = WireType{.quantity = Quantity::Dimensionless,
                                     .frame = Frame::Scalar,
                                     .dtype = DType::F32};
    EXPECT_TRUE(graph.AddBridge(Bridge{
        .id = "b1",
        .type = scalar,
        .producer = PortRef{.nodeId = "voltage", .portName = "out"},
        .consumer = PortRef{.nodeId = "sink", .portName = "in"},
    }));
}

TEST(Bridge, RejectMismatchedType) {
    /* A current-typed bridge with a voltage producer must still fail:
     * the consumer has to match the bridge exactly. */
    Graph graph;
    graph.AddNodeType(MakeVoltageType());
    graph.AddNodeType(MakeCurrentSinkType());
    graph.AddNode(Node{.id = "voltage", .type = "constant.voltage", .domain = "adc_sample"});
    graph.AddNode(Node{.id = "sink", .type = "load.current", .domain = "app_loop"});

    const WireType current = WireType{.quantity = Quantity::Current,
                                      .frame = Frame::Scalar,
                                      .dtype = DType::F32};
    EXPECT_FALSE(graph.AddBridge(Bridge{
        .id = "b1",
        .type = current,
        .producer = PortRef{.nodeId = "voltage", .portName = "out"},
        .consumer = PortRef{.nodeId = "sink", .portName = "in"},
    }));
}

TEST(Bridge, RejectConsumerWithConnection) {
    Graph graph;
    graph.AddNodeType(MakeValueType());
    graph.AddNodeType(MakeDisplayType());
    graph.AddNode(Node{.id = "source", .type = "constant.value", .domain = "app_loop"});
    graph.AddNode(Node{.id = "sink", .type = "display.value", .domain = "app_loop"});

    EXPECT_TRUE(graph.Connect(Connection{
        .id = "c1",
        .from = PortRef{.nodeId = "source", .portName = "out"},
        .to = PortRef{.nodeId = "sink", .portName = "in"},
    }));

    const WireType scalar = WireType{.quantity = Quantity::Dimensionless,
                                     .frame = Frame::Scalar,
                                     .dtype = DType::F32};
    EXPECT_FALSE(graph.AddBridge(Bridge{
        .id = "b1",
        .type = scalar,
        .producer = PortRef{.nodeId = "source", .portName = "out"},
        .consumer = PortRef{.nodeId = "sink", .portName = "in"},
    }));
}

TEST(Bridge, ConnectRejectsBridgedConsumer) {
    Graph graph;
    graph.AddNodeType(MakeValueType());
    graph.AddNodeType(MakeDisplayType());
    graph.AddNode(Node{.id = "source", .type = "constant.value", .domain = "adc_sample"});
    graph.AddNode(Node{.id = "sink", .type = "display.value", .domain = "app_loop"});

    const WireType scalar = WireType{.quantity = Quantity::Dimensionless,
                                     .frame = Frame::Scalar,
                                     .dtype = DType::F32};
    EXPECT_TRUE(graph.AddBridge(Bridge{
        .id = "b1",
        .type = scalar,
        .producer = PortRef{.nodeId = "source", .portName = "out"},
        .consumer = PortRef{.nodeId = "sink", .portName = "in"},
    }));

    EXPECT_FALSE(graph.Connect(Connection{
        .id = "c1",
        .from = PortRef{.nodeId = "source", .portName = "out"},
        .to = PortRef{.nodeId = "sink", .portName = "in"},
    }));
}

TEST(Bridge, RemoveNodeCleansBridges) {
    Graph graph;
    graph.AddNodeType(MakeValueType());
    graph.AddNodeType(MakeDisplayType());
    graph.AddNode(Node{.id = "source", .type = "constant.value", .domain = "adc_sample"});
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

    EXPECT_EQ(graph.GetBridges().size(), 1u);
    EXPECT_TRUE(graph.RemoveNode("source"));
    EXPECT_EQ(graph.GetBridges().size(), 0u);
}

TEST(Serialization, BridgeRoundTrip) {
    Graph graph;
    graph.SetName("bridged");
    graph.AddNodeType(MakeValueType());
    graph.AddNodeType(MakeDisplayType());
    graph.AddNode(Node{.id = "source", .type = "constant.value", .domain = "adc_sample"});
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

    const std::string json = SaveToJson(graph);
    const Graph loaded = LoadFromJson(json);

    EXPECT_EQ(loaded.GetBridges().size(), 1u);
    const auto bridge = loaded.FindBridge("b1");
    ASSERT_TRUE(bridge.has_value());
    EXPECT_EQ(bridge->producer.nodeId, "source");
    EXPECT_EQ(bridge->consumer.nodeId, "sink");
    EXPECT_EQ(bridge->type, scalar);
}
