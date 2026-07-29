#include "HttpApiServer.h"

#include <cstdio>

namespace NodeGUI::runtime {

// Windows stub: upstream HttpApiServer is POSIX sockets / Linux-first.
// Path A (HostSim live telemetry) uses InverterProtocol TCP via --tcp and
// does not require this HTTP flash API on Windows.

HttpApiServer::HttpApiServer(FirmwareUpdater& updater, TelemetryStore& store,
                             std::string port)
    : updater_(updater)
    , store_(store)
    , port_(std::move(port)) {}

HttpApiServer::~HttpApiServer() { stop(); }

bool HttpApiServer::start() { return start(port_); }

bool HttpApiServer::start(const std::string& port) {
    port_ = port;
    running_.store(false);
    actual_port_.store(0);
    std::fprintf(stderr,
                 "HttpApiServer: not available on Windows (use HostSim --live + "
                 "NodeGUI --tcp for Path A telemetry)\n");
    return false;
}

bool HttpApiServer::restart(const std::string& port) {
    stop();
    return start(port);
}

void HttpApiServer::stop() {
    stop_.store(true);
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

void HttpApiServer::setDevicePort(const std::string& port) {
    std::lock_guard<std::mutex> lk(config_mtx_);
    device_port_ = port;
}

void HttpApiServer::setCommandHandler(std::function<bool(const std::string&)> handler) {
    std::lock_guard<std::mutex> lk(config_mtx_);
    command_handler_ = std::move(handler);
}

void HttpApiServer::setAppName(const std::string& name) {
    std::lock_guard<std::mutex> lk(config_mtx_);
    app_name_ = name;
}

void HttpApiServer::threadMain() {}

} // namespace NodeGUI::runtime
