#pragma once

#include "LegacyTelemetryClient.h"
#include "TelemetryStore.h"

#include <inverter_protocol/host/host_client.h>

#include <QObject>
#include <QString>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <variant>
#include <vector>

class QTimer;

namespace NodeGUI::runtime {

enum class Protocol {
    Legacy,
    Inverter,
};

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

    bool SendCommand(const QString& line);
    bool SendCommandRaw(const std::string& line);

    TelemetryStore& Store() { return store_; }
    const TelemetryStore& Store() const { return store_; }

    // Drains the last queued device batch, then returns the complete runtime
    // session accumulated since this controller was created.
    RuntimeSessionSnapshot CaptureSession();

    // Discards both rolling UI data and the full export archive, then starts
    // a new session at the current time.
    void ClearSession();

    QString Port() const { return port_; }
    QString ConnectionLabel() const;
    bool IsSimulating() const { return simulate_; }
    Protocol GetProtocol() const { return protocol_; }

    void SuspendForFlash();
    void ResumeAfterFlash();
    bool IsSuspended() const { return suspended_; }

    bool IsSimPaused() const { return sim_paused_; }
    void SetSimPaused(bool paused);
    void ToggleSimPause();

    double SimSpeedFactor() const { return sim_speed_factor_; }
    bool SimSpeedTurbo() const { return sim_speed_turbo_; }
    void SetSimSpeed(double factor, bool turbo = false);

    double LastSimTimeSec() const { return last_sim_time_sec_; }

signals:
    void storeChanged();
    void simPauseChanged(bool paused);
    void plotTimeFreezeChanged(bool frozen, double anchorSimSec);
    void simSpeedChanged(double factor);
    void sessionCleared();

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
        uint64_t rejectPayloadParse;
        uint64_t rejectUnknownId;
        uint32_t seq;
    };
    struct IngressAcc {
        float min_v = 0.0f;
        float max_v = 0.0f;
        float last_v = 0.0f;
        float first_t = 0.0f;
        float last_t = 0.0f;
        int count = 0;

        void Push(float value, float tsec) {
            if (count == 0) {
                min_v = max_v = last_v = value;
                first_t = last_t = tsec;
                count = 1;
                return;
            }
            min_v = std::min(min_v, value);
            max_v = std::max(max_v, value);
            last_v = value;
            last_t = tsec;
            ++count;
        }
    };

    // pwm_gate_*: keep every 0/1 transition (scope edges), not batch min/max.
    struct GateIngressAcc {
        float last_v = 0.0f;
        bool has_last = false;
        std::vector<std::pair<float, float>> edges;

        void Push(float value, float tsec) {
            if (!has_last) {
                has_last = true;
                last_v = value;
                edges.emplace_back(tsec, value);
                return;
            }
            if (value != last_v) {
                edges.emplace_back(tsec, last_v);
                edges.emplace_back(tsec, value);
                last_v = value;
            }
            constexpr std::size_t kCap = 512;
            if (edges.size() > kCap) {
                edges.erase(edges.begin(),
                            edges.begin() + static_cast<std::ptrdiff_t>(edges.size() - kCap / 2));
            }
        }
    };

    using PendingItem = std::variant<StringItem, ConsoleItem, StatsItem>;

    void PushF32(const std::string& key, float value, float tsec);
    void Push(PendingItem item);
    void DrainQueue();
    void TickSimulator();
    float NowSec() const;
    bool SendLine(const std::string& line);
    void MaybeEmitStoreChanged();

    QString port_;
    QString tcpHost_;
    int tcpPort_ = 0;
    bool simulate_ = false;
    Protocol protocol_;
    bool suspended_ = false;
    bool sim_paused_ = false;
    double last_sim_time_sec_ = 0.0;
    double sim_speed_factor_ = 1.0;
    bool sim_speed_turbo_ = false;

    LegacyTelemetryClient legacyClient_;
    ivp::InverterClient ivpClient_;
    TelemetryStore store_;
    QTimer* drainTimer_ = nullptr;

    std::mutex ingressMtx_;
    std::unordered_map<std::string, GateIngressAcc> gateIngress_;
    std::unordered_map<std::string, IngressAcc> pwmIngress_;
    std::vector<F32Item> plantIngress_;
    std::vector<PendingItem> otherQueue_;

    std::chrono::steady_clock::time_point lastStoreEmit_{};

    QTimer* simTimer_ = nullptr;
    uint64_t simTick_ = 0;
    std::chrono::steady_clock::time_point startTime_;
};

}  // namespace NodeGUI::runtime
