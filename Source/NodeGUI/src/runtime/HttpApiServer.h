#pragma once

#include "FirmwareUpdater.h"
#include "TelemetryStore.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace NodeGUI::runtime {

// Loopback-only (127.0.0.1) HTTP/JSON API server, ported from the old ImGui
// client's HttpFlashServer. One blocking accept thread, one request at a
// time, Content-Length bodies only, Connection: close. Linux/POSIX only.
//
// Endpoints:
//   GET  /api/info                     app/device/http port/uptime/endpoint list
//   GET  /api/telemetry                latest float + string signals
//   GET  /api/console?lines=N&since=SEQ  device console scrollback
//   POST /api/command?wait_ms=N        send one shell line, return new console
//   POST /flash                        queue a raw .bin for STM32 flash
//   GET  /flash/status                 updater state + log tail
class HttpApiServer {
public:
    HttpApiServer(FirmwareUpdater& updater, TelemetryStore& store,
                  std::string port = "18080");
    ~HttpApiServer();

    HttpApiServer(const HttpApiServer&) = delete;
    HttpApiServer& operator=(const HttpApiServer&) = delete;

    bool start();
    bool start(const std::string& port);
    bool restart(const std::string& port);
    void stop();
    bool isRunning() const { return running_.load(); }
    int actualPort() const { return actual_port_.load(); }

    // Device serial port, reported by GET /api/info and forwarded to the
    // updater so POST /flash knows which port to use.
    void setDevicePort(const std::string& port);

    // Sends a shell line to the device; returns true on write success.
    void setCommandHandler(std::function<bool(const std::string&)> handler);

    // Reported as "app" by GET /api/info. Default "NodeGUI".
    void setAppName(const std::string& name);

private:
    void threadMain();

    FirmwareUpdater& updater_;
    TelemetryStore& store_;
    std::chrono::steady_clock::time_point start_time_{};
    std::string port_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::atomic<int> actual_port_{0};

    mutable std::mutex config_mtx_;  // guards device_port_, command_handler_, app_name_
    std::string device_port_;
    std::function<bool(const std::string&)> command_handler_;
    std::string app_name_ = "NodeGUI";

    int listen_fd_ = -1;
    std::thread thread_;
};

}  // namespace NodeGUI::runtime
