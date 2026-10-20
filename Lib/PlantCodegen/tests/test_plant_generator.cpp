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
    // (PYTHONPATH=. python3 trace of PlantGraphSim._run_pass, 2026-10-20).
    // Knee/DcBus are the cycle-forced evaluations (PRE readings).
    const std::vector<std::string> expected = {
        "AbsOm", "Knee", "Machine", "Im", "HallU", "HallV", "HallW", "Enc",
        "Loss", "Draw", "DcBus", "Thermal", "VSense", "Meas",
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
        "Loss", "Draw", "DcBus", "Thermal", "VSense", "Meas",
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

    // Init: DutyIn zero-vector default, DcBus supply init, constructor paste.
    EXPECT_NE(cpp.find("state.DutyIn.Du = 50.0f;"), std::string::npos);
    EXPECT_NE(cpp.find("state.DcBus.VSet_V = 144.7;"), std::string::npos);
    EXPECT_NE(cpp.find("Id = Id0;"), std::string::npos);           // PmsmDq constructor
    EXPECT_NE(cpp.find("Lag = 0.0;"), std::string::npos);          // VSense constructor

    // Step bindings per the contract.
    EXPECT_NE(cpp.find("const double DtPlant = (double)dt;"), std::string::npos);
    EXPECT_NE(cpp.find("const float Vdc = state.DcBus.Vdc;"), std::string::npos);
    EXPECT_NE(cpp.find("float& Id = state.Machine.Id;"), std::string::npos);

    // Host nodes emit no statements in PlantStep; duty/dyno helpers exist.
    EXPECT_NE(cpp.find("void PlantSetDuty(PlantState& state, float du, float dv, float dw)"),
              std::string::npos);
    EXPECT_NE(cpp.find("void PlantSetSupply(PlantState& state"), std::string::npos);
    // Unconnected MeasOut channels read as NaN; connected as producer fields.
    EXPECT_NE(cpp.find("out.Vu_V = std::numeric_limits<float>::quiet_NaN();"),
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

}  // namespace
