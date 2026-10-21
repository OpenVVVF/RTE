#pragma once

#include <filesystem>

namespace NodeGUI {

// Walks up from the application binary looking for the installed share/rte
// resource tree (packaged layout); empty when the app runs unpackaged.
// Shared by every Assets/Images resolver (used to live as a MainWindow-local
// helper; the Plant Editor's example-dialog seed needs the same walk).
std::filesystem::path InstalledResourceRoot();

// Project root from the RTE_PROJECT_ROOT build define, else the process cwd.
std::filesystem::path ProjectRoot();

}  // namespace NodeGUI
