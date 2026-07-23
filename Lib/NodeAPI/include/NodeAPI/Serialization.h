#pragma once

#include "NodeAPI/Graph.h"

#include <string>
#include <string_view>

namespace NodeAPI {

std::string SaveToJson(const Graph& graph);
Graph LoadFromJson(std::string_view jsonText);

// Populate an existing Graph from JSON. Useful when templates have already been
// loaded into the graph before the instance graph is parsed.
void LoadIntoGraph(Graph& graph, std::string_view jsonText);

// Parse a single NodeType from its JSON representation. Throws on invalid input.
NodeType NodeTypeFromJson(std::string_view jsonText);

}  // namespace NodeAPI
