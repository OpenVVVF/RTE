#pragma once

#include <string>

namespace NodeGUI {

// Routing kind of a graph document. The engines never mix Plant.* and
// controller types in one file, so a single Plant.* node makes the document a
// plant graph (mirrors the historical GraphContainsPlantNodes: readable JSON
// object + any node "type" starting "Plant." => Plant, else Controller).
// Unknown means the file cannot be read or is not a JSON object; callers
// treat it like the old 'false' result (controller path) and let the real
// loader surface the parse error.
enum class GraphKind { Controller, Plant, Unknown };

GraphKind GraphKindOf(const std::string& path);

}  // namespace NodeGUI
