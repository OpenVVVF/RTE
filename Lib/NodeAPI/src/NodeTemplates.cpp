#include "NodeAPI/NodeTemplates.h"

#include "NodeAPI/Serialization.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <system_error>

namespace NodeAPI {

namespace {

using json = nlohmann::json;

std::optional<std::string> ReadFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::in | std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }
    std::string contents((std::istreambuf_iterator<char>(stream)),
                          std::istreambuf_iterator<char>());
    return contents;
}

// Read a code block file if it exists. Returns empty string on missing file.
std::string ReadCodeBlock(const std::filesystem::path& dir, const std::string& filename) {
    const auto path = dir / filename;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return "";
    }
    auto text = ReadFile(path);
    return text ? *text : "";
}

// Extract a schema version without throwing on a wrong-typed value.
int SchemaVersionOf(const json& j) {
    if (const auto it = j.find("schemaVersion"); it != j.end() && it->is_number_integer()) {
        return it->get<int>();
    }
    return 0;
}

// Insert one parsed NodeType object into the graph, recording failures.
void AddParsedNodeType(Graph& graph,
                       const json& item,
                       const std::filesystem::path& source,
                       LoadResult& result) {
    NodeType nodeType;
    try {
        nodeType = NodeTypeFromJson(item);
    } catch (const std::exception& e) {
        result.ok = false;
        result.errors.push_back("invalid NodeType in " + source.string() + ": " + e.what());
        return;
    }
    if (!graph.AddNodeType(nodeType)) {
        result.ok = false;
        result.errors.push_back("failed to add node type '" + nodeType.id +
                                "' from " + source.string());
        return;
    }
    ++result.typesLoaded;
}

void LoadFolderTemplate(Graph& graph,
                        const std::filesystem::path& dir,
                        LoadResult& result) {
    const auto nodeJsonPath = dir / "node.json";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(nodeJsonPath, ec)) {
        // Not a folder-based template; silently skip.
        return;
    }

    const auto fileText = ReadFile(nodeJsonPath);
    if (!fileText) {
        result.ok = false;
        result.errors.push_back("failed to read " + nodeJsonPath.string());
        return;
    }

    json parsed;
    try {
        parsed = json::parse(*fileText);
    } catch (const std::exception& e) {
        result.ok = false;
        result.errors.push_back("JSON parse error in " + nodeJsonPath.string() + ": " + e.what());
        return;
    }

    if (!parsed.is_object()) {
        result.ok = false;
        result.errors.push_back("unexpected JSON value in " + nodeJsonPath.string());
        return;
    }

    ++result.filesLoaded;

    const auto schema = CheckSchemaVersion(SchemaVersionOf(parsed),
                                           kTemplateSchemaVersion,
                                           "node template");
    if (schema.status != SchemaStatus::kCurrent) {
        result.warnings.push_back(nodeJsonPath.filename().string() + " (" + dir.filename().string()
                                  + "): " + schema.warning);
    }

    NodeType nodeType;
    try {
        nodeType = NodeTypeFromJson(parsed);
    } catch (const std::exception& e) {
        result.ok = false;
        result.errors.push_back("invalid NodeType in " + nodeJsonPath.string() + ": " + e.what());
        return;
    }

    // Load optional code block files from the same folder.
    nodeType.classHeader = ReadCodeBlock(dir, "class_header.h");
    nodeType.classDefinition = ReadCodeBlock(dir, "class_definition.cpp");
    nodeType.constructorCode = ReadCodeBlock(dir, "constructor.cpp");
    nodeType.inlineCode = ReadCodeBlock(dir, "inline.cpp");

    if (!graph.AddNodeType(nodeType)) {
        result.ok = false;
        result.errors.push_back("failed to add node type '" + nodeType.id +
                                "' from " + dir.string());
        return;
    }

    ++result.typesLoaded;
}

void LoadFileTemplate(Graph& graph,
                      const std::filesystem::path& path,
                      LoadResult& result) {
    const auto fileText = ReadFile(path);
    if (!fileText) {
        result.ok = false;
        result.errors.push_back("failed to read " + path.string());
        return;
    }

    json parsed;
    try {
        parsed = json::parse(*fileText);
    } catch (const std::exception& e) {
        result.ok = false;
        result.errors.push_back("JSON parse error in " + path.string() + ": " + e.what());
        return;
    }

    if (!parsed.is_object() && !parsed.is_array()) {
        result.ok = false;
        result.errors.push_back("unexpected JSON value in " + path.string());
        return;
    }

    ++result.filesLoaded;

    const auto checkSchema = [&path, &result](const json& item) {
        const auto schema = CheckSchemaVersion(SchemaVersionOf(item),
                                               kTemplateSchemaVersion,
                                               "node template");
        if (schema.status != SchemaStatus::kCurrent) {
            result.warnings.push_back(path.filename().string() + ": " + schema.warning);
        }
    };

    // Each file may contain a single NodeType object or an array of them.
    if (parsed.is_array()) {
        for (const auto& item : parsed) {
            if (!item.is_object()) {
                result.ok = false;
                result.errors.push_back("unexpected array item in " + path.string());
                continue;
            }
            checkSchema(item);
            AddParsedNodeType(graph, item, path, result);
        }
        return;
    }

    checkSchema(parsed);
    AddParsedNodeType(graph, parsed, path, result);
}

}  // namespace

LoadResult LoadNodeTypesFromDirectory(Graph& graph, const std::filesystem::path& directory) {
    LoadResult result;

    std::error_code ec;
    if (!std::filesystem::exists(directory, ec) || ec) {
        result.ok = false;
        result.errors.push_back("template directory does not exist: " + directory.string());
        return result;
    }
    if (!std::filesystem::is_directory(directory, ec)) {
        result.ok = false;
        result.errors.push_back("template path is not a directory: " + directory.string());
        return result;
    }

    // Folder templates (a subdirectory holding node.json plus optional code
    // block files) and top-level *.json files (single NodeType or an array).
    for (std::filesystem::directory_iterator it(directory, ec), end; it != end && !ec;
         it.increment(ec)) {
        std::error_code entryEc;
        if (it->is_directory(entryEc)) {
            LoadFolderTemplate(graph, it->path(), result);
        } else if (it->is_regular_file(entryEc) && it->path().extension() == ".json") {
            LoadFileTemplate(graph, it->path(), result);
        }
    }
    if (ec) {
        result.ok = false;
        result.errors.push_back("failed to list " + directory.string() + ": " + ec.message());
    }

    // Per the contract on LoadResult::ok, loading zero types is a failure.
    if (result.typesLoaded == 0 && result.errors.empty()) {
        result.ok = false;
        result.errors.push_back("no node types found in " + directory.string());
    }

    return result;
}

}  // namespace NodeAPI
