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
 *      shared scenario/format code) into the work dir, and write
 *      sim_config_seed.inc — the FRAM config seed derived from the sw
 *      graph's Values.Config nodes (see DeriveConfigSeed below);
 *   4. compile the scenario binary with the host system compiler, SKIPPED
 *      when the fingerprint of the staged inputs (hash of every staged
 *      sw/plant/host file byte post steps 1-3 plus the compiler identity —
 *      everything steps 1-3 derive from the sw graph, plant graph, base
 *      source tree and config seed) matches the stamp in
 *      <workDir>/.input_hash AND the staged binary still exists; the skip is
 *      logged on the progress stream as its own line
 *      ("compile_host_skipped_fresh: ..."). forceCompile /
 *      RTESIM_FORCE_COMPILE=1 always recompiles and re-stamps;
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
    // Step-4 compile cache override: ignore a fresh <workDir>/.input_hash
    // stamp and always recompile + re-stamp. Same effect as the
    // RTESIM_FORCE_COMPILE=1 environment variable (the two OR together, so
    // CLI/GUI callers inherit the env escape hatch with no new switch).
    bool forceCompile = false;
};

// Directory layout used inside workDir; exposed for structural tests.
struct SimLayout {
    std::filesystem::path sw;      // emitted firmware tree (generated/, Inc/, ...)
    std::filesystem::path plant;   // plant_generated.{h,cpp}
    std::filesystem::path host;    // staged runtime sources + sim_host binary
    std::filesystem::path binary;  // host/sim_host
};

SimLayout LayoutForWorkDir(const std::filesystem::path& workDir);

/* --------------------------------------------------------------------------
 * FRAM config seed derivation (bug: the scenario host used to seed its fake
 * FRAM from a verbatim hardcoded table, so tuning a graph's Values.Config
 * DefaultValue in the GUI changed nothing under `rte sim`).
 *
 * The scenario host's FRAM seed is now derived from the controller graph the
 * user actually runs: every config node (type "Values.Config" or "config.*",
 * the same rule RTECodeEmitter uses for the g_<domain>_configs[] registry)
 * contributes its "Key" parameter (FRAM key string) and "DefaultValue"
 * parameter. Keys the graph mentions override the baseline; keys it does not
 * mention keep the baseline below (the old verbatim twin/native table, i.e.
 * FocController.DEFAULTS parity), so nothing twin-compatible regresses when
 * the graph is silent.
 * -------------------------------------------------------------------------- */

struct ConfigSeedEntry {
    std::string key;    // FRAM key, e.g. "Ctrl.Iq.SlewAps"
    double value = 0.0;
};

// The baseline table (was: inline literal in hostmain/main.cpp).
std::vector<ConfigSeedEntry> ConfigSeedBaseline();

// Baseline merged with the graph's config-node defaults; on unparseable JSON
// the baseline is returned unchanged (the emitter already validated it).
std::vector<ConfigSeedEntry> DeriveConfigSeed(const std::string& graphJsonText);

// The staged include written next to the host sources (kGraphConfigSeed[]).
std::string RenderConfigSeedInclude(const std::vector<ConfigSeedEntry>& seed);

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
