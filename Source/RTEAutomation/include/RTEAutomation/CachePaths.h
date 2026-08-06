#pragma once

#include <filesystem>
#include <string>

namespace RTEAutomation {

struct FirmwareWorkspace {
    std::string projectId;
    std::filesystem::path root;
    std::filesystem::path generated;
    std::filesystem::path build;
    std::filesystem::path artifacts;
    std::filesystem::path manifest;
};

std::filesystem::path DefaultCacheRoot();
std::string ProjectIdForGraph(const std::filesystem::path& graph);
FirmwareWorkspace WorkspaceForGraph(const std::filesystem::path& graph,
                                    const std::string& configuration,
                                    const std::filesystem::path& cacheRoot = {});

}  // namespace RTEAutomation
