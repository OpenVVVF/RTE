#pragma once

#include "GatewayClient.h"
#include "TelemetryStore.h"

#include <QObject>
#include <QString>

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
    bool SendCommand(const QString& line);

    bool TakeControl(QString* error = nullptr);
    bool ReleaseControl(QString* error = nullptr);
    bool HasControl() const;
    QString ControlOwner() const;
    QString LeaseId() const { return QString::fromStdString(gateway_->leaseId()); }

    TelemetryStore& Store() { return store_; }
    const TelemetryStore& Store() const { return store_; }
    RuntimeSessionSnapshot CaptureSession();
    void ClearSession();

    QString Port() const { return serialPort_; }
    QString ServerUrl() const { return serverUrl_; }
    bool IsSimulating() const { return simulate_; }
    Protocol GetProtocol() const { return protocol_; }
    bool IsConnected() const { return connected_; }

signals:
    void storeChanged();
    void sessionCleared();
    void connectionChanged(bool connected, const QString& detail);
    void controlChanged(bool held, const QString& owner);

private:
    struct F32Item { std::string key; float value; float tsec; };
    struct StringItem { std::string key; std::string value; };
    struct ConsoleItem { std::string text; };
    struct StatsItem { rte::runtime::GatewayClientStats stats; };
    using PendingItem = std::variant<F32Item, StringItem, ConsoleItem, StatsItem>;

    void Push(PendingItem item);
    void DrainQueue();
    void TickSimulator();
    float NowSec() const;

    std::shared_ptr<rte::runtime::GatewayClient> gateway_;
    QString serverUrl_;
    QString serialPort_;
    bool simulate_ = false;
    Protocol protocol_;
    bool started_ = false;
    bool connected_ = false;

    TelemetryStore store_;
    QTimer* drainTimer_ = nullptr;
    std::mutex queueMtx_;
    std::vector<PendingItem> queue_;
    QTimer* simTimer_ = nullptr;
    uint64_t simTick_ = 0;
    std::chrono::steady_clock::time_point startTime_;
};

}  // namespace NodeGUI::runtime
