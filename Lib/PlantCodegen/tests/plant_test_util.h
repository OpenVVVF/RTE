#pragma once

// Shared loader + scenario helpers for PlantCodegen tests and the build-time
// golden generator driver (tests/gen_main.cpp).

#include "NodeAPI/Graph.h"
#include "NodeAPI/NodeTemplates.h"
#include "NodeAPI/Serialization.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace planttest {

// Loads a PlantExamples graph: templates from Assets/PlantTemplates into the
// graph first (so ports/types/inline bodies resolve), then the instance JSON
// via NodeAPI's sanctioned loader. The example files embed no "nodeTypes"
// array, so an empty one is injected — LoadIntoGraph requires it.
inline bool LoadPlantGraph(const std::filesystem::path& graphPath,
                           const std::filesystem::path& templatesDir,
                           NodeAPI::Graph& graph, std::string& error) {
    const auto loaded = NodeAPI::LoadNodeTypesFromDirectory(graph, templatesDir);
    if (!loaded.ok) {
        std::ostringstream text;
        text << "template loading failed for " << templatesDir;
        for (const auto& item : loaded.errors) text << "; " << item;
        error = text.str();
        return false;
    }

    std::ifstream file(graphPath);
    if (!file) {
        error = "could not open graph: " + graphPath.string();
        return false;
    }
    std::ostringstream contents;
    contents << file.rdbuf();

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(contents.str());
    } catch (const std::exception& e) {
        error = "could not parse graph JSON " + graphPath.string() + ": " + e.what();
        return false;
    }
    if (!parsed.contains("nodeTypes")) parsed["nodeTypes"] = nlohmann::json::array();
    try {
        NodeAPI::LoadIntoGraph(graph, parsed.dump());
    } catch (const std::exception& e) {
        error = "could not load graph " + graphPath.string() + ": " + e.what();
        return false;
    }

    // The Graph mutators ignore their bool results inside LoadIntoGraph;
    // every node/connection must have landed.
    if (graph.GetNodes().size() != parsed["nodes"].size()) {
        error = "graph " + graphPath.string() + ": only " +
                std::to_string(graph.GetNodes().size()) + " of " +
                std::to_string(parsed["nodes"].size()) + " nodes were accepted";
        return false;
    }
    if (graph.GetConnections().size() != parsed["connections"].size()) {
        error = "graph " + graphPath.string() + ": only " +
                std::to_string(graph.GetConnections().size()) + " of " +
                std::to_string(parsed["connections"].size()) +
                " connections were accepted";
        return false;
    }
    return true;
}

inline std::filesystem::path AssetsDir() {
    return std::filesystem::path(RTE_SOURCE_DIR) / "Assets";
}

inline std::filesystem::path PlantExample(const std::string& name) {
    return AssetsDir() / "PlantExamples" / (name + ".json");
}

inline std::filesystem::path PlantTemplatesDir() {
    return AssetsDir() / "PlantTemplates";
}

}  // namespace planttest
