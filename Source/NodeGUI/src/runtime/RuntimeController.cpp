#include "RuntimeController.h"

#include <QTimer>

#include <algorithm>
#include <cmath>
#include <random>
#include <tuple>

namespace NodeGUI::runtime {

namespace {

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

constexpr std::size_t kMaxPlantIngress = 3000;
constexpr std::size_t kMaxOtherQueue = 512;

bool IsPwmGateKey(const std::string& key) {
    return key.rfind("pwm_gate_", 0) == 0;
}

bool IsHighRatePwmKey(const std::string& key) {
    return key.rfind("pwm_", 0) == 0;
}

}  // namespace

RuntimeController::RuntimeController(QString port,
                                     bool simulate,
                                     Protocol protocol,
                                     QString tcpHost,
                                     int tcpPort,
                                     QObject* parent)
    : QObject(parent)
    , port_(std::move(port))
    , tcpHost_(std::move(tcpHost))
    , tcpPort_(tcpPort)
    , simulate_(simulate)
    , protocol_(protocol)
    , startTime_(std::chrono::steady_clock::now()) {
    legacyClient_.onF32 = [this](const std::string& key, float value, float tsec) {
        PushF32(key, value, tsec);
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

    ivpClient_.onF32Value([this](uint16_t, const std::string& key, float value, uint32_t time_us) {
        PushF32(key, value, static_cast<float>(time_us) * 1e-6f);
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
    if (simulate_) {
        simTimer_ = new QTimer(this);
        simTimer_->setInterval(10);
        connect(simTimer_, &QTimer::timeout, this, &RuntimeController::TickSimulator);
        simTimer_->start();
    } else if (protocol_ == Protocol::Legacy) {
        legacyClient_.start(port_.toStdString());
    } else if (!tcpHost_.isEmpty() && tcpPort_ > 0) {
        ivpClient_.startTcp(tcpHost_.toStdString(), tcpPort_);
    } else {
        ivpClient_.start(port_.toStdString());
    }

    drainTimer_ = new QTimer(this);
    drainTimer_->setInterval(40);
    connect(drainTimer_, &QTimer::timeout, this, &RuntimeController::DrainQueue);
    drainTimer_->start();
}

QString RuntimeController::ConnectionLabel() const {
    if (simulate_) {
        return QStringLiteral("simulate");
    }
    if (!tcpHost_.isEmpty() && tcpPort_ > 0) {
        return QStringLiteral("tcp %1:%2").arg(tcpHost_).arg(tcpPort_);
    }
    return port_;
}

void RuntimeController::SetSimPaused(bool paused) {
    if (sim_paused_ == paused) {
        return;
    }
    const bool ok = SendCommand(paused ? QStringLiteral("pause")
                                       : QStringLiteral("resume"));
    if (!ok && !simulate_) {
        return;
    }
    sim_paused_ = paused;
    store_.SetHistoryFrozen(paused);
    emit simPauseChanged(sim_paused_);
    emit plotTimeFreezeChanged(sim_paused_, last_sim_time_sec_);
}

void RuntimeController::ToggleSimPause() {
    SetSimPaused(!sim_paused_);
}

void RuntimeController::SetSimSpeed(double factor, bool turbo) {
    if (turbo) {
        sim_speed_turbo_ = true;
        sim_speed_factor_ = 0.0;
        SendCommandRaw("speed turbo");
    } else {
        const double clamped = std::clamp(factor, 0.05, 4.0);
        sim_speed_turbo_ = false;
        sim_speed_factor_ = clamped;
        SendCommandRaw(QString("speed %1").arg(clamped, 0, 'f', 3).toStdString());
    }
    emit simSpeedChanged(sim_speed_turbo_ ? 0.0 : sim_speed_factor_);
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
        } else if (!tcpHost_.isEmpty() && tcpPort_ > 0) {
            ivpClient_.startTcp(tcpHost_.toStdString(), tcpPort_);
        } else {
            ivpClient_.start(port_.toStdString());
        }
    }
    suspended_ = false;
    store_.SetSuspended(false);
}

void RuntimeController::PushF32(const std::string& key, float value, float tsec) {
    std::lock_guard lock(ingressMtx_);
    if (IsPwmGateKey(key)) {
        gateIngress_[key].Push(value, tsec);
        return;
    }
    if (IsHighRatePwmKey(key)) {
        pwmIngress_[key].Push(value, tsec);
        return;
    }
    plantIngress_.push_back(F32Item{key, value, tsec});
    if (plantIngress_.size() > kMaxPlantIngress) {
        plantIngress_.erase(plantIngress_.begin(),
                              plantIngress_.begin() +
                                  static_cast<std::ptrdiff_t>(plantIngress_.size() / 2));
    }
}

void RuntimeController::Push(PendingItem item) {
    std::lock_guard lock(ingressMtx_);
    if (otherQueue_.size() >= kMaxOtherQueue) {
        otherQueue_.erase(otherQueue_.begin(),
                          otherQueue_.begin() + static_cast<std::ptrdiff_t>(kMaxOtherQueue / 2));
    }
    otherQueue_.push_back(std::move(item));
}

void RuntimeController::MaybeEmitStoreChanged() {
    const auto now = std::chrono::steady_clock::now();
    if (lastStoreEmit_.time_since_epoch().count() == 0 ||
        now - lastStoreEmit_ >= std::chrono::milliseconds(50)) {
        lastStoreEmit_ = now;
        emit storeChanged();
    }
}

void RuntimeController::DrainQueue() {
    std::unordered_map<std::string, GateIngressAcc> gate_acc;
    std::unordered_map<std::string, IngressAcc> pwm_acc;
    std::vector<F32Item> plant_items;
    std::vector<PendingItem> other;
    {
        std::lock_guard lock(ingressMtx_);
        gate_acc.swap(gateIngress_);
        pwm_acc.swap(pwmIngress_);
        plant_items.swap(plantIngress_);
        other.swap(otherQueue_);
    }

    if (gate_acc.empty() && pwm_acc.empty() && plant_items.empty() && other.empty()) {
        return;
    }

    std::vector<std::tuple<std::string, float, float>> batch;
    batch.reserve(plant_items.size() + gate_acc.size() * 8 + pwm_acc.size());

    for (const F32Item& f32 : plant_items) {
        last_sim_time_sec_ = std::max(last_sim_time_sec_, static_cast<double>(f32.tsec));
        batch.emplace_back(f32.key, f32.value, f32.tsec);
    }

    for (auto& [key, acc] : gate_acc) {
        for (const auto& [t, v] : acc.edges) {
            last_sim_time_sec_ = std::max(last_sim_time_sec_, static_cast<double>(t));
            batch.emplace_back(key, v, t);
        }
    }

    for (auto& [key, acc] : pwm_acc) {
        last_sim_time_sec_ = std::max(last_sim_time_sec_, static_cast<double>(acc.last_t));
        batch.emplace_back(key, acc.last_v, acc.last_t);
    }

    if (!batch.empty()) {
        store_.AddF32Batch(batch);
    }

    for (const auto& item : other) {
        std::visit(
            [this](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, StringItem>) {
                    store_.AddString(v.key, v.value);
                } else if constexpr (std::is_same_v<T, ConsoleItem>) {
                    store_.AddConsoleLine(v.text);
                    const QString line = QString::fromStdString(v.text);
                    if (line.contains(QStringLiteral("HostSim: simulation paused"))) {
                        if (!sim_paused_) {
                            sim_paused_ = true;
                            store_.SetHistoryFrozen(true);
                            emit simPauseChanged(true);
                            emit plotTimeFreezeChanged(true, last_sim_time_sec_);
                        }
                    } else if (line.contains(QStringLiteral("HostSim: simulation resumed"))) {
                        if (sim_paused_) {
                            sim_paused_ = false;
                            store_.SetHistoryFrozen(false);
                            emit simPauseChanged(false);
                            emit plotTimeFreezeChanged(false, last_sim_time_sec_);
                        }
                    } else if (line.contains(QStringLiteral("HostSim: speed = turbo"))) {
                        sim_speed_turbo_ = true;
                        sim_speed_factor_ = 0.0;
                        emit simSpeedChanged(0.0);
                    } else if (line.contains(QStringLiteral("HostSim: speed ="))) {
                        const int eq = line.indexOf('=');
                        if (eq >= 0) {
                            QString num = line.mid(eq + 1).trimmed();
                            num.chop(1);
                            bool ok = false;
                            const double factor = num.toDouble(&ok);
                            if (ok && factor > 0.0) {
                                sim_speed_turbo_ = false;
                                sim_speed_factor_ = factor;
                                emit simSpeedChanged(factor);
                            }
                        }
                    }
                } else if constexpr (std::is_same_v<T, StatsItem>) {
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

    MaybeEmitStoreChanged();
}

void RuntimeController::TickSimulator() {
    if (sim_paused_) {
        return;
    }
    ++simTick_;
    const float t = NowSec();

    static std::mt19937 rng{42};
    std::uniform_real_distribution<float> noise(-0.05f, 0.05f);

    for (std::size_t i = 0; i < std::size(kSimWaves); ++i) {
        const auto& w = kSimWaves[i];
        const double phase = 2.0 * M_PI * w.freq * t + i * 1.1;
        const float value = static_cast<float>(w.offset + w.amplitude * std::sin(phase))
                            + noise(rng);
        PushF32(w.name, value, t);
    }

    if (simTick_ % 100 == 0) {
        Push(ConsoleItem{"sim: tick " + std::to_string(simTick_)});
    }

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
