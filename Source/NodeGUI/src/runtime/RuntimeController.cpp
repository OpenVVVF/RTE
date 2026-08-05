#include "RuntimeController.h"

#include <QFutureWatcher>
#include <QMetaObject>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <cmath>
#include <random>

namespace NodeGUI::runtime {
namespace {

struct SimWave { const char* name; double freq; double amplitude; double offset; };
constexpr SimWave kSimWaves[] = {
    {"vdc_v", 0.2, 0.5, 24.0}, {"ph_u_a", 2.0, 8.0, 0.0},
    {"ph_v_a", 2.0, 8.0, 0.0}, {"ph_w_a", 2.0, 8.0, 0.0},
    {"id_a", 2.0, 3.0, 0.0}, {"iq_a", 0.5, 5.0, 2.0},
    {"V_bus", 0.1, 0.2, 24.0}, {"I_ROTOR_speed", 0.3, 50.0, 400.0},
    {"enc_angle_deg", 0.25, 180.0, 180.0}, {"temp_c", 0.05, 1.5, 35.0},
};

constexpr int kTelemetryDrainIntervalMs = 8;  // 125 Hz ceiling for ~120 Hz input
constexpr int kPresentationIntervalMs = 33;   // text-heavy panels do not need 120 Hz

struct CommandResult {
    std::string command;
    std::string error;
    bool sent = false;
};

}  // namespace

RuntimeController::RuntimeController(
    std::shared_ptr<rte::runtime::GatewayClient> gateway,
    QString serverUrl,
    QString serialPort,
    bool simulate,
    Protocol protocol,
    QObject* parent)
    : QObject(parent), gateway_(std::move(gateway)), serverUrl_(std::move(serverUrl)),
      serialPort_(std::move(serialPort)), simulate_(simulate), protocol_(protocol),
      startTime_(std::chrono::steady_clock::now()) {
    // Preserve command ordering without ever doing HTTP work on Qt's GUI
    // thread. A dedicated one-thread pool also avoids consuming the global
    // application pool with a slow remote request.
    commandPool_.setMaxThreadCount(1);
    gateway_->onF32 = [this](const std::string& key, float value, float tsec) {
        Push(F32Item{key, value, tsec});
    };
    gateway_->onString = [this](const std::string& key, const std::string& value) {
        Push(StringItem{key, value});
    };
    gateway_->onConsole = [this](uint64_t, const std::string& line) {
        Push(ConsoleItem{line});
    };
    gateway_->onStats = [this](const rte::runtime::GatewayClientStats& stats) {
        Push(StatsItem{stats});
    };
    gateway_->onConnection = [this](bool connected, const std::string& detail) {
        QMetaObject::invokeMethod(this, [this, connected, detail] {
            connected_ = connected;
            emit connectionChanged(connected, QString::fromStdString(detail));
        }, Qt::QueuedConnection);
    };
    gateway_->onLease = [this](bool held, const std::string& owner) {
        QMetaObject::invokeMethod(this, [this, held, owner] {
            emit controlChanged(held, QString::fromStdString(owner));
        }, Qt::QueuedConnection);
    };
}

RuntimeController::~RuntimeController() {
    gateway_->stopEvents();
    commandPool_.waitForDone();
}

void RuntimeController::Start() {
    started_ = true;
    if (simulate_) {
        simTimer_ = new QTimer(this);
        simTimer_->setInterval(10);
        connect(simTimer_, &QTimer::timeout, this, &RuntimeController::TickSimulator);
        simTimer_->start();
    } else {
        gateway_->startEvents();
    }
    drainTimer_ = new QTimer(this);
    drainTimer_->setTimerType(Qt::PreciseTimer);
    drainTimer_->setInterval(kTelemetryDrainIntervalMs);
    connect(drainTimer_, &QTimer::timeout, this, &RuntimeController::DrainQueue);
    drainTimer_->start();

    presentationTimer_ = new QTimer(this);
    presentationTimer_->setInterval(kPresentationIntervalMs);
    connect(presentationTimer_, &QTimer::timeout, this, [this] {
        if (!storeDirty_) return;
        storeDirty_ = false;
        emit storeChanged();
    });
    presentationTimer_->start();
}

void RuntimeController::ConnectTo(const QString& serverUrl) {
    if (simulate_) return;
    serverUrl_ = serverUrl;
    gateway_->setBaseUrl(serverUrl.toStdString());
}

void RuntimeController::SendCommand(const QString& line) {
    const std::string command = line.toStdString();
    store_.AddConsoleLine("> " + command);
    storeDirty_ = true;
    if (simulate_) {
        store_.AddCommand(command, "ui", false);
        store_.AddConsoleLine("(simulated: no device)");
        emit storeChanged();
        return;
    }

    auto* watcher = new QFutureWatcher<CommandResult>(this);
    connect(watcher, &QFutureWatcher<CommandResult>::finished, this,
            [this, watcher] {
        const CommandResult result = watcher->result();
        store_.AddCommand(result.command, "ui", result.sent);
        if (!result.sent) store_.AddConsoleLine("(FAILED: " + result.error + ")");
        emit storeChanged();
        watcher->deleteLater();
    });
    const auto gateway = gateway_;
    watcher->setFuture(QtConcurrent::run(&commandPool_, [gateway, command] {
        CommandResult result;
        result.command = command;
        result.sent = gateway->sendCommand(command, nullptr, &result.error);
        return result;
    }));
}

bool RuntimeController::TakeControl(QString* error) {
    std::string detail;
    const bool acquired = gateway_->acquireLease("NodeGUI", &detail);
    if (error) *error = QString::fromStdString(detail);
    return acquired;
}

bool RuntimeController::ReleaseControl(QString* error) {
    std::string detail;
    const bool released = gateway_->releaseLease(&detail);
    if (error) *error = QString::fromStdString(detail);
    return released;
}

bool RuntimeController::HasControl() const { return gateway_->hasLease(); }
QString RuntimeController::ControlOwner() const {
    return QString::fromStdString(gateway_->leaseOwner());
}

RuntimeSessionSnapshot RuntimeController::CaptureSession() {
    DrainQueue();
    return store_.SessionSnapshot();
}

void RuntimeController::ClearSession() {
    DrainQueue();
    store_.ClearSession();
    storeDirty_ = false;
    emit sessionCleared();
    emit telemetryChanged();
    emit storeChanged();
}

void RuntimeController::Push(PendingItem item) {
    std::lock_guard lock(queueMtx_);
    queue_.push_back(std::move(item));
}

void RuntimeController::DrainQueue() {
    std::vector<PendingItem> items;
    {
        std::lock_guard lock(queueMtx_);
        if (queue_.empty()) return;
        items.swap(queue_);
    }
    bool hasFloatTelemetry = false;
    for (const auto& item : items) {
        std::visit([this, &hasFloatTelemetry](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, F32Item>) {
                store_.AddF32(value.key, value.value, value.tsec);
                hasFloatTelemetry = true;
            } else if constexpr (std::is_same_v<T, StringItem>) {
                store_.AddString(value.key, value.value);
            } else if constexpr (std::is_same_v<T, ConsoleItem>) {
                store_.AddConsoleLine(value.text);
            } else {
                const auto& s = value.stats;
                store_.SetStats(s.rxHz, s.rxBytesPerSec, s.goodFrames, s.badFrames,
                                s.rejectCrc, s.rejectHdr, s.rejectLen,
                                s.rejectPayloadParse, s.rejectUnknownId, s.lastSeq);
                store_.SetSuspended(s.suspended);
            }
        }, item);
    }
    // Plot history is published at the source cadence. Tables, labels, and
    // console layout are deliberately published by presentationTimer_ so a
    // text burst cannot consume the graph's update budget.
    if (hasFloatTelemetry) emit telemetryChanged();
    storeDirty_ = true;
}

void RuntimeController::TickSimulator() {
    ++simTick_;
    const float t = NowSec();
    static std::mt19937 random{42};
    std::uniform_real_distribution<float> noise(-0.05f, 0.05f);
    for (std::size_t i = 0; i < std::size(kSimWaves); ++i) {
        const auto& wave = kSimWaves[i];
        const double phase = 2.0 * M_PI * wave.freq * t + i * 1.1;
        Push(F32Item{wave.name,
                     static_cast<float>(wave.offset + wave.amplitude * std::sin(phase))
                         + noise(random), t});
    }
    if (simTick_ % 100 == 0) {
        Push(ConsoleItem{"sim: tick " + std::to_string(simTick_)});
        rte::runtime::GatewayClientStats stats;
        stats.rxHz = 100.0f;
        stats.rxBytesPerSec = 4000.0f;
        stats.goodFrames = simTick_;
        stats.lastSeq = static_cast<uint32_t>(simTick_);
        Push(StatsItem{stats});
    }
}

float RuntimeController::NowSec() const {
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - startTime_)
        .count();
}

}  // namespace NodeGUI::runtime
