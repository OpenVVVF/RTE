#include "AppState.h"
#include "sim_runtime.h"

#include <cstdio>
#include <cstring>
#include <string>

AppState appState;

namespace {

bool ParseHostPort(const std::string& spec, std::string* host, int* port) {
    const auto colon = spec.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= spec.size()) {
        return false;
    }
    *host = spec.substr(0, colon);
    *port = std::atoi(spec.substr(colon + 1).c_str());
    return *port > 0 && *port < 65536;
}

void PrintUsage(const char* exe) {
    std::fprintf(stderr,
                 "usage: %s [scenario.json] [options]\n"
                 "  --live               long-running mode; publish InverterProtocol over TCP\n"
                 "  --listen host:port   TCP listen address (default 127.0.0.1:14608)\n"
                 "  --realtime N         wall-clock pacing factor (1.0 = realtime, 0 = as-fast)\n"
                 "  --telem-hz N         telemetry publish rate (default 500 in live mode)\n"
                 "  --plant-backend S    plant backend: ode (fast) or ngspice (accurate)\n"
                 "  --duration N         simulation duration in seconds (batch mode)\n"
                 "  --tim-isr-hz N       TIM ISR rate in Hz (controls waveform sampling)\n"
                 "  --substeps N         ngspice electrical substeps per TIM step\n"
                 "\n"
                 "Fast view (default):  --plant-backend ode\n"
                 "Accurate view:        --plant-backend ngspice --tim-isr-hz 50000\n"
                 "\n"
                 "NodeGUI:  NodeGUI --tcp 127.0.0.1:14608 --protocol ivp\n"
                 "Console:  throttle a 0.5 | pause | resume | clear | quit\n",
                 exe);
}

} // namespace

int main(int argc, char** argv) {
    const char* scenario = "scenarios/default_motor.json";
    bool live = false;
    bool listen_set = false;
    std::string listen_host = "127.0.0.1";
    int listen_port = 14608;
    float realtime = -1.0f;
    float telem_hz = -1.0f;
    std::string plant_backend;
    float duration_s = -1.0f;
    float tim_isr_hz = -1.0f;
    int substeps = -1;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--live") == 0) {
            live = true;
        } else if (std::strcmp(arg, "--listen") == 0) {
            if (++i >= argc) {
                PrintUsage(argv[0]);
                return 1;
            }
            if (!ParseHostPort(argv[i], &listen_host, &listen_port)) {
                std::fprintf(stderr, "invalid --listen (expected host:port)\n");
                return 1;
            }
            listen_set = true;
        } else if (std::strcmp(arg, "--realtime") == 0) {
            if (++i >= argc) {
                PrintUsage(argv[0]);
                return 1;
            }
            realtime = static_cast<float>(std::atof(argv[i]));
        } else if (std::strcmp(arg, "--telem-hz") == 0) {
            if (++i >= argc) {
                PrintUsage(argv[0]);
                return 1;
            }
            telem_hz = static_cast<float>(std::atof(argv[i]));
        } else if (std::strcmp(arg, "--plant-backend") == 0) {
            if (++i >= argc) {
                PrintUsage(argv[0]);
                return 1;
            }
            plant_backend = argv[i];
        } else if (std::strcmp(arg, "--duration") == 0) {
            if (++i >= argc) {
                PrintUsage(argv[0]);
                return 1;
            }
            duration_s = static_cast<float>(std::atof(argv[i]));
        } else if (std::strcmp(arg, "--tim-isr-hz") == 0) {
            if (++i >= argc) {
                PrintUsage(argv[0]);
                return 1;
            }
            tim_isr_hz = static_cast<float>(std::atof(argv[i]));
        } else if (std::strcmp(arg, "--substeps") == 0) {
            if (++i >= argc) {
                PrintUsage(argv[0]);
                return 1;
            }
            substeps = std::atoi(argv[i]);
        } else if (std::strcmp(arg, "--scenario") == 0) {
            if (++i >= argc) {
                PrintUsage(argv[0]);
                return 1;
            }
            scenario = argv[i];
        } else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            PrintUsage(argv[0]);
            return 0;
        } else if (arg[0] != '-') {
            scenario = arg;
        } else {
            PrintUsage(argv[0]);
            return 1;
        }
    }

    hostsim::SimRuntime& runtime = hostsim::GlobalSimRuntime();
    if (!runtime.LoadScenario(scenario)) {
        std::fprintf(stderr, "HostSim: failed to load scenario %s\n", scenario);
        return 1;
    }

    // CLI overrides applied after scenario load so they win.
    if (!plant_backend.empty()) runtime.SetPlantBackend(plant_backend);
    if (duration_s > 0.0f) runtime.SetDuration(duration_s);
    if (tim_isr_hz > 0.0f) runtime.SetTimIsrHz(tim_isr_hz);
    if (substeps > 0) runtime.SetNgspiceSubsteps(substeps);
    if (live) runtime.SetLive(true);
    if (listen_set || live) runtime.SetListen(listen_host, listen_port);
    if (realtime >= 0.0f) runtime.SetRealtimeFactor(realtime);
    if (telem_hz > 0.0f) runtime.SetTelemetryHz(telem_hz);

    return runtime.Run();
}
