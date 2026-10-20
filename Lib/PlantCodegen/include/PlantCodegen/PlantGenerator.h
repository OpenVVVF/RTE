#pragma once

#include "NodeAPI/Graph.h"

#include <string>

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
    explicit PlantGenerator(const NodeAPI::Graph& graph);

    // Writes <outputDir>/plant_generated.h and plant_generated.cpp.
    // Returns false with a human-readable message on validation errors
    // (bridges, non-plant domains, dangling connections, duplicate ids,
    // unknown types, unparseable/unwired-but-required inputs, missing
    // parameters, unschedulable combinational cycles, maxInstances overruns).
    bool Generate(const std::string& outputDir, std::string& error) const;

private:
    const NodeAPI::Graph& graph_;
};

}  // namespace PlantCodegen
