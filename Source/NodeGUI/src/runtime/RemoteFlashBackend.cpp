#include "RemoteFlashBackend.h"

#include <chrono>

namespace NodeGUI::runtime {

RemoteFlashBackend::RemoteFlashBackend(
    std::shared_ptr<rte::runtime::GatewayClient> client)
    : client_(std::move(client)), pollThread_(&RemoteFlashBackend::PollLoop, this) {}

RemoteFlashBackend::~RemoteFlashBackend() {
    run_.store(false);
    if (pollThread_.joinable()) pollThread_.join();
}

void RemoteFlashBackend::SetServer(const QString& baseUrl) {
    client_->setBaseUrl(baseUrl.toStdString());
    std::lock_guard lock(mutex_);
    status_ = FlashBackendStatus{};
    jobId_.clear();
}

QString RemoteFlashBackend::BaseUrl() const {
    return QString::fromStdString(client_->baseUrl());
}

bool RemoteFlashBackend::QueueFlash(const std::string& firmwarePath, bool autoGpio) {
    std::string job;
    std::string error;
    const bool queued = client_->flashFile(firmwarePath, autoGpio, &job, &error);
    std::lock_guard lock(mutex_);
    if (!queued) {
        queueError_ = std::move(error);
        status_.lastError = queueError_;
        return false;
    }
    jobId_ = std::move(job);
    queueError_.clear();
    status_.busy = true;
    status_.reachable = true;
    status_.state = "Queued";
    return true;
}

FlashBackendStatus RemoteFlashBackend::Status() {
    std::lock_guard lock(mutex_);
    FlashBackendStatus result = status_;
    if (!queueError_.empty()) result.lastError = queueError_;
    return result;
}

void RemoteFlashBackend::PollLoop() {
    while (run_.load()) {
        std::string job;
        {
            std::lock_guard lock(mutex_);
            job = jobId_;
        }
        if (!job.empty()) {
            const auto remote = client_->flashStatus(job);
            FlashBackendStatus current;
            current.reachable = remote.reachable;
            current.state = remote.state;
            current.busy = remote.busy;
            current.progress = remote.progress;
            current.lastError = remote.lastError;
            current.log = remote.log;
            std::lock_guard lock(mutex_);
            status_ = std::move(current);
        }
        for (int i = 0; i < 5 && run_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

}  // namespace NodeGUI::runtime
