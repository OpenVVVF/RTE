#pragma once

#include "GatewayClient.h"
#include "TelemetryStore.h"

#include <QObject>
#include <QString>
#include <QThreadPool>

#include <chrono>
#include <memory>
#include <mutex>
#include <variant>
#include <vector>

class QTimer;

namespace NodeGUI::runtime {

enum class Protocol { Legacy, Inverter };

// Qt presentation adapter for the shared gateway client. The gateway is the
// sole parser and serial owner; this class only batches decoded API events
// onto the GUI thread and maintains the local plotting/session store.
class RuntimeController : public QObject {
    Q_OBJECT

public:
    RuntimeController(std::shared_ptr<rte::runtime::GatewayClient> gateway,
                      QString serverUrl,
                      QString serialPort,
                      bool simulate,
                      Protocol protocol = Protocol::Legacy,
                      QObject* parent = nullptr);
    ~RuntimeController() override;

    void Start();
    void ConnectTo(const QString& serverUrl);
    void SendCommand(const QString& line);

    bool TakeControl(QString* error = nullptr);
    bool ReleaseControl(QString* error = nullptr);
    bool HasControl() const;
    QString ControlOwner() const;
    QString LeaseId() const { return QString::fromStdString(gateway_->leaseId()); }

    rte::runtime::TelemetryStore& Store() { return store_; }
    const rte::runtime::TelemetryStore& Store() const { return store_; }
    rte::runtime::RuntimeSessionSnapshot CaptureSession();
    void ClearSession();

    QString Port() const { return serialPort_; }
    QString ServerUrl() const { return serverUrl_; }
    bool IsSimulating() const { return simulate_; }
    Protocol GetProtocol() const { return protocol_; }
    bool IsConnected() const { return connected_; }
    bool IsDeviceConnected() const { return deviceConnected_; }

signals:
    void telemetryChanged();
    void storeChanged();
    void sessionCleared();
    void connectionChanged(bool connected, const QString& detail);
    void deviceConnectionChanged(bool connected, const QString& port);
    void controlChanged(bool held, const QString& owner);

private:
    struct F32Item { std::string key; float value; float tsec; };
    struct StringItem { std::string key; std::string value; };
    struct ConsoleItem { std::string text; };
    struct StatsItem { rte::runtime::GatewayClientStats stats; };
    struct ResetItem {};
    using PendingItem =
        std::variant<F32Item, StringItem, ConsoleItem, StatsItem, ResetItem>;

    void Push(PendingItem item);
    void DrainQueue();
    void TickSimulator();
    float NowSec() const;

    std::shared_ptr<rte::runtime::GatewayClient> gateway_;
    QThreadPool commandPool_;
    QString serverUrl_;
    QString serialPort_;
    bool simulate_ = false;
    Protocol protocol_;
    bool started_ = false;
    bool connected_ = false;
    bool deviceConnected_ = false;

    rte::runtime::TelemetryStore store_;
    QTimer* drainTimer_ = nullptr;
    QTimer* presentationTimer_ = nullptr;
    bool storeDirty_ = false;
    std::mutex queueMtx_;
    std::vector<PendingItem> queue_;
    QTimer* simTimer_ = nullptr;
    uint64_t simTick_ = 0;
    std::chrono::steady_clock::time_point startTime_;
};

}  // namespace NodeGUI::runtime
