#include <filesystem>
#include <iostream>

#include "simulation/ControllerComparison.h"

namespace fs = std::filesystem;

int main() {
    using namespace simulation;

    PmsmParameters motor;
    ControllerComparison comparison(motor);

    ComparisonScenario scenario;
    scenario.name = "load_step_5nm";
    scenario.duration = 0.08;
    scenario.ts = 100e-6f;
    scenario.vdc = 540.0f;
    scenario.id_ref = 0.0f;
    scenario.iq_ref = 10.0f;
    scenario.mpcc_mode = MPCCMode::ConventionalOneStep;
    // Disturbance: load torque steps from 0 to 5 Nm at t = 0.03 s.
    scenario.load_torque = [](double t) { return (t >= 0.03) ? 5.0f : 0.0f; };

    const auto mpcc_samples = comparison.run(ControllerType::MPCC, scenario);
    scenario.mpcc_mode = MPCCMode::OptimalDutyCycle;
    const auto mpcc_opt_samples = comparison.run(ControllerType::MPCC, scenario);
    const auto foc_samples = comparison.run(ControllerType::FOC, scenario);

    const fs::path out_dir = fs::path("results") / "comparison";
    fs::create_directories(out_dir);

    const std::string mpcc_path = (out_dir / "mpcc.csv").string();
    const std::string mpcc_opt_path = (out_dir / "mpcc_opt.csv").string();
    const std::string foc_path = (out_dir / "foc.csv").string();
    ControllerComparison::writeCsv(mpcc_path, mpcc_samples);
    ControllerComparison::writeCsv(mpcc_opt_path, mpcc_opt_samples);
    ControllerComparison::writeCsv(foc_path, foc_samples);

    std::cout << "Wrote " << mpcc_path << " (" << mpcc_samples.size() << " samples)\n";
    std::cout << "Wrote " << mpcc_opt_path << " (" << mpcc_opt_samples.size() << " samples)\n";
    std::cout << "Wrote " << foc_path << " (" << foc_samples.size() << " samples)\n";
    std::cout << "Run: python3 scripts/compare_mpcc_foc.py\n";
    return 0;
}
