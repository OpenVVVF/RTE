#pragma once

#include "FlashBackend.h"
#include "GatewayClient.h"

#include <QString>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace NodeGUI::runtime {

class RemoteFlashBackend : public FlashBackend {
public:
    explicit RemoteFlashBackend(std::shared_ptr<rte::runtime::GatewayClient> client);
    ~RemoteFlashBackend() override;

    void SetServer(const QString& baseUrl);
    QString BaseUrl() const;
    bool QueueFlash(const std::string& firmwarePath, bool autoGpio) override;
    FlashBackendStatus Status() override;

private:
    void PollLoop();

    std::shared_ptr<rte::runtime::GatewayClient> client_;
    mutable std::mutex mutex_;
    FlashBackendStatus status_;
    std::string queueError_;
    std::string jobId_;
    std::atomic<bool> run_{true};
    std::thread pollThread_;
};

}  // namespace NodeGUI::runtime
