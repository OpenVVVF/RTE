#pragma once

#include "NodeAPI/Graph.h"

#include <string>
#include <string_view>

namespace NodeAPI {

std::string SaveToJson(const Graph& graph);
Graph LoadFromJson(std::string_view jsonText);

}  // namespace NodeAPI
