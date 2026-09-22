#pragma once

#include "IvpTcpClient.h"
#include "LegacyTelemetryClient.h"
#include "PendingQueue.h"
#include "TelemetryStore.h"

#include <inverter_protocol/host/host_client.h>

#include <QObject>
#include <QString>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

class QTimer;

namespace NodeGUI::runtime {

// Which wire protocol the device speaks.
//  Legacy   - the protocol the current firmware runs (ported from the old
//             ImGui client). Default.
//  Inverter - the new Lib/InverterProtocol stack (ivp), for future firmware.
enum class Protocol {
    Legacy,
    Inverter,
};

// Bridges the telemetry client(s) into the Qt world. Client callbacks fire on
// the client's worker thread (Legacy/Inverter) or on the GUI thread (TCP);
// they only append to a pending queue. A ~33 ms QTimer on the GUI thread
// drains the queue into the TelemetryStore and emits storeChanged() once per
// batch.
//
// With simulate=true no link is opened; synthetic 100 Hz signals are fed
// through the same path (used for UI verification without hardware). When a
// TCP endpoint is given (--tcp host:port), the COBS-framed InverterProtocol
// stream from HostSim --live is used instead of the serial port.
class RuntimeController : public QObject {
    Q_OBJECT

public:
    RuntimeController(QString port,
                      bool simulate,
                      Protocol protocol = Protocol::Legacy,
                      QString tcpHost = {},
                      int tcpPort = 0,
                      QObject* parent = nullptr);
    ~RuntimeController() override;

    void Start();

    // Sends a text shell command line. Returns false if the client is not
    // running (e.g. suspended for flashing). Echoes the command into the
    // console (UI path).
    bool SendCommand(const QString& line);

    // Sends an external command and records its source, exact text, and send
    // result in the visible console and session command history.
    bool SendCommandRaw(const std::string& line, const std::string& source);

    // Adds an MCP action to the visible console and exportable session.
    void AddAutomationLine(const std::string& line);

    TelemetryStore& Store() { return store_; }
    const TelemetryStore& Store() const { return store_; }

    // Drains the last queued device batch, then returns the complete runtime
    // session accumulated since this controller was created.
    RuntimeSessionSnapshot CaptureSession();

    // Discards both rolling UI data and the full export archive, then starts
    // a new session at the current time.
    void ClearSession();

    QString Port() const;
    void SetPort(const QString& port);
    bool IsSimulating() const { return simulate_; }
    Protocol GetProtocol() const { return protocol_; }

    // True when --tcp host:port was given; the TCP IVP link replaces the
    // serial port for this run.
    bool UsingTcp() const { return !tcpHost_.isEmpty() && tcpPort_ > 0; }

    // Temporarily switches the live link to a HostSim --live TCP endpoint
    // (Build & Run Simulation attach) without changing the configured link;
    // ClearLinkOverride() restores it. The TCP client reconnects on its own
    // until host_sim is listening, so calling this before the simulator is up
    // is safe. Wildcard bind hosts (0.0.0.0/::) are normalized to loopback,
    // and re-attaching to the endpoint already in use is a no-op.
    void ConnectTcpOverride(const QString& host, int port);
    void ClearLinkOverride();
    bool HasLinkOverride() const { return linkOverride_; }

    // True while the TCP IVP client holds an open connection, on the
    // configured --tcp link or an active override.
    bool IsTcpConnected() const;

    // Frees the serial port for the firmware updater and back.
    void SuspendForFlash();
    void ResumeAfterFlash();
    bool IsSuspended() const { return suspended_; }

signals:
    void storeChanged();
    void sessionCleared();

private:
    // Client callbacks fire from arbitrary producer threads; Push() appends
    // to pending_ and the ~33 ms GUI timer drains. See PendingQueue for the
    // bounding policy when the GUI stalls (coalesce/drop, counters reported
    // throttled into the console).
    void Push(PendingItem item);
    void DrainQueue();
    void NoteQueueBacklog(uint64_t coalesced, uint64_t dropped);
    void TickSimulator();
    float NowSec() const;
    bool SendLine(const std::string& line);
    // Starts/stops whichever link the current configuration selects
    // (override TCP, simulated feed, configured TCP, or serial).
    void StartActiveLink();
    void StopActiveLink();

    QString port_;
    QString tcpHost_;
    int tcpPort_ = 0;
    bool simulate_ = false;
    Protocol protocol_;
    bool suspended_ = false;
    bool linkOverride_ = false;
    QString overrideHost_;
    int overridePort_ = 0;

    // Only the backend matching the link selection (simulate / tcp / protocol)
    // is started.
    LegacyTelemetryClient legacyClient_;
    ivp::InverterClient ivpClient_;
    IvpTcpClient tcpClient_;
    TelemetryStore store_;
    QTimer* drainTimer_ = nullptr;

    PendingQueue pending_;
    // GUI thread only: throttles the console notice about queue drops.
    std::chrono::steady_clock::time_point lastQueueNotice_{};

    // Simulator state.
    QTimer* simTimer_ = nullptr;
    uint64_t simTick_ = 0;
    std::chrono::steady_clock::time_point startTime_;
};

}  // namespace NodeGUI::runtime
