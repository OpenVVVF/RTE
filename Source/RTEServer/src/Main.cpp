// rte-gateway: the sole owner of an inverter serial connection.

#include <FirmwareUpdater.h>
#include <GatewayApiServer.h>
#include <LegacyTelemetryClient.h>
#include <TelemetryStore.h>

#include <inverter_protocol/host/host_client.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

using json = nlohmann::json;

namespace {

volatile std::sig_atomic_t gStop = 0;
void OnSignal(int) { gStop = 1; }

struct Config {
    std::string serialPort;
    std::string protocol = "legacy";
    std::string bindAddress = "127.0.0.1";
    int httpPort = 18080;
    std::size_t workerThreads = 32;
    std::size_t maxStreams = 24;
    std::size_t maxFirmwareBytes = 32U * 1024U * 1024U;
    float telemetryRetainSeconds = 30.0f;
    std::size_t telemetryMaxSamples = 12000;
    std::size_t consoleCapLines = 6000;
    std::size_t eventReplayCap = 10000;
    std::string programmerCli;
    std::string gpioHelper;
    std::string python;
};

void Usage(const char* executable) {
    std::cerr
        << "usage: " << executable << " --serial <port> [options]\n"
        << "  --config <file>      JSON configuration file\n"
        << "  --serial <port>      inverter UART, e.g. /dev/ttyACM0\n"
        << "  --protocol <name>    legacy or inverter (default legacy)\n"
        << "  --bind <address>     HTTP bind address (default 127.0.0.1)\n"
        << "  --http-port <port>   versioned API port (default 18080)\n";
}

bool LoadConfig(const std::string& path, Config& config, std::string& error) {
    std::ifstream file(path);
    if (!file) {
        error = "could not open config: " + path;
        return false;
    }
    json value = json::parse(file, nullptr, false);
    if (!value.is_object()) {
        error = "config must contain a JSON object";
        return false;
    }
    if (value.contains("serial")) config.serialPort = value["serial"].value("port", "");
    config.protocol = value.value("protocol", config.protocol);
    if (value.contains("http")) {
        const auto& http = value["http"];
        config.bindAddress = http.value("bind", config.bindAddress);
        config.httpPort = http.value("port", config.httpPort);
        config.workerThreads = http.value("worker_threads", config.workerThreads);
        config.maxStreams = http.value("max_streams", config.maxStreams);
        config.maxFirmwareBytes = http.value("max_firmware_bytes", config.maxFirmwareBytes);
    }
    if (value.contains("telemetry")) {
        const auto& telemetry = value["telemetry"];
        config.telemetryRetainSeconds = telemetry.value(
            "retain_seconds", config.telemetryRetainSeconds);
        config.telemetryMaxSamples = telemetry.value(
            "max_samples_per_signal", config.telemetryMaxSamples);
        config.consoleCapLines = telemetry.value(
            "console_lines", config.consoleCapLines);
        config.eventReplayCap = telemetry.value(
            "event_replay", config.eventReplayCap);
    }
    if (value.contains("tools")) {
        const auto& tools = value["tools"];
        config.programmerCli = tools.value("stm32_programmer_cli", "");
        config.gpioHelper = tools.value("gpio_helper", "");
        config.python = tools.value("python", "");
    }
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
#ifdef RTE_DEPRECATED_SERVER_ALIAS
    std::cerr << "warning: RTEServer is deprecated; use rte-gateway\n";
#endif
    Config config;

    // Load the config first, then apply explicit CLI overrides regardless of
    // where --config appears.
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--config" && i + 1 < argc) {
            std::string error;
            if (!LoadConfig(argv[++i], config, error)) {
                std::cerr << "rte-gateway: " << error << "\n";
                return 1;
            }
        }
    }
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        auto value = [&](const char* flag) -> std::string {
            if (++i >= argc) {
                std::cerr << "missing value for " << flag << "\n";
                Usage(argv[0]);
                std::exit(1);
            }
            return argv[i];
        };
        if (argument == "--config") {
            ++i;
        } else if (argument == "--serial") {
            config.serialPort = value("--serial");
        } else if (argument == "--protocol") {
            config.protocol = value("--protocol");
        } else if (argument == "--bind") {
            config.bindAddress = value("--bind");
        } else if (argument == "--http-port") {
            config.httpPort = std::stoi(value("--http-port"));
        } else if (argument == "--help" || argument == "-h") {
            Usage(argv[0]);
            return 0;
        } else {
            std::cerr << "unknown argument: " << argument << "\n";
            Usage(argv[0]);
            return 1;
        }
    }

    if (config.serialPort.empty()
        || (config.protocol != "legacy" && config.protocol != "inverter")) {
        Usage(argv[0]);
        return 1;
    }

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    rte::runtime::TelemetryStore store(config.telemetryRetainSeconds,
                                       config.telemetryMaxSamples,
                                       config.consoleCapLines,
                                       config.eventReplayCap);
    rte::runtime::LegacyTelemetryClient legacy;
    ivp::InverterClient inverter;

    auto addFloat = [&store](const std::string& key, float value, float tsec) {
        store.AddF32(key, value, tsec);
    };
    auto addString = [&store](const std::string& key, const std::string& value) {
        store.AddString(key, value);
    };
    auto addConsole = [&store](const std::string& line) { store.AddConsoleLine(line); };
    legacy.onF32 = addFloat;
    legacy.onString = addString;
    legacy.onConsole = addConsole;
    legacy.onStats = [&store](const rte::runtime::LegacyTelemetryClient::Stats& stats) {
        store.SetStats(stats.rxHz, stats.rxBytesPerSec, stats.goodFrames,
                       stats.badFrames, stats.rejectCrc, stats.rejectHdr,
                       stats.rejectLen, stats.rejectPayloadParse,
                       stats.rejectUnknownId, stats.lastSeq);
    };

    const auto started = std::chrono::steady_clock::now();
    inverter.onF32Value([&](uint16_t, const std::string& key, float value, uint32_t) {
        addFloat(key, value,
                 std::chrono::duration<float>(std::chrono::steady_clock::now() - started).count());
    });
    inverter.onStringValue([&](uint16_t, const std::string& key,
                               const std::string& value, uint32_t) {
        addString(key, value);
    });
    inverter.onConsoleLine(addConsole);
    inverter.onStats([&store](const ivp::ClientStats& stats) {
        store.SetStats(stats.rx_hz, stats.rx_bytes_per_sec, stats.good_frames,
                       stats.bad_frames, stats.reject_crc, stats.reject_hdr,
                       stats.reject_len, 0, 0, stats.last_seq);
    });

    const bool opened = config.protocol == "legacy"
        ? legacy.start(config.serialPort)
        : inverter.start(config.serialPort);
    if (!opened) {
        std::cerr << "rte-gateway: failed to open " << config.serialPort << "\n";
        return 1;
    }

    rte::runtime::FirmwareUpdater updater;
    if (!config.programmerCli.empty()) {
        setenv("RTE_STM32_PROGRAMMER_CLI", config.programmerCli.c_str(), 1);
    }
    if (!config.gpioHelper.empty()) {
        setenv("RTE_GPIO_HELPER", config.gpioHelper.c_str(), 1);
    }
    if (!config.python.empty()) setenv("RTE_PYTHON", config.python.c_str(), 1);
    updater.setCurrentPort(config.serialPort);
    updater.setSuspendCallback([&](bool suspend) {
        store.SetSuspended(suspend);
        if (config.protocol == "legacy") {
            suspend ? legacy.suspend() : legacy.resume();
        } else if (suspend) {
            inverter.stop();
        } else {
            inverter.start(config.serialPort);
        }
    });

    rte::runtime::GatewayApiOptions apiOptions;
    apiOptions.bindAddress = config.bindAddress;
    apiOptions.port = config.httpPort;
    apiOptions.workerThreads = config.workerThreads;
    apiOptions.maxStreams = config.maxStreams;
    apiOptions.maxFirmwareBytes = config.maxFirmwareBytes;
    rte::runtime::GatewayApiServer api(updater, store, apiOptions);
    api.setDevicePort(config.serialPort);
    api.setProtocol(config.protocol);
    api.setCommandHandler([&](const std::string& command) {
        return config.protocol == "legacy" ? legacy.sendLine(command)
                                            : inverter.sendCommandLine(command);
    });
    if (!api.start()) {
        std::cerr << "rte-gateway: failed to bind http://" << config.bindAddress
                  << ':' << config.httpPort << "\n";
        legacy.stop();
        inverter.stop();
        return 1;
    }

    std::cout << "rte-gateway: " << config.protocol << " serial "
              << config.serialPort << " on http://" << config.bindAddress
              << ':' << api.actualPort() << "\n";
    while (!gStop) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    api.stop();
    legacy.stop();
    inverter.stop();
    std::cout << "rte-gateway: stopped\n";
    return 0;
}
