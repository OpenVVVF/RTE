#pragma once

#include "FirmwareUpdater.h"
#include "TelemetryStore.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace rte::runtime {

struct GatewayApiOptions {
    std::string bindAddress = "127.0.0.1";
    int port = 18080;
    std::size_t workerThreads = 32;
    std::size_t maxStreams = 24;
    std::size_t maxFirmwareBytes = 32U * 1024U * 1024U;
    std::chrono::seconds leaseTtl{15};
};

// Versioned device gateway API. Observers are unrestricted; commands and
// flashes require the single renewable operator lease.
class GatewayApiServer {
public:
    GatewayApiServer(FirmwareUpdater& updater,
                     TelemetryStore& store,
                     GatewayApiOptions options = {});
    ~GatewayApiServer();

    GatewayApiServer(const GatewayApiServer&) = delete;
    GatewayApiServer& operator=(const GatewayApiServer&) = delete;

    bool start();
    void stop();
    bool isRunning() const;
    int actualPort() const;

    void setDevicePort(std::string port);
    void setProtocol(std::string protocol);
    void setDeviceConnected(bool connected);
    void setCommandHandler(std::function<bool(const std::string&)> handler);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace rte::runtime
