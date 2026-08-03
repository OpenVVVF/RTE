#include "NodeAPI/NodeTemplates.h"

#include "NodeAPI/Serialization.h"

#include <nlohmann/json.hpp>

#include <fstream>

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
    if (!std::filesystem::is_regular_file(path)) {
        return "";
    }
    auto text = ReadFile(path);
    return text ? *text : "";
}

bool LoadFolderTemplate(Graph& graph,
                        const std::filesystem::path& dir,
                        LoadResult& result) {
    const auto nodeJsonPath = dir / "node.json";
    if (!std::filesystem::is_regular_file(nodeJsonPath)) {
        // Not a folder-based template; silently skip.
        return true;
    }

    const auto fileText = ReadFile(nodeJsonPath);
    if (!fileText) {
        result.ok = false;
        result.errors.push_back("failed to read " + nodeJsonPath.string());
        return false;
    }

    json parsed;
    try {
        parsed = json::parse(*fileText);
    } catch (const std::exception& e) {
        result.ok = false;
        result.errors.push_back("JSON parse error in " + nodeJsonPath.string() + ": " + e.what());
        return false;
    }

    if (!parsed.is_object()) {
        result.ok = false;
        result.errors.push_back("unexpected JSON value in " + nodeJsonPath.string());
        return false;
    }

    const auto schema = CheckSchemaVersion(parsed.value("schemaVersion", 0),
                                           kTemplateSchemaVersion,
                                           "node template");
    if (schema.status != SchemaStatus::kCurrent) {
        result.warnings.push_back(nodeJsonPath.filename().string() + " (" + dir.filename().string()
                                  + "): " + schema.warning);
    }

    NodeType nodeType;
    try {
        nodeType = NodeTypeFromJson(parsed.dump());
    } catch (const std::exception& e) {
        result.ok = false;
        result.errors.push_back("invalid NodeType in " + nodeJsonPath.string() + ": " + e.what());
        return false;
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
        return false;
    }

    ++result.typesLoaded;
    return true;
}

}  // namespace

LoadResult LoadNodeTypesFromDirectory(Graph& graph, const std::filesystem::path& directory) {
    LoadResult result;

    if (!std::filesystem::exists(directory)) {
        result.ok = false;
        result.errors.push_back("template directory does not exist: " + directory.string());
        return result;
    }
    if (!std::filesystem::is_directory(directory)) {
        result.ok = false;
        result.errors.push_back("template path is not a directory: " + directory.string());
        return result;
    }

    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_directory()) {
            continue;
        }

        if (LoadFolderTemplate(graph, entry.path(), result)) {
            ++result.filesLoaded;
        }
    }

    return result;
}

}  // namespace NodeAPI
