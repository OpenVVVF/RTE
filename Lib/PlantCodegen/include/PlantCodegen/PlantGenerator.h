#pragma once

#include "NodeAPI/Graph.h"

#include <string>
#include <vector>

namespace PlantCodegen {

// Emits a headless C++ plant engine (plant_generated.h/.cpp) from a plant
// nodegraph: a PlantState struct, PlantInit/PlantStep, the PlantMeas seam,
// and the PlantSet* host helpers. The evaluation order follows the
// Assets/PlantTemplates/README.md execution contract (frozen at codegen
// time; mirrors twin/plantgraph/plantgraph.py's PlantGraphSim._run_pass).
//
// The graph must already hold the node types used by its nodes
// (NodeAPI::LoadNodeTypesFromDirectory(graph, "Assets/PlantTemplates")).
class PlantGenerator {
public:
    // D1: emitted-code naming. Two plants can coexist in one binary only when
    // each is emitted with a distinct emitNamespace (and distinct fileStem
    // when the headers share an include path); the v1 defaults keep
    // compatibility with existing consumers (namespace plant, plant_generated).
    struct Options {
        std::string emitNamespace = "plant";
        std::string fileStem = "plant_generated";
    };

    explicit PlantGenerator(const NodeAPI::Graph& graph);
    PlantGenerator(const NodeAPI::Graph& graph, Options opts);

    // D1: suggests an emit namespace for a graph name: "plant_<name>"
    // sanitized to a C++ identifier (non-identifier chars become '_'). The
    // "plant_" prefix keeps the result valid even for leading-digit names.
    // "plant" is returned for names that sanitize to nothing usable.
    static std::string SuggestNamespace(const std::string& graphName);

    // Writes <outputDir>/<fileStem>.h and <fileStem>.cpp.
    // Returns false with a human-readable message on validation errors
    // (bridges, non-plant domains, dangling connections, duplicate ids,
    // unknown/unlisted types, unparseable/non-finite parameters, invalid or
    // colliding identifiers, unwired required inputs, missing parameters,
    // body ground-rule violations, unschedulable combinational cycles,
    // maxInstances overruns).
    // D2: when `warnings` is non-null, mild issues (unused or undeclared
    // graph parameters materialized into the struct, deprecated parameter
    // spellings) are appended there instead of failing the generation.
    bool Generate(const std::string& outputDir, std::string& error,
                  std::vector<std::string>* warnings = nullptr) const;

    const Options& GetOptions() const { return opts_; }

private:
    const NodeAPI::Graph& graph_;
    Options opts_;
};

}  // namespace PlantCodegen
