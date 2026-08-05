#pragma once

#include "FirmwareUpdater.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rte::runtime {

struct GatewayClientStats {
    float rxHz = 0.0f;
    float rxBytesPerSec = 0.0f;
    uint64_t goodFrames = 0;
    uint64_t badFrames = 0;
    uint64_t rejectCrc = 0;
    uint64_t rejectHdr = 0;
    uint64_t rejectLen = 0;
    uint64_t rejectPayloadParse = 0;
    uint64_t rejectUnknownId = 0;
    uint32_t lastSeq = 0;
    bool suspended = false;
};

struct GatewayFlashStatus {
    bool reachable = false;
    std::string jobId;
    std::string state = "Unreachable";
    bool busy = false;
    int progress = -1;
    std::string lastError;
    std::vector<std::string> log;
};

class GatewayClient {
public:
    explicit GatewayClient(std::string baseUrl = "http://127.0.0.1:18080");
    ~GatewayClient();

    GatewayClient(const GatewayClient&) = delete;
    GatewayClient& operator=(const GatewayClient&) = delete;

    void setBaseUrl(std::string baseUrl);
    std::string baseUrl() const;

    bool startEvents();
    void stopEvents();
    bool eventsRunning() const { return eventsRun_.load(); }

    bool acquireLease(const std::string& owner, std::string* error = nullptr);
    bool releaseLease(std::string* error = nullptr);
    bool hasLease() const;
    std::string leaseId() const;
    std::string leaseOwner() const;
    void useLease(std::string id, std::string owner = "external");

    bool sendCommand(const std::string& command,
                     std::vector<std::string>* output = nullptr,
                     std::string* error = nullptr,
                     int waitMs = 100);
    bool flashFile(const std::string& path,
                   bool autoGpio,
                   std::string* jobId = nullptr,
                   std::string* error = nullptr);
    GatewayFlashStatus flashStatus(const std::string& jobId = {}) const;
    std::string infoJson(std::string* error = nullptr) const;
    std::string stateJson(std::string* error = nullptr) const;
    std::string consoleJson(uint64_t since = 0,
                            std::size_t limit = 200,
                            std::string* error = nullptr) const;

    std::function<void(const std::string&, float, float)> onF32;
    std::function<void(const std::string&, const std::string&)> onString;
    std::function<void(uint64_t, const std::string&)> onConsole;
    std::function<void(const GatewayClientStats&)> onStats;
    std::function<void()> onReset;
    std::function<void(bool, const std::string&)> onConnection;
    std::function<void(bool, const std::string&)> onDevice;
    std::function<void(bool, const std::string&)> onLease;

private:
    void eventLoop();
    void renewalLoop();
    void startLeaseRenewal();
    void stopLeaseRenewal();
    void dispatchEvent(const std::string& type, const std::string& data);
    void clearLease();

    mutable std::mutex mutex_;
    std::string baseUrl_;
    std::string leaseId_;
    std::string leaseOwner_;
    bool ownsLease_ = false;
    std::string lastFlashJobId_;
    uint64_t lastEventId_ = 0;

    mutable std::mutex activeClientMutex_;
    void* activeHttpClient_ = nullptr;
    std::atomic<bool> eventsRun_{false};
    std::atomic<bool> leaseRun_{false};
    std::atomic<int> leaseRenewIntervalMs_{5000};
    std::thread eventThread_;
    std::thread renewalThread_;
};

}  // namespace rte::runtime
