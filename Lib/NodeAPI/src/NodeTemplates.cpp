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
        if (!entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }

        const auto fileText = ReadFile(entry.path());
        if (!fileText) {
            result.ok = false;
            result.errors.push_back("failed to read " + entry.path().string());
            continue;
        }

        json parsed;
        try {
            parsed = json::parse(*fileText);
        } catch (const std::exception& e) {
            result.ok = false;
            result.errors.push_back("JSON parse error in " + entry.path().string() + ": " + e.what());
            continue;
        }

        std::vector<json> items;
        if (parsed.is_array()) {
            items.assign(parsed.begin(), parsed.end());
        } else if (parsed.is_object()) {
            items.push_back(parsed);
        } else {
            result.ok = false;
            result.errors.push_back("unexpected JSON value in " + entry.path().string());
            continue;
        }

        bool fileOk = true;
        for (const auto& item : items) {
            NodeType nodeType;
            try {
                nodeType = NodeTypeFromJson(item.dump());
            } catch (const std::exception& e) {
                result.ok = false;
                fileOk = false;
                result.errors.push_back("invalid NodeType in " + entry.path().string() + ": " + e.what());
                continue;
            }

            if (!graph.AddNodeType(nodeType)) {
                result.ok = false;
                fileOk = false;
                result.errors.push_back("failed to add node type '" + nodeType.id +
                                        "' from " + entry.path().string());
                continue;
            }
            ++result.typesLoaded;
        }

        if (fileOk) {
            ++result.filesLoaded;
        }
    }

    return result;
}

}  // namespace NodeAPI
