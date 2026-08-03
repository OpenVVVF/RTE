// RTEServer: headless inverter link server.
//
// Owns the inverter's UART and serves, on the LAN or loopback:
//   * the raw serial byte stream over TCP (telemetry + console), and
//   * the HTTP API (telemetry/console/command + firmware flash).
//
// NodeGUI connects to both; it never touches the UART itself.
//
// Usage:
//   RTEServer --serial /dev/ttyACM0 [--bind 127.0.0.1] [--bridge-port 4001]
//             [--http-port 18080]

#include "SerialBridge.h"

#include <FirmwareUpdater.h>
#include <HttpApiServer.h>
#include <LegacyTelemetryClient.h>
#include <TelemetryStore.h>

#include <csignal>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {

volatile std::sig_atomic_t g_stop = 0;

void onSignal(int) {
    g_stop = 1;
}

void usage(const char* exe) {
    std::cerr
        << "usage: " << exe << " --serial <port> [options]\n"
        << "  --serial <port>     inverter UART, e.g. /dev/ttyACM0 (required)\n"
        << "  --bind <addr>       HTTP bind address (default 127.0.0.1; 0.0.0.0 = LAN)\n"
        << "  --bridge-port <n>   serial-over-TCP port (default 4001)\n"
        << "  --http-port <n>     HTTP API port (default 18080)\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string serialPort;
    std::string bindAddress = "127.0.0.1";
    int bridgePort = 4001;
    int httpPort = 18080;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto needValue = [&](const char* flag) -> std::string {
            if (++i >= argc) {
                std::cerr << "missing value for " << flag << "\n";
                usage(argv[0]);
                std::exit(1);
            }
            return argv[i];
        };
        if (arg == "--serial") {
            serialPort = needValue("--serial");
        } else if (arg == "--bind") {
            bindAddress = needValue("--bind");
        } else if (arg == "--bridge-port") {
            bridgePort = std::stoi(needValue("--bridge-port"));
        } else if (arg == "--http-port") {
            httpPort = std::stoi(needValue("--http-port"));
        } else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            usage(argv[0]);
            return 1;
        }
    }

    if (serialPort.empty()) {
        usage(argv[0]);
        return 1;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // The bridge owns the UART; everyone else (including the local telemetry
    // client below) talks to it over TCP.
    RTEServer::SerialBridge bridge;
    if (!bridge.start(serialPort, bridgePort)) {
        std::cerr << "RTEServer: failed to open serial port " << serialPort
                  << " or bind bridge port " << bridgePort << "\n";
        return 1;
    }
    std::cout << "RTEServer: serial " << serialPort << " bridged on TCP port "
              << bridgePort << "\n";

    // Local telemetry client feeding the HTTP telemetry endpoints, connected
    // to the bridge over loopback like any other client.
    NodeGUI::runtime::TelemetryStore store;
    NodeGUI::runtime::LegacyTelemetryClient telemetry;
    telemetry.onF32 = [&store](const std::string& key, float value, float tsec) {
        store.AddF32(key, value, tsec);
    };
    telemetry.onString = [&store](const std::string& key, const std::string& value) {
        store.AddString(key, value);
    };
    telemetry.onConsole = [&store](const std::string& line) {
        store.AddConsoleLine(line);
    };
    telemetry.onStats = [&store](const NodeGUI::runtime::LegacyTelemetryClient::Stats& s) {
        store.SetStats(s.rxHz, s.rxBytesPerSec, s.goodFrames, s.badFrames,
                       s.rejectCrc, s.rejectHdr, s.rejectLen,
                       s.rejectPayloadParse, s.rejectUnknownId, s.lastSeq);
    };
    telemetry.startTcp("127.0.0.1", bridgePort);

    NodeGUI::runtime::FirmwareUpdater updater;
    updater.setCurrentPort(serialPort);
    updater.setSuspendCallback([&](bool suspend) {
        if (suspend) {
            telemetry.suspend();
            bridge.suspend();
        } else {
            bridge.resume();
            telemetry.resume();
        }
    });

    NodeGUI::runtime::HttpApiServer http(updater, store, std::to_string(httpPort));
    http.setBindAddress(bindAddress);
    http.setAppName("RTEServer");
    http.setDevicePort(serialPort);
    http.setCommandHandler(
        [&telemetry](const std::string& cmd) { return telemetry.sendLine(cmd); });
    if (!http.start()) {
        std::cerr << "RTEServer: failed to bind HTTP port " << httpPort << " on "
                  << bindAddress << "\n";
        return 1;
    }
    std::cout << "RTEServer: HTTP API on http://" << bindAddress << ":" << httpPort
              << "\n";

    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "RTEServer: shutting down\n";
    http.stop();
    telemetry.stop();
    bridge.stop();
    return 0;
}
