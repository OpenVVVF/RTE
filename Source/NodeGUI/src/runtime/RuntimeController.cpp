#include "RuntimeController.h"

#include <QTimer>

#include <cmath>
#include <random>

namespace NodeGUI::runtime {

namespace {

// Synthetic signals for --simulate mode: name, frequency (Hz), amplitude,
// offset.
struct SimWave {
    const char* name;
    double freq;
    double amplitude;
    double offset;
};

constexpr SimWave kSimWaves[] = {
    {"vdc_v", 0.2, 0.5, 24.0},
    {"ph_u_a", 2.0, 8.0, 0.0},
    {"ph_v_a", 2.0, 8.0, 0.0},
    {"ph_w_a", 2.0, 8.0, 0.0},
    {"id_a", 2.0, 3.0, 0.0},
    {"iq_a", 0.5, 5.0, 2.0},
    {"V_bus", 0.1, 0.2, 24.0},
    {"I_ROTOR_speed", 0.3, 50.0, 400.0},
    {"enc_angle_deg", 0.25, 180.0, 180.0},
    {"temp_c", 0.05, 1.5, 35.0},
};

}  // namespace

RuntimeController::RuntimeController(QString serverHost,
                                     int bridgePort,
                                     QString serialPort,
                                     bool simulate,
                                     Protocol protocol,
                                     QObject* parent)
    : QObject(parent)
    , serverHost_(std::move(serverHost))
    , bridgePort_(bridgePort)
    , serialPort_(std::move(serialPort))
    , simulate_(simulate)
    , protocol_(protocol)
    , startTime_(std::chrono::steady_clock::now()) {
    legacyClient_.onF32 = [this](const std::string& key, float value, float tsec) {
        Push(F32Item{key, value, tsec});
    };
    legacyClient_.onString = [this](const std::string& key, const std::string& value) {
        Push(StringItem{key, value});
    };
    legacyClient_.onConsole = [this](const std::string& line) { Push(ConsoleItem{line}); };
    legacyClient_.onStats = [this](const LegacyTelemetryClient::Stats& s) {
        Push(StatsItem{s.rxHz,
                       s.rxBytesPerSec,
                       s.goodFrames,
                       s.badFrames,
                       s.rejectCrc,
                       s.rejectHdr,
                       s.rejectLen,
                       s.rejectPayloadParse,
                       s.rejectUnknownId,
                       s.lastSeq});
    };

    ivpClient_.onF32Value([this](uint16_t, const std::string& key, float value, uint32_t) {
        Push(F32Item{key, value, NowSec()});
    });
    ivpClient_.onStringValue([this](uint16_t, const std::string& key, const std::string& value,
                                    uint32_t) { Push(StringItem{key, value}); });
    ivpClient_.onConsoleLine([this](const std::string& line) { Push(ConsoleItem{line}); });
    ivpClient_.onStats([this](const ivp::ClientStats& s) {
        Push(StatsItem{s.rx_hz,
                       s.rx_bytes_per_sec,
                       s.good_frames,
                       s.bad_frames,
                       s.reject_crc,
                       s.reject_hdr,
                       s.reject_len,
                       0,
                       0,
                       s.last_seq});
    });
}

RuntimeController::~RuntimeController() {
    legacyClient_.stop();
    ivpClient_.stop();
}

void RuntimeController::Start() {
    started_ = true;
    if (simulate_) {
        simTimer_ = new QTimer(this);
        simTimer_->setInterval(10);  // 100 Hz
        connect(simTimer_, &QTimer::timeout, this, &RuntimeController::TickSimulator);
        simTimer_->start();
    } else if (protocol_ == Protocol::Legacy) {
        legacyClient_.startTcp(serverHost_.toStdString(), bridgePort_);
    } else {
        ivpClient_.start(serialPort_.toStdString());
    }

    drainTimer_ = new QTimer(this);
    drainTimer_->setInterval(33);  // ~30 Hz GUI updates
    connect(drainTimer_, &QTimer::timeout, this, &RuntimeController::DrainQueue);
    drainTimer_->start();
}

void RuntimeController::ConnectTo(const QString& serverHost, int bridgePort) {
    if (simulate_) {
        return;
    }
    serverHost_ = serverHost;
    bridgePort_ = bridgePort;
    if (protocol_ != Protocol::Legacy) {
        return;
    }
    legacyClient_.stop();
    if (started_ && !suspended_) {
        legacyClient_.startTcp(serverHost_.toStdString(), bridgePort_);
    }
}

bool RuntimeController::SendLine(const std::string& line) {
    if (suspended_ || simulate_) {
        return false;
    }
    return protocol_ == Protocol::Legacy ? legacyClient_.sendLine(line)
                                         : ivpClient_.sendCommandLine(line);
}

bool RuntimeController::SendCommand(const QString& line) {
    store_.AddConsoleLine("> " + line.toStdString());
    const bool ok = SendLine(line.toStdString());
    store_.AddCommand(line.toStdString(), "ui", ok);
    if (!ok) {
        store_.AddConsoleLine(simulate_ ? "(simulated: no device)"
                                        : (suspended_ ? "(suspended)" : "(FAILED to send)"));
        return false;
    }
    store_.AddConsoleLine("(sent)");
    return true;
}

bool RuntimeController::SendCommandRaw(const std::string& line) {
    const bool ok = SendLine(line);
    store_.AddCommand(line, "api", ok);
    return ok;
}

RuntimeSessionSnapshot RuntimeController::CaptureSession() {
    DrainQueue();
    return store_.SessionSnapshot();
}

void RuntimeController::ClearSession() {
    DrainQueue();
    store_.ClearSession();
    emit sessionCleared();
    emit storeChanged();
}

void RuntimeController::SuspendForFlash() {
    if (suspended_) {
        return;
    }
    suspended_ = true;
    store_.SetSuspended(true);
    if (simulate_) {
        return;
    }
    if (protocol_ == Protocol::Legacy) {
        legacyClient_.suspend();
    } else {
        ivpClient_.stop();
    }
}

void RuntimeController::ResumeAfterFlash() {
    if (!suspended_) {
        return;
    }
    if (!simulate_) {
        if (protocol_ == Protocol::Legacy) {
            legacyClient_.resume();
        } else {
            ivpClient_.start(serialPort_.toStdString());
        }
    }
    suspended_ = false;
    store_.SetSuspended(false);
}

void RuntimeController::Push(PendingItem item) {
    std::lock_guard lock(queueMtx_);
    queue_.push_back(std::move(item));
}

void RuntimeController::DrainQueue() {
    std::vector<PendingItem> items;
    {
        std::lock_guard lock(queueMtx_);
        if (queue_.empty()) {
            return;
        }
        items.swap(queue_);
    }

    for (const auto& item : items) {
        std::visit(
            [this](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, F32Item>) {
                    store_.AddF32(v.key, v.value, v.tsec);
                } else if constexpr (std::is_same_v<T, StringItem>) {
                    store_.AddString(v.key, v.value);
                } else if constexpr (std::is_same_v<T, ConsoleItem>) {
                    store_.AddConsoleLine(v.text);
                } else {
                    store_.SetStats(v.rxHz,
                                    v.rxBytesPerSec,
                                    v.goodFrames,
                                    v.badFrames,
                                    v.rejectCrc,
                                    v.rejectHdr,
                                    v.rejectLen,
                                    v.rejectPayloadParse,
                                    v.rejectUnknownId,
                                    v.seq);
                }
            },
            item);
    }

    emit storeChanged();
}

void RuntimeController::TickSimulator() {
    ++simTick_;
    const float t = NowSec();

    static std::mt19937 rng{42};
    std::uniform_real_distribution<float> noise(-0.05f, 0.05f);

    for (std::size_t i = 0; i < std::size(kSimWaves); ++i) {
        const auto& w = kSimWaves[i];
        const double phase = 2.0 * M_PI * w.freq * t + i * 1.1;
        const float value = static_cast<float>(w.offset + w.amplitude * std::sin(phase))
                            + noise(rng);
        Push(F32Item{w.name, value, t});
    }

    // Occasional console output so the console path is exercised.
    if (simTick_ % 100 == 0) {
        Push(ConsoleItem{"sim: tick " + std::to_string(simTick_)});
    }

    // Stats every second.
    if (simTick_ % 100 == 0) {
        Push(StatsItem{100.0f,
                       100.0f * 40.0f,
                       simTick_ / 100 * 100,
                       0,
                       0,
                       0,
                       0,
                       0,
                       0,
                       static_cast<uint32_t>(simTick_)});
    }
}

float RuntimeController::NowSec() const {
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - startTime_)
        .count();
}

}  // namespace NodeGUI::runtime
