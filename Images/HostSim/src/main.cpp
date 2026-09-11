#include "AppState.h"
#include "can_bridge.h"
#include "sim_runtime.h"

#include <cstdio>
#include <cstdlib>
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
                 "usage: %s [scenario.json] [--live] [--listen host:port] [--realtime N] [--telem-hz N]\n"
                 "  --live               long-running mode; publish InverterProtocol over TCP\n"
                 "  --listen host:port   TCP listen address (default 127.0.0.1:14608)\n"
                 "  --realtime N         wall-clock pacing factor (1.0 = realtime, 0 = as-fast)\n"
                 "  --telem-hz N         telemetry publish rate (default 500 in live mode)\n"
                 "  --can-bridge-listen PORT        share CAN with other host_sim instances (hub)\n"
                 "  --can-bridge-connect HOST:PORT  join a hub as a spoke (Linux only)\n"
                 "  --can-bridge-id N    instance tag for loop filtering (default: pid-derived)\n"
                 "  --can-bridge-debug   also log transmitted frames\n"
                 "  --can-selftest       emit platform_can_send bus=0 id=0x123 every 100 ms\n"
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
    hostsim::CanBridgeConfig bridge_cfg;
    bool bridge_id_given = false;

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
        } else if (std::strcmp(arg, "--scenario") == 0) {
            if (++i >= argc) {
                PrintUsage(argv[0]);
                return 1;
            }
            scenario = argv[i];
        } else if (std::strcmp(arg, "--can-bridge-listen") == 0) {
            if (++i >= argc) {
                PrintUsage(argv[0]);
                return 1;
            }
            bridge_cfg.port = std::atoi(argv[i]);
            if (bridge_cfg.port <= 0 || bridge_cfg.port > 65535) {
                std::fprintf(stderr, "invalid --can-bridge-listen port\n");
                return 1;
            }
            bridge_cfg.hub = true;
        } else if (std::strcmp(arg, "--can-bridge-connect") == 0) {
            if (++i >= argc) {
                PrintUsage(argv[0]);
                return 1;
            }
            if (!ParseHostPort(argv[i], &bridge_cfg.host, &bridge_cfg.port)) {
                std::fprintf(stderr,
                             "invalid --can-bridge-connect (expected HOST:PORT)\n");
                return 1;
            }
            bridge_cfg.connect = true;
        } else if (std::strcmp(arg, "--can-bridge-id") == 0) {
            if (++i >= argc) {
                PrintUsage(argv[0]);
                return 1;
            }
            bridge_cfg.instance_id =
                static_cast<uint32_t>(std::strtoul(argv[i], nullptr, 0));
            bridge_id_given = bridge_cfg.instance_id != 0;
        } else if (std::strcmp(arg, "--can-bridge-debug") == 0) {
            bridge_cfg.debug = true;
        } else if (std::strcmp(arg, "--can-selftest") == 0) {
            bridge_cfg.selftest = true;
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

    if (bridge_cfg.hub && bridge_cfg.connect) {
        std::fprintf(stderr,
                     "--can-bridge-listen and --can-bridge-connect are "
                     "mutually exclusive\n");
        return 1;
    }
    if (bridge_id_given && bridge_cfg.instance_id == 0) {
        std::fprintf(stderr, "--can-bridge-id must be non-zero\n");
        return 1;
    }

    hostsim::SimRuntime& runtime = hostsim::GlobalSimRuntime();
    if (!runtime.LoadScenario(scenario)) {
        std::fprintf(stderr, "HostSim: failed to load scenario %s\n", scenario);
        return 1;
    }

    if (live) runtime.SetLive(true);
    if (listen_set || live) runtime.SetListen(listen_host, listen_port);
    if (realtime >= 0.0f) runtime.SetRealtimeFactor(realtime);
    if (telem_hz > 0.0f) runtime.SetTelemetryHz(telem_hz);
    if (bridge_cfg.hub || bridge_cfg.connect || bridge_cfg.selftest) {
        hostsim::GlobalCanBridge().Configure(bridge_cfg);
    }

    return runtime.Run();
}
