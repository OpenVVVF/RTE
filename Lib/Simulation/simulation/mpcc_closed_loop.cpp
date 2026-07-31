#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "simulation/ClosedLoopSimulator.h"

namespace fs = std::filesystem;

int main() {
    using namespace simulation;

    PmsmParameters motor;
    MpccParameters ctrl;
    ctrl.motor = motor;
    ctrl.ts = 100e-6f;

    ClosedLoopSimulator sim(motor, ctrl);

    const fs::path results_dir = fs::path("results") / "closed_loop";
    fs::create_directories(results_dir);

    std::vector<Scenario> scenarios = {
        {"iq_step", 0.05, 100e-6f, 540.0f, 0.0f, 0.0f, 10.0f},
        {"iq_reversal", 0.08, 100e-6f, 540.0f, 0.0f, 0.0f, -10.0f},
        {"load_step", 0.1, 100e-6f, 540.0f, 5.0f, 0.0f, 10.0f},
        {"dc_reduction", 0.05, 100e-6f, 300.0f, 0.0f, 0.0f, 10.0f},
        {"rs_mismatch_p20", 0.05, 100e-6f, 540.0f, 0.0f, 0.0f, 10.0f, false, 0.0f, 0.5f, 20.0f,
         MPCCMode::ConventionalOneStep, 1.2f, 1.0f, 1.0f, 1.0f},
        {"l_mismatch_m20", 0.05, 100e-6f, 540.0f, 0.0f, 0.0f, 10.0f, false, 0.0f, 0.5f, 20.0f,
         MPCCMode::ConventionalOneStep, 1.0f, 0.8f, 0.8f, 1.0f},
        {"psi_mismatch_p10", 0.05, 100e-6f, 540.0f, 0.0f, 0.0f, 10.0f, false, 0.0f, 0.5f, 20.0f,
         MPCCMode::ConventionalOneStep, 1.0f, 1.0f, 1.0f, 1.1f},
    };

    Scenario speed_case{"speed_step", 0.2, 100e-6f, 540.0f, 0.0f, 0.0f, 0.0f, true, 100.0f};
    scenarios.push_back(speed_case);

    for (const auto& scenario : scenarios) {
        const auto samples = sim.run(scenario);
        const std::string path = (results_dir / (scenario.name + ".csv")).string();
        ClosedLoopSimulator::writeCsv(path, samples);
        std::cout << "Wrote " << path << " (" << samples.size() << " samples)\n";
    }

    return 0;
}
