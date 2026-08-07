#pragma once

#include "NodeAPI/Graph.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace NodeAPI {

// Schema version of the graph JSON written by SaveToJson. Bump this whenever
// the graph file format changes in a way older builds would misread; loaders
// compare it and warn on mismatch. Files without a version are treated as
// legacy (pre-versioning) and load silently.
inline constexpr int kGraphSchemaVersion = 1;

// Outcome of checking a file's schema version against the running build.
enum class SchemaStatus {
    kCurrent,  // matches this build (or file is pre-versioning legacy)
    kOlder,    // file is older than this build; loads, upgraded on next save
    kNewer,    // file is newer than this build; data may be missing/misread
};

struct SchemaCheck {
    SchemaStatus status = SchemaStatus::kCurrent;
    // Human-readable warning for kOlder/kNewer, empty otherwise.
    std::string warning;
};

// Reads just the schema version of a graph JSON document. Never throws:
// unparseable input is reported as kCurrent (the real parse reports errors).
SchemaCheck CheckGraphSchema(std::string_view jsonText);

// Shared version-check logic for graph and template files. `kind` is used in
// the warning text (e.g. "graph", "node template").
SchemaCheck CheckSchemaVersion(int fileVersion, int currentVersion, const char* kind);

std::string SaveToJson(const Graph& graph);
Graph LoadFromJson(std::string_view jsonText);

// Outcome of populating a Graph from JSON.
struct GraphLoadResult {
    bool ok = true;                     // true when every item was inserted into the graph.
    std::vector<std::string> failures;  // human-readable messages for rejected items.
};

// Populate an existing Graph from JSON. Useful when templates have already been
// loaded into the graph before the instance graph is parsed. Items rejected by
// the graph (duplicate ids, unknown types, occupied input ports, ...) are
// collected in the result instead of being silently dropped; result.ok is
// false if any item failed. Throws on malformed JSON.
GraphLoadResult LoadIntoGraph(Graph& graph, std::string_view jsonText);

// Parse a single NodeType from its JSON representation. Throws on invalid input.
NodeType NodeTypeFromJson(std::string_view jsonText);

// Parse a single NodeType from an already-parsed JSON value. Throws on invalid input.
NodeType NodeTypeFromJson(const nlohmann::json& j);

}  // namespace NodeAPI
