// PlantCodegen: scheduler, emission-structure, and validation tests.

#include "PlantCodegen/PlantGenerator.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "plant_test_util.h"

#ifndef RTE_SOURCE_DIR
#error RTE_SOURCE_DIR must point at the repository root
#endif

namespace {

using planttest::LoadPlantGraph;

NodeAPI::Graph LoadExample(const std::string& name) {
    NodeAPI::Graph graph;
    std::string error;
    EXPECT_TRUE(LoadPlantGraph(planttest::PlantExample(name),
                               planttest::PlantTemplatesDir(), graph, error))
        << error;
    return graph;
}

struct Emitted {
    std::filesystem::path dir;
    std::string header;
    std::string source;
    std::vector<std::string> schedule;  // node ids in frozen step order
};

Emitted GenerateIntoTemp(const NodeAPI::Graph& graph, const std::string& tag) {
    Emitted emitted;
    emitted.dir = std::filesystem::temp_directory_path() /
                  ("plantcodegen_" + tag + "_" + std::to_string(::getpid()));
    std::filesystem::remove_all(emitted.dir);
    PlantCodegen::PlantGenerator generator(graph);
    std::string error;
    EXPECT_TRUE(generator.Generate(emitted.dir.string(), error)) << error;

    {
        std::ifstream file(emitted.dir / "plant_generated.h");
        std::ostringstream text;
        text << file.rdbuf();
        emitted.header = text.str();
    }
    {
        std::ifstream file(emitted.dir / "plant_generated.cpp");
        std::ostringstream text;
        text << file.rdbuf();
        emitted.source = text.str();
    }
    EXPECT_FALSE(emitted.header.empty());
    EXPECT_FALSE(emitted.source.empty());

    // Parse the frozen schedule from the "// ---- step N: <id> (<type>)" markers.
    std::istringstream lines(emitted.source);
    std::string line;
    while (std::getline(lines, line)) {
        const auto marker = line.find("step ");
        if (marker == std::string::npos) continue;
        const auto colon = line.find(": ", marker + 5);
        const auto paren = line.find(" (", colon);
        if (colon == std::string::npos || paren == std::string::npos) continue;
        emitted.schedule.push_back(line.substr(colon + 2, paren - colon - 2));
    }
    return emitted;
}

std::size_t StepOf(const Emitted& emitted, const std::string& nodeId) {
    for (std::size_t i = 0; i < emitted.schedule.size(); ++i) {
        if (emitted.schedule[i] == nodeId) return i;
    }
    ADD_FAILURE() << "node " << nodeId << " never scheduled";
    return emitted.schedule.size();
}

bool ExpectGenerateFails(NodeAPI::Graph graph, const std::string& tag) {
    PlantCodegen::PlantGenerator generator(graph);
    const auto dir = std::filesystem::temp_directory_path() /
                     ("plantcodegen_bad_" + tag + "_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    std::string error;
    return !generator.Generate(dir.string(), error) && !error.empty();
}

// ---------------------------------------------------------------------------
// Scheduler order (frozen at codegen; the python reference trace is
// twin/plantgraph/plantgraph.py _run_pass — see PLANtTESTS derivation note).

TEST(PlantCodegenSchedule, Gen7FrozenOrder) {
    const auto emitted = GenerateIntoTemp(LoadExample("gen7_bench_pmsm"), "sched_gen7");

    // The node-by-node relative assertions from the execution contract:
    EXPECT_LT(StepOf(emitted, "Knee"), StepOf(emitted, "Machine"));
    EXPECT_LT(StepOf(emitted, "Machine"), StepOf(emitted, "Im"));
    EXPECT_LT(StepOf(emitted, "Machine"), StepOf(emitted, "Loss"));
    EXPECT_LT(StepOf(emitted, "Machine"), StepOf(emitted, "Draw"));
    EXPECT_LT(StepOf(emitted, "Loss"), StepOf(emitted, "DcBus"));
    EXPECT_LT(StepOf(emitted, "Draw"), StepOf(emitted, "DcBus"));
    EXPECT_LT(StepOf(emitted, "DcBus"), StepOf(emitted, "VSense"));
    EXPECT_LT(StepOf(emitted, "Machine"), StepOf(emitted, "HallU"));
    EXPECT_LT(StepOf(emitted, "Machine"), StepOf(emitted, "Enc"));
    EXPECT_EQ(StepOf(emitted, "Meas"), emitted.schedule.size() - 1);

    // Exact frozen order, verified against the python reference evaluator
    // (PYTHONPATH=. python3 trace of PlantGraphSim._run_pass, 2026-10-21;
    // includes the v1.1 phase-ADC voltage-sense chain).
    // Knee/DcBus are the cycle-forced evaluations (PRE readings).
    const std::vector<std::string> expected = {
        "AbsOm", "Knee", "Machine", "Im", "HallU", "HallV", "HallW", "Enc",
        "Loss", "Draw", "DcBus", "Thermal", "VSense", "VSenseU", "VSenseV",
        "VSenseW", "Meas",
    };
    EXPECT_EQ(emitted.schedule, expected);
}

TEST(PlantCodegenSchedule, TecoFrozenOrder) {
    const auto emitted = GenerateIntoTemp(LoadExample("teco_vhz_induction"), "sched_teco");

    EXPECT_LT(StepOf(emitted, "Knee"), StepOf(emitted, "Machine"));
    EXPECT_LT(StepOf(emitted, "Machine"), StepOf(emitted, "Im"));
    EXPECT_LT(StepOf(emitted, "Loss"), StepOf(emitted, "DcBus"));
    EXPECT_LT(StepOf(emitted, "Draw"), StepOf(emitted, "DcBus"));
    EXPECT_LT(StepOf(emitted, "DcBus"), StepOf(emitted, "VSense"));
    EXPECT_LT(StepOf(emitted, "Machine"), StepOf(emitted, "HallU"));
    EXPECT_EQ(StepOf(emitted, "Meas"), emitted.schedule.size() - 1);

    // Cycle-force direction (verified against the python reference's force
    // rule "fewest pending stateful ancestors"): Shaft has only the Machine
    // as stateful ancestor, the Machine counts Shaft+DcBus — so Shaft is
    // forced FIRST and integrates against the PRE machine torque; the Machine
    // then integrates against the POST shaft omega.
    EXPECT_LT(StepOf(emitted, "Shaft"), StepOf(emitted, "Machine"));
    EXPECT_EQ(StepOf(emitted, "Shaft"), 0u);

    const std::vector<std::string> expected = {
        "Shaft", "AbsOm", "Knee", "Machine", "Im", "HallU", "HallV", "HallW",
        "Loss", "Draw", "DcBus", "Thermal", "VSense", "VSenseU", "VSenseV",
        "VSenseW", "Meas",
    };
    EXPECT_EQ(emitted.schedule, expected);
}

// The Zero DSR graph shares the gen7 topology (PMSM + dyno) with 16 pole
// pairs; pin its exact frozen order too (python _run_pass trace 2026-10-21).
TEST(PlantCodegenSchedule, ZeroFrozenOrder) {
    const auto emitted = GenerateIntoTemp(LoadExample("zero_dsr_zf75_7"), "sched_zero");

    // Same forced-evaluation shape as gen7: Knee before Machine, DcBus after
    // Loss+Draw, Meas is the collector last.
    EXPECT_LT(StepOf(emitted, "Knee"), StepOf(emitted, "Machine"));
    EXPECT_LT(StepOf(emitted, "Machine"), StepOf(emitted, "Enc"));
    EXPECT_LT(StepOf(emitted, "Loss"), StepOf(emitted, "DcBus"));
    EXPECT_LT(StepOf(emitted, "Draw"), StepOf(emitted, "DcBus"));
    EXPECT_EQ(StepOf(emitted, "Meas"), emitted.schedule.size() - 1);

    const std::vector<std::string> expected = {
        "AbsOm", "Knee", "Machine", "Im", "HallU", "HallV", "HallW", "Enc",
        "Loss", "Draw", "DcBus", "Thermal", "VSense", "VSenseU", "VSenseV",
        "VSenseW", "Meas",
    };
    EXPECT_EQ(emitted.schedule, expected);
}

// ---------------------------------------------------------------------------
// Emission structure.

TEST(PlantCodegenEmission, Gen7Structure) {
    const auto emitted = GenerateIntoTemp(LoadExample("gen7_bench_pmsm"), "struct_gen7");
    const auto& h = emitted.header;
    const auto& cpp = emitted.source;

    // Per-node structs: outputs, internal state, params, supply mutables.
    EXPECT_NE(h.find("struct MachineNode"), std::string::npos);
    EXPECT_NE(h.find("float ThetaE = 0.0f;"), std::string::npos);
    EXPECT_NE(h.find("float Lag = 0.0f;"), std::string::npos);   // VSense internal state
    EXPECT_NE(h.find("static constexpr double Rs = 0.00765;"), std::string::npos);
    EXPECT_NE(h.find("double VSet_V = 0.0;"), std::string::npos);  // DcBus supply field
    EXPECT_NE(h.find("double CanSink = 0.0;"), std::string::npos);
    // Unwired input escape-hatch fields (PlantSetInput targets).
    EXPECT_NE(h.find("float C = 0.0f;"), std::string::npos);       // Draw.C
    // PlantMeas has one field per template input port.
    EXPECT_NE(h.find("float Iu_A = 0.0f;"), std::string::npos);
    EXPECT_NE(h.find("float Dclink_P_W = 0.0f;"), std::string::npos);

    // Init: DutyIn zero-vector default, DcBus supply init (all seven D9
    // knobs), constructor paste, and the D11 t=0 publish call.
    EXPECT_NE(cpp.find("state.DutyIn.Du = 50.0f;"), std::string::npos);
    EXPECT_NE(cpp.find("state.DcBus.VSet_V = 144.7;"), std::string::npos);
    EXPECT_NE(cpp.find("state.DcBus.CvClamp_V = 0;"), std::string::npos);
    EXPECT_NE(cpp.find("state.DcBus.SeriesR_mOhm = 5;"), std::string::npos);
    EXPECT_NE(cpp.find("Id = Id0;"), std::string::npos);           // PmsmDq constructor
    EXPECT_NE(cpp.find("Lag = 0.0;"), std::string::npos);          // VSense constructor
    EXPECT_NE(cpp.find("PlantStep(state, 0.0);"), std::string::npos);  // D11 t0 publish

    // Step bindings per the contract; bodies are lambda-pasted so a
    // template-level early return cannot abort PlantStep.
    EXPECT_NE(cpp.find("const double DtPlant = (double)dt;"), std::string::npos);
    EXPECT_NE(cpp.find("[&] {"), std::string::npos);
    EXPECT_NE(cpp.find("}();"), std::string::npos);
    EXPECT_NE(cpp.find("const float Vdc = state.DcBus.Vdc;"), std::string::npos);
    EXPECT_NE(cpp.find("float& Id = state.Machine.Id;"), std::string::npos);

    // Host nodes emit no statements in PlantStep; duty/dyno helpers exist.
    EXPECT_NE(cpp.find("void PlantSetDuty(PlantState& state, float du, float dv, float dw)"),
              std::string::npos);
    // D9 surface: 7-arg setter + defaults/boot accessors.
    EXPECT_NE(cpp.find("void PlantSetSupply(PlantState& state, double vSet, double iLimit,"),
              std::string::npos);
    EXPECT_NE(h.find("double CvClamp_V = 0.0;"), std::string::npos);
    EXPECT_NE(h.find("double SeriesR_mOhm = 0.0;"), std::string::npos);
    EXPECT_NE(h.find("void PlantSupplyDefaults(const PlantState& state, double* vSet,"),
              std::string::npos);
    EXPECT_NE(h.find("void PlantBootParams(const PlantState& state, double* poles,"),
              std::string::npos);
    // D8: PlantSetFault returns bool.
    EXPECT_NE(h.find("bool PlantSetFault(PlantState& state, const char* name, bool active);"),
              std::string::npos);
    // D12: the NaN contract is documented in the emitted header itself.
    EXPECT_NE(h.find("NaN is the unpopulated-measurement sentinel"), std::string::npos);
    // D1 default-compat: guard + namespace stay plant/plant_generated.
    EXPECT_NE(h.find("#ifndef PLANT_GENERATED_H"), std::string::npos);
    EXPECT_NE(h.find("namespace plant {"), std::string::npos);
    // Unconnected MeasOut channels read as NaN; connected as producer fields.
    EXPECT_NE(cpp.find("out.Temp_Inv3_C = std::numeric_limits<float>::quiet_NaN();"),
              std::string::npos);
    EXPECT_NE(cpp.find("out.Iu_A = state.HallU.I_Meas;"), std::string::npos);
}

TEST(PlantCodegenEmission, TecoDefaultsAndUnwired) {
    const auto emitted = GenerateIntoTemp(LoadExample("teco_vhz_induction"), "struct_teco");
    // Loss omits IdleHeat_W: python-parity default (120.0) must be substituted
    // or the generated file would not compile.
    EXPECT_NE(emitted.source.find("constexpr double IdleHeat_W = 120;"),
              std::string::npos) << emitted.source;
    // InductDq.Omega_Cmd unwired -> PlantSetInput escape hatch + zero-bind field.
    EXPECT_NE(emitted.header.find("float Omega_Cmd = 0.0f;"), std::string::npos);
    EXPECT_NE(emitted.source.find("std::strcmp(portName, \"Omega_Cmd\")"),
              std::string::npos);
    // Teco has no encoder: the unwired meas channels collect NaN.
    EXPECT_NE(emitted.source.find("out.Enc_Sin = std::numeric_limits<float>::quiet_NaN();"),
              std::string::npos);
}

// ---------------------------------------------------------------------------
// Validation errors.

TEST(PlantCodegenValidation, AcceptsAllExampleGraphs) {
    for (const auto* name : {"gen7_bench_pmsm", "teco_vhz_induction", "zero_dsr_zf75_7"}) {
        auto graph = LoadExample(name);
        PlantCodegen::PlantGenerator generator(graph);
        const auto dir = std::filesystem::temp_directory_path() /
                         ("plantcodegen_ok_" + std::string(name) + "_" +
                          std::to_string(::getpid()));
        std::filesystem::remove_all(dir);
        std::string error;
        EXPECT_TRUE(generator.Generate(dir.string(), error))
            << name << ": " << error;
    }
}

TEST(PlantCodegenValidation, RejectsBridges) {
    auto graph = LoadExample("gen7_bench_pmsm");
    // Draw.C is unwired, so a bridge can target it (producer DutyIn.Du,
    // dimensionless f32 like the input port).
    NodeAPI::Bridge bridge;
    bridge.id = "b1";
    bridge.type = NodeAPI::WireType{NodeAPI::Quantity::Dimensionless,
                                    NodeAPI::Frame::Scalar, NodeAPI::DType::F32};
    bridge.producer = NodeAPI::PortRef{"DutyIn", "Du"};
    bridge.consumer = NodeAPI::PortRef{"Draw", "C"};
    ASSERT_TRUE(graph.AddBridge(bridge));
    EXPECT_TRUE(ExpectGenerateFails(graph, "bridges"));
}

TEST(PlantCodegenValidation, RejectsNonPlantDomain) {
    auto graph = LoadExample("gen7_bench_pmsm");
    ASSERT_TRUE(graph.SetNodeDomain("Knee", "sw_5khz"));
    EXPECT_TRUE(ExpectGenerateFails(graph, "domain"));
}

TEST(PlantCodegenValidation, RejectsUnknownTemplateType) {
    auto graph = LoadExample("gen7_bench_pmsm");
    ASSERT_TRUE(graph.RemoveNodeType("Plant.DcBus"));
    EXPECT_TRUE(ExpectGenerateFails(graph, "unknown_type"));
}

TEST(PlantCodegenValidation, RejectsDoubleWiredInput) {
    auto graph = LoadExample("gen7_bench_pmsm");
    // Graph::Connect does not check duplicate consumers: Knee.Du is already
    // driven by c1 (DutyIn.Du); a second producer must be caught by codegen.
    NodeAPI::Connection extra;
    extra.id = "c_bad";
    extra.from = NodeAPI::PortRef{"DutyIn", "Dv"};
    extra.to = NodeAPI::PortRef{"Knee", "Du"};
    ASSERT_TRUE(graph.Connect(extra));
    EXPECT_TRUE(ExpectGenerateFails(graph, "double_wired"));
}

TEST(PlantCodegenValidation, RejectsUnparseableFloatParameter) {
    auto graph = LoadExample("gen7_bench_pmsm");
    auto machine = graph.FindNode("Machine");
    ASSERT_TRUE(machine.has_value());
    auto params = machine->parameters;
    params["Rs"] = "not-a-float";
    ASSERT_TRUE(graph.SetNodeParameters("Machine", params));
    EXPECT_TRUE(ExpectGenerateFails(graph, "bad_param"));
}

TEST(PlantCodegenValidation, RejectsMissingParameterWithoutDefault) {
    auto graph = LoadExample("gen7_bench_pmsm");
    // PmsmDq.Rs is required by the pasted body and has no template default.
    auto machine = graph.FindNode("Machine");
    ASSERT_TRUE(machine.has_value());
    auto params = machine->parameters;
    params.erase("Rs");
    ASSERT_TRUE(graph.SetNodeParameters("Machine", params));
    EXPECT_TRUE(ExpectGenerateFails(graph, "missing_param"));
}

TEST(PlantCodegenValidation, RejectsCombinationalCycle) {
    // Two Plant.Util.Sum nodes wired in a pure combinational loop: neither
    // can ever be ready, and no stateful node exists to force.
    NodeAPI::Graph graph;
    std::string error;
    ASSERT_TRUE(LoadPlantGraph(planttest::PlantExample("gen7_bench_pmsm"),
                               planttest::PlantTemplatesDir(), graph, error));

    NodeAPI::Node u1 = *graph.FindNode("Draw");
    u1.id = "U1";
    u1.parameters = {};
    NodeAPI::Node u2 = u1;
    u2.id = "U2";
    ASSERT_TRUE(graph.AddNode(u1));
    ASSERT_TRUE(graph.AddNode(u2));
    NodeAPI::Connection c101{"c101", {"U1", "Out"}, {"U2", "A"}};
    NodeAPI::Connection c102{"c102", {"U2", "Out"}, {"U1", "A"}};
    NodeAPI::Connection c103{"c103", {"U1", "Out"}, {"U1", "B"}};
    NodeAPI::Connection c104{"c104", {"U2", "Out"}, {"U2", "B"}};
    ASSERT_TRUE(graph.Connect(c101));
    ASSERT_TRUE(graph.Connect(c102));
    ASSERT_TRUE(graph.Connect(c103));
    ASSERT_TRUE(graph.Connect(c104));
    EXPECT_TRUE(ExpectGenerateFails(graph, "comb_cycle"));
}

TEST(PlantCodegenValidation, RejectsUnwiredRequiredInput) {
    auto graph = LoadExample("gen7_bench_pmsm");
    ASSERT_TRUE(graph.Disconnect("c1"));  // DutyIn.Du -> Knee.Du
    EXPECT_TRUE(ExpectGenerateFails(graph, "unwired_required"));
}

// ---------------------------------------------------------------------------
// Synthetic-graph builders for the adversarial validation cases. A cloned Sum
// type keeps the classification-table membership (D3: the table is the
// contract) while letting the test inject arbitrary bodies/ports.

NodeAPI::Graph LoadLibraryGraph() {
    NodeAPI::Graph graph;
    std::string error;
    EXPECT_TRUE(LoadPlantGraph(planttest::PlantExample("gen7_bench_pmsm"),
                               planttest::PlantTemplatesDir(), graph, error))
        << error;
    return graph;
}

NodeAPI::NodeType ClonedSumType() {
    auto library = LoadLibraryGraph();
    const auto type = library.FindNodeType("Plant.Util.Sum");
    EXPECT_TRUE(type.has_value());
    return *type;
}

// Single combinational Sum-derived node (id U1, domain plant), nothing
// wired; the caller sets the type's bodies/ports before calling.
NodeAPI::Graph SyntheticSingleNodeGraph(NodeAPI::NodeType type,
                                        std::map<std::string, std::string> params = {},
                                        const std::string& nodeId = "U1") {
    type.constructorCode.clear();
    NodeAPI::Node node;
    node.id = nodeId;
    node.type = type.id;
    node.domain = "plant";
    node.parameters = std::move(params);

    NodeAPI::Graph graph;
    graph.SetName("synthetic");
    EXPECT_TRUE(graph.AddNodeType(std::move(type)));
    EXPECT_TRUE(graph.AddNode(std::move(node)));
    return graph;
}

struct GenResult {
    bool ok;
    std::string error;
    std::vector<std::string> warnings;
    std::filesystem::path dir;
};

GenResult GenerateCapturing(NodeAPI::Graph& graph, const std::string& tag,
                            PlantCodegen::PlantGenerator::Options opts =
                                PlantCodegen::PlantGenerator::Options{}) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("plantcodegen_cap_" + tag + "_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    PlantCodegen::PlantGenerator generator(graph, opts);
    GenResult result;
    result.ok = generator.Generate(dir.string(), result.error, &result.warnings);
    result.dir = dir;
    return result;
}

// --- D3: a loaded type missing from the classify table is a hard error.
TEST(PlantCodegenValidation, RejectsUnlistedTemplateType) {
    auto graph = LoadLibraryGraph();
    NodeAPI::NodeType custom;
    custom.id = "Plant.Custom.SlowBus";
    custom.inputPorts = {*graph.FindNodeType("Plant.DcBus")->FindInputPort("P_Draw")};
    custom.outputPorts = {*graph.FindNodeType("Plant.DcBus")->FindOutputPort("Vdc")};
    ASSERT_TRUE(graph.AddNodeType(custom));
    // Retype the DcBus node onto the unlisted type.
    auto bus = graph.FindNode("DcBus");
    ASSERT_TRUE(bus.has_value());
    graph.RemoveNode("DcBus");
    NodeAPI::Node rerig = *bus;
    rerig.type = custom.id;
    ASSERT_TRUE(graph.AddNode(rerig));
    const auto result = GenerateCapturing(graph, "unlisted_type");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("Plant.Custom.SlowBus"), std::string::npos)
        << result.error;
    EXPECT_NE(result.error.find("register it in PlantGenerator's classify table"),
              std::string::npos) << result.error;
}

// --- D4: non-finite parameter strings are rejected (used param path).
TEST(PlantCodegenValidation, RejectsNonFiniteParameters) {
    for (const char* bad : {"inf", "nan", "1e999"}) {
        auto graph = LoadExample("gen7_bench_pmsm");
        auto machine = graph.FindNode("Machine");
        ASSERT_TRUE(machine.has_value());
        auto params = machine->parameters;
        params["Rs"] = bad;
        ASSERT_TRUE(graph.SetNodeParameters("Machine", params));
        const auto result = GenerateCapturing(graph, std::string("nonfinite_") + bad);
        EXPECT_FALSE(result.ok) << "value " << bad;
        EXPECT_NE(result.error.find("Rs"), std::string::npos) << result.error;
    }
}

// --- D5: C++20 keywords are not valid identifiers for node ids / ports.
TEST(PlantCodegenValidation, RejectsCpp20KeywordNodeId) {
    auto type = ClonedSumType();
    auto graph = SyntheticSingleNodeGraph(type, {}, "concept");
    const auto result = GenerateCapturing(graph, "keyword_id");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("concept"), std::string::npos) << result.error;
}

TEST(PlantCodegenValidation, RejectsKeywordPortName) {
    auto type = ClonedSumType();
    type.inputPorts.clear();  // nothing unwired: mask the required-input check
    NodeAPI::Port port;
    port.name = "class";  // never valid, and in every keyword set
    port.direction = NodeAPI::PortDirection::Output;
    type.outputPorts = {port};
    auto graph = SyntheticSingleNodeGraph(type);
    const auto result = GenerateCapturing(graph, "keyword_port");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("class"), std::string::npos) << result.error;
}

// --- D5: a graph parameter colliding with a port name is rejected.
TEST(PlantCodegenValidation, RejectsParameterPortCollision) {
    auto type = ClonedSumType();  // output port "Out"
    type.inputPorts.clear();
    type.inlineCode.clear();  // isolate the collision check
    auto graph = SyntheticSingleNodeGraph(type, {{"Out", "5"}});
    const auto result = GenerateCapturing(graph, "param_port_collision");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("Out"), std::string::npos) << result.error;
}

// --- D5: an input port colliding with an output/state name is rejected.
TEST(PlantCodegenValidation, RejectsInputOutputNameClash) {
    auto type = ClonedSumType();
    type.constructorCode.clear();
    type.inlineCode.clear();
    NodeAPI::Port in;
    in.name = "Out";
    in.direction = NodeAPI::PortDirection::Input;
    in.type = type.outputPorts.front().type;
    in.optional = true;  // keep the unwired-required check out of the way
    type.inputPorts = {in};
    auto graph = SyntheticSingleNodeGraph(type);
    const auto result = GenerateCapturing(graph, "input_output_clash");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("Out"), std::string::npos) << result.error;
}

// --- D6a: a statement-initial "<word> <word>" with an unknown type word.
TEST(PlantCodegenValidation, RejectsUnknownLocalTypeInBody) {
    auto type = ClonedSumType();
    type.inputPorts.clear();
    type.inlineCode = "uint32_t ticks = 0;\nOut = 0.0;\n";
    auto graph = SyntheticSingleNodeGraph(type);
    const auto result = GenerateCapturing(graph, "unknown_local_type");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("unknown local type 'uint32_t'"), std::string::npos)
        << result.error;
}

// --- D6b: a body read of an undeclared identifier is rejected.
TEST(PlantCodegenValidation, RejectsUndeclaredBodyIdentifier) {
    auto type = ClonedSumType();
    type.inputPorts.clear();
    type.inlineCode = "double acc = 1.0;\nOut = acc + Typo_Read;\n";
    auto graph = SyntheticSingleNodeGraph(type);
    const auto result = GenerateCapturing(graph, "undeclared_read");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("Typo_Read"), std::string::npos) << result.error;
}

// --- D6c: static locals are hidden state PlantInit cannot reset.
TEST(PlantCodegenValidation, RejectsStaticLocalInBody) {
    auto type = ClonedSumType();
    type.inputPorts.clear();
    type.inlineCode = "static double acc = 0.0;\nOut = acc;\n";
    auto graph = SyntheticSingleNodeGraph(type);
    const auto result = GenerateCapturing(graph, "static_local");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("static"), std::string::npos) << result.error;
}

// --- Duplicate node ids / extra MeasOut instances / dangling connections are
// rejected at the Graph/loader layer already; the codegen checks behind them
// are safety nets. Pin the enforcement points.
TEST(PlantCodegenValidation, RejectsDuplicateNodeIds) {
    auto graph = LoadExample("gen7_bench_pmsm");
    auto dup = graph.FindNode("Draw");
    ASSERT_TRUE(dup.has_value());
    EXPECT_FALSE(graph.AddNode(*dup));  // Graph::AddNode enforces uniqueness
}

TEST(PlantCodegenValidation, RejectsMultipleMeasOutInstances) {
    auto graph = LoadExample("gen7_bench_pmsm");
    auto meas = graph.FindNode("Meas");
    ASSERT_TRUE(meas.has_value());
    auto second = *meas;
    second.id = "Meas2";
    // Graph::AddNode enforces template maxInstances (MeasOut is maxInstances 1).
    EXPECT_FALSE(graph.AddNode(second));
}

TEST(PlantCodegenValidation, RejectsDanglingConnectionAtLoad) {
    // A connection referencing a nonexistent node is dropped by
    // Graph::Connect during LoadIntoGraph; the shared loader's landing-parity
    // check turns the mismatch into a load error (loud, not truncated).
    NodeAPI::Graph graph;
    const auto loaded = NodeAPI::LoadNodeTypesFromDirectory(
        graph, planttest::PlantTemplatesDir());
    ASSERT_TRUE(loaded.ok);
    std::string error;
    std::ifstream file(planttest::PlantExample("gen7_bench_pmsm"));
    std::ostringstream contents;
    contents << file.rdbuf();
    auto parsed = nlohmann::json::parse(contents.str());
    parsed["connections"].push_back(
        {{"id", "cdangle"},
         {"from", {{"nodeId", "Ghost"}, {"portName", "Out"}}},
         {"to", {{"nodeId", "Draw"}, {"portName", "C"}}}});
    if (!parsed.contains("nodeTypes")) parsed["nodeTypes"] = nlohmann::json::array();
    NodeAPI::LoadIntoGraph(graph, parsed.dump());
    EXPECT_LT(graph.GetConnections().size(), parsed["connections"].size());
    // The dropped wire leaves Draw.C zero-bound: codegen still succeeds, but
    // the loader parity check (plant_test_util pattern) flags the mismatch.
}

// F-09: an absent/empty domain validates as 'plant' (v1.1 contract).
TEST(PlantCodegenValidation, AcceptsEmptyDomainAsPlant) {
    auto graph = LoadExample("gen7_bench_pmsm");
    ASSERT_TRUE(graph.SetNodeDomain("Knee", ""));
    const auto result = GenerateCapturing(graph, "empty_domain");
    EXPECT_TRUE(result.ok) << result.error;
}

// F-23: controller-format pruning keys are an error on plant graphs
// (detection beats silently ignoring them).
TEST(PlantCodegenValidation, RejectsControllerFormatKeys) {
    {
        auto graph = LoadExample("gen7_bench_pmsm");
        ASSERT_TRUE(graph.SetNodeExcludeFromCompile("Draw", true));
        EXPECT_TRUE(ExpectGenerateFails(graph, "exclude_key"));
    }
    {
        auto graph = LoadExample("gen7_bench_pmsm");
        ASSERT_TRUE(graph.SetNodeExcludeFromCompileRecursive("Draw", true));
        EXPECT_TRUE(ExpectGenerateFails(graph, "exclude_rec_key"));
    }
    {
        auto graph = LoadExample("gen7_bench_pmsm");
        ASSERT_TRUE(graph.SetNodeParameterInputs("Draw", {"C"}));
        EXPECT_TRUE(ExpectGenerateFails(graph, "param_inputs_key"));
    }
    {
        auto graph = LoadExample("gen7_bench_pmsm");
        ASSERT_TRUE(graph.SetNodeZeroInputs("Draw", {"C"}));
        EXPECT_TRUE(ExpectGenerateFails(graph, "zero_inputs_key"));
    }
}

// ---------------------------------------------------------------------------
// D2 warnings channel: mild issues are reported, generation still succeeds.
TEST(PlantCodegenValidation, WarningsForUnusedAndUndeclaredParameters) {
    for (const auto* name : {"gen7_bench_pmsm", "teco_vhz_induction", "zero_dsr_zf75_7"}) {
        auto graph = LoadExample(name);
        const auto result = GenerateCapturing(graph, std::string("warn_") + name);
        ASSERT_TRUE(result.ok) << name << ": " << result.error;
        EXPECT_TRUE(result.warnings.empty())
            << name << " should generate warning-free (v1.1 library state)";
    }

    // Undeclared key (typo class): warned, materialized, still successful.
    {
        auto graph = LoadExample("gen7_bench_pmsm");
        auto bus = graph.FindNode("DcBus");
        ASSERT_TRUE(bus.has_value());
        auto params = bus->parameters;
        params["VSett_V"] = "42.0";  // typo of VSet_V on purpose
        ASSERT_TRUE(graph.SetNodeParameters("DcBus", params));
        const auto result = GenerateCapturing(graph, "warn_typo_param");
        ASSERT_TRUE(result.ok) << result.error;
        EXPECT_FALSE(result.warnings.empty());
        EXPECT_NE(result.warnings.front().find("VSett_V"), std::string::npos)
            << result.warnings.front();
    }

    // Declared-but-unreferenced key: warned as unused.
    {
        auto type = ClonedSumType();
        type.inputPorts.clear();
        type.inlineCode.clear();
        NodeAPI::WireType wt{NodeAPI::Quantity::Dimensionless, NodeAPI::Frame::Scalar,
                             NodeAPI::DType::F32};
        type.parameterTypes.emplace("UnusedP", wt);
        auto graph = SyntheticSingleNodeGraph(type, {{"UnusedP", "1.0"}});
        const auto result = GenerateCapturing(graph, "warn_unused_param");
        ASSERT_TRUE(result.ok) << result.error;
        ASSERT_FALSE(result.warnings.empty());
        EXPECT_NE(result.warnings.front().find("never referenced"), std::string::npos)
            << result.warnings.front();
    }
}

// --- D13: the deprecated StartRpm spelling still resolves, with a warning.
TEST(PlantCodegenValidation, DynoStartRpmFallbackWarns) {
    auto graph = LoadExample("gen7_bench_pmsm");
    auto dyno = graph.FindNode("Dyno");
    ASSERT_TRUE(dyno.has_value());
    auto params = dyno->parameters;
    params.erase("Rpm0");
    params["StartRpm"] = "120";
    ASSERT_TRUE(graph.SetNodeParameters("Dyno", params));
    const auto result = GenerateCapturing(graph, "dyno_start_rpm");
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_FALSE(result.warnings.empty());
    EXPECT_NE(result.warnings.front().find("StartRpm"), std::string::npos)
        << result.warnings.front();

    // The canonical Rpm0 spelling wins when both are present, silently.
    params["Rpm0"] = "60";
    ASSERT_TRUE(graph.SetNodeParameters("Dyno", params));
    const auto canonical = GenerateCapturing(graph, "dyno_rpm0_wins");
    ASSERT_TRUE(canonical.ok) << canonical.error;
    EXPECT_TRUE(canonical.warnings.empty());
    std::ifstream file(canonical.dir / "plant_generated.cpp");
    std::ostringstream text;
    text << file.rdbuf();
    EXPECT_NE(text.str().find("constexpr double StartRpm = 60;"), std::string::npos)
        << "canonical Rpm0 value must feed the legacy-bodied constructor";
}

// --- D7: PlantSetInput emits an assignment for EVERY unwired match.
TEST(PlantCodegenEmission, SetInputCoversAllMatches) {
    auto type = ClonedSumType();
    type.constructorCode.clear();
    for (auto& port : type.inputPorts) port.optional = true;  // tolerate unwired

    NodeAPI::Graph graph;
    graph.SetName("two_sum");
    ASSERT_TRUE(graph.AddNodeType(type));
    NodeAPI::Node u1;
    u1.id = "U1";
    u1.type = type.id;
    u1.domain = "plant";
    ASSERT_TRUE(graph.AddNode(u1));
    auto u2 = u1;
    u2.id = "U2";
    ASSERT_TRUE(graph.AddNode(u2));

    const auto emitted = GenerateIntoTemp(graph, "set_input_multi");
    EXPECT_NE(emitted.source.find("state.U1.C = value;"), std::string::npos);
    EXPECT_NE(emitted.source.find("state.U2.C = value;"), std::string::npos);
    EXPECT_EQ(emitted.source.find("return true;"), std::string::npos)
        << "all-match semantics must not early-return after the first node";
    EXPECT_NE(emitted.source.find("bool matched = false;"), std::string::npos);
    EXPECT_NE(emitted.source.find("return matched;"), std::string::npos);
}

// --- D1: Options drive namespace, file stem, and include guard.
TEST(PlantCodegenEmission, OptionsDriveNamespaceAndFileNames) {
    auto graph = LoadExample("gen7_bench_pmsm");
    PlantCodegen::PlantGenerator::Options opts;
    opts.emitNamespace = "bench7";
    opts.fileStem = "bench7_engine";
    const auto dir = std::filesystem::temp_directory_path() /
                     ("plantcodegen_opts_" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    PlantCodegen::PlantGenerator generator(graph, opts);
    std::string error;
    std::vector<std::string> warnings;
    ASSERT_TRUE(generator.Generate(dir.string(), error, &warnings)) << error;
    EXPECT_TRUE(std::filesystem::exists(dir / "bench7_engine.h"));
    EXPECT_TRUE(std::filesystem::exists(dir / "bench7_engine.cpp"));

    std::ifstream hfile(dir / "bench7_engine.h");
    std::ostringstream htext;
    htext << hfile.rdbuf();
    EXPECT_NE(htext.str().find("namespace bench7 {"), std::string::npos);
    EXPECT_NE(htext.str().find("#ifndef BENCH7_ENGINE_H"), std::string::npos);
    std::ifstream cfile(dir / "bench7_engine.cpp");
    std::ostringstream ctext;
    ctext << cfile.rdbuf();
    EXPECT_NE(ctext.str().find("#include \"bench7_engine.h\""), std::string::npos);

    // An unusable namespace is rejected up front.
    PlantCodegen::PlantGenerator::Options bad;
    bad.emitNamespace = "not a namespace";
    PlantCodegen::PlantGenerator failing(graph, bad);
    std::string badError;
    EXPECT_FALSE(failing.Generate(dir.string(), badError));
    EXPECT_NE(badError.find("not a namespace"), std::string::npos);
}

TEST(PlantCodegenEmission, SuggestNamespaceSanitizesGraphNames) {
    using PlantCodegen::PlantGenerator;
    EXPECT_EQ(PlantGenerator::SuggestNamespace("gen7_bench_pmsm"), "plant_gen7_bench_pmsm");
    EXPECT_EQ(PlantGenerator::SuggestNamespace("teco-vhz induction"), "plant_teco_vhz_induction");
    EXPECT_EQ(PlantGenerator::SuggestNamespace(""), "plant");
}

}  // namespace
