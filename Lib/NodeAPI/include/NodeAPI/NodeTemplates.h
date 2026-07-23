#pragma once

#include "NodeAPI/Graph.h"

#include <filesystem>
#include <string>
#include <vector>

namespace NodeAPI {

// Result of loading node-template JSON files from a directory.
struct LoadResult {
    bool ok = true;                      // true if at least one type was loaded and no errors.
    std::size_t filesLoaded = 0;         // number of JSON files successfully parsed.
    std::size_t typesLoaded = 0;         // number of NodeType objects added to the graph.
    std::vector<std::string> errors;     // human-readable messages for failed files/insertions.
};

// Load every *.json file in `directory` into `graph` as NodeType entries.
//
// Each file may contain a single NodeType object or an array of NodeType objects.
// Existing node types with the same id are left untouched; an error is recorded
// and loading continues with the remaining files.
LoadResult LoadNodeTypesFromDirectory(Graph& graph, const std::filesystem::path& directory);

}  // namespace NodeAPI
