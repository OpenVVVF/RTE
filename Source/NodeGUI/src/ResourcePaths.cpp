#include "ResourcePaths.h"

#include <QCoreApplication>

namespace NodeGUI {

std::filesystem::path InstalledResourceRoot() {
    std::filesystem::path directory =
        std::filesystem::path(QCoreApplication::applicationDirPath().toStdString());
    for (int level = 0; level < 6 && !directory.empty(); ++level) {
        const auto candidate = directory / "share" / "rte";
        if (std::filesystem::is_directory(candidate)) return candidate;
        directory = directory.parent_path();
    }
    return {};
}

std::filesystem::path ProjectRoot() {
#ifdef RTE_PROJECT_ROOT
    return RTE_PROJECT_ROOT;
#else
    return std::filesystem::current_path();
#endif
}

}  // namespace NodeGUI
