#include "AppState.h"
#include "sim_runtime.h"

#include <cstdio>
#include <cstring>

AppState appState;

int main(int argc, char** argv) {
    const char* scenario = "scenarios/default_motor.json";
    if (argc > 1 && argv[1][0] != '\0') {
        scenario = argv[1];
    }

    hostsim::SimRuntime runtime;
    if (!runtime.LoadScenario(scenario)) {
        std::fprintf(stderr, "HostSim: failed to load scenario %s\n", scenario);
        return 1;
    }

    runtime.InitDomains();

    while (runtime.StepOnce()) {
    }

    runtime.Shutdown();
    std::printf("HostSim: wrote %s\n", runtime.Config().trace_csv.c_str());
    return 0;
}
