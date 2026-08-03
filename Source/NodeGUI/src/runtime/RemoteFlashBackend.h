#pragma once

#include "FlashBackend.h"

#include <QString>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace NodeGUI::runtime {

// FlashBackend over the RTEServer HTTP API: POST /flash to queue, and a
// polling thread keeps GET /flash/status cached for Status().
class RemoteFlashBackend : public FlashBackend {
public:
    RemoteFlashBackend(QString host, int httpPort);
    ~RemoteFlashBackend() override;

    RemoteFlashBackend(const RemoteFlashBackend&) = delete;
    RemoteFlashBackend& operator=(const RemoteFlashBackend&) = delete;

    // Re-points the backend at a (possibly different) server.
    void SetServer(const QString& host, int httpPort);

    QString Host() const;
    int HttpPort() const;

    bool QueueFlash(const std::string& firmwarePath, bool autoGpio) override;
    FlashBackendStatus Status() override;

private:
    void PollLoop();

    mutable std::mutex mtx_;
    QString host_;
    int httpPort_ = 18080;
    FlashBackendStatus status_;
    std::string queueError_;

    std::atomic<bool> run_{true};
    std::thread pollThread_;
};

}  // namespace NodeGUI::runtime
