#pragma once

/**
 * SimRunner: headless closed-loop sw+hw simulator driver.
 *
 * Pipeline per Run():
 *   1. emit the controller (SW) nodegraph into a full firmware tree with the
 *      exact emit pipeline used by `rte generate` (RTECodeEmitter::Emitter);
 *   2. generate the plant (HW) nodegraph C++ via Lib/PlantCodegen
 *      (PlantCodegenLib), or stage the built-in fallback plant when that
 *      library is not part of this build;
 *   3. stage the fixed host runtime (scheduler main + platform shim +
 *      shared scenario/format code) into the work dir;
 *   4. compile the scenario binary with the host system compiler;
 *   5. execute it; it writes a v3 rte-runtime-session-jsonl identical in
 *      schema to twin/harness.py's TwinSession.
 */

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "RTESim/Scenario.h"

namespace RTESim {

struct SimOptions {
    std::filesystem::path swGraphPath;      // controller nodegraph JSON (e.g. foc_demo)
    std::filesystem::path plantGraphPath;   // plant nodegraph JSON (Plant.* templates)
    std::filesystem::path baseSourceDir;    // Gen6FW base image for the SW emit
    std::filesystem::path templatesDir;     // SW template dir (empty = graph-embedded only)
    std::filesystem::path plantTemplatesDir; // Assets/PlantTemplates (empty = autodetect)
    std::filesystem::path workDir;          // empty -> <outPath>.work

    std::string name = "rte_sim";
    double durationS = 3.0;
    double dtSubS = 0.0;                 // F4: plant substep; 0 = host default (200e-6)
    double dynoRpm = 0.0;
    bool dynoRpmSet = false;
    std::vector<DynoStep> dynoSeries;    // overrides dynoRpm when non-empty
    SupplyOverride supply;
    std::vector<ScenarioEvent> events;

    double telemetryHz = 100.0;
    std::filesystem::path outPath;
    // F11: the work dir is ALWAYS kept (no --keep-work switch).
    // F15: without PlantCodegenLib the run is refused unless this opt-in is
    // set (the fallback plant is a physics substitute, not the graph's).
    bool allowFallbackPlant = false;
};

// Directory layout used inside workDir; exposed for structural tests.
struct SimLayout {
    std::filesystem::path sw;      // emitted firmware tree (generated/, Inc/, ...)
    std::filesystem::path plant;   // plant_generated.{h,cpp}
    std::filesystem::path host;    // staged runtime sources + sim_host binary
    std::filesystem::path binary;  // host/sim_host
};

SimLayout LayoutForWorkDir(const std::filesystem::path& workDir);

class SimRunner {
public:
    // progress lines (human-readable, one per pipeline phase)
    using Logger = std::function<void(const std::string&)>;

    explicit SimRunner(SimOptions options, Logger logger = {});

    bool Run(std::string& error);

private:
    bool EmitSw(std::string& error);
    bool GeneratePlant(std::string& error);
    bool StageHostSources(std::string& error);
    bool CompileHost(std::string& error);
    bool RunHost(std::string& error);

    SimOptions options_;
    Logger logger_;
    SimLayout layout_;
};

} // namespace RTESim
