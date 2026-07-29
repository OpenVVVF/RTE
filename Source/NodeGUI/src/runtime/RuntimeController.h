#pragma once

#include "TelemetryStore.h"

#include <inverter_protocol/host/host_client.h>

#include <QObject>
#include <QString>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <variant>
#include <vector>

class QTimer;

namespace NodeGUI::runtime {

// Bridges the threaded ivp::InverterClient into the Qt world. Client callbacks
// fire on the client's worker thread; they only append to a pending queue. A
// ~33 ms QTimer on the GUI thread drains the queue into the TelemetryStore and
// emits storeChanged() once per batch.
//
// With simulate=true no serial port is opened; synthetic 100 Hz signals are
// fed through the same path (used for UI verification without hardware).
class RuntimeController : public QObject {
    Q_OBJECT

public:
    RuntimeController(QString port, bool simulate, QObject* parent = nullptr);
    ~RuntimeController() override;

    void Start();

    // Sends a text shell command line. Returns false if the client is not
    // running (e.g. suspended for flashing). Echoes the command into the
    // console (UI path).
    bool SendCommand(const QString& line);

    // Sends a text shell command line without echoing into the console (HTTP
    // API path; response lines are collected by the caller via the console).
    bool SendCommandRaw(const std::string& line);

    TelemetryStore& Store() { return store_; }
    const TelemetryStore& Store() const { return store_; }

    QString Port() const { return port_; }
    bool IsSimulating() const { return simulate_; }

    // Frees the serial port for the firmware updater and back.
    void SuspendForFlash();
    void ResumeAfterFlash();
    bool IsSuspended() const { return suspended_; }

signals:
    void storeChanged();

private:
    struct F32Item {
        std::string key;
        float value;
        float tsec;
    };
    struct StringItem {
        std::string key;
        std::string value;
    };
    struct ConsoleItem {
        std::string text;
    };
    struct StatsItem {
        float rxHz;
        float rxBytesPerSec;
        uint64_t goodFrames;
        uint64_t badFrames;
        uint64_t rejectCrc;
        uint64_t rejectHdr;
        uint64_t rejectLen;
        uint32_t seq;
    };
    using PendingItem = std::variant<F32Item, StringItem, ConsoleItem, StatsItem>;

    void Push(PendingItem item);
    void DrainQueue();
    void TickSimulator();
    float NowSec() const;

    QString port_;
    bool simulate_ = false;
    bool suspended_ = false;

    ivp::InverterClient client_;
    TelemetryStore store_;
    QTimer* drainTimer_ = nullptr;

    std::mutex queueMtx_;
    std::vector<PendingItem> queue_;

    // Simulator state.
    QTimer* simTimer_ = nullptr;
    uint64_t simTick_ = 0;
    std::chrono::steady_clock::time_point startTime_;
};

}  // namespace NodeGUI::runtime
