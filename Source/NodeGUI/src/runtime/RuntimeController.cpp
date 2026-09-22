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

std::string ConsoleSafe(const std::string& value) {
    std::string safe;
    safe.reserve(value.size());
    for (const unsigned char ch : value) {
        if (ch == '\n') safe += "\\n";
        else if (ch == '\r') safe += "\\r";
        else if (ch == '\t') safe += "\\t";
        else if (ch < 0x20 || ch == 0x7f) safe += '?';
        else safe += static_cast<char>(ch);
    }
    return safe;
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
        Push(QueuedF32{key, value, tsec});
    };
    legacyClient_.onString = [this](const std::string& key, const std::string& value) {
        Push(QueuedString{key, value});
    };
    legacyClient_.onConsole = [this](const std::string& line) { Push(QueuedConsole{line}); };
    legacyClient_.onStats = [this](const LegacyTelemetryClient::Stats& s) {
        Push(QueuedStats{s.rxHz,
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
        Push(QueuedF32{key, value, NowSec()});
    });
    ivpClient_.onStringValue([this](uint16_t, const std::string& key, const std::string& value,
                                    uint32_t) { Push(QueuedString{key, value}); });
    ivpClient_.onConsoleLine([this](const std::string& line) { Push(QueuedConsole{line}); });
    ivpClient_.onStats([this](const ivp::ClientStats& s) {
        Push(QueuedStats{s.rx_hz,
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

    tcpClient_.onF32Value = [this](uint16_t, const std::string& key, float value, uint32_t) {
        Push(QueuedF32{key, value, NowSec()});
    };
    tcpClient_.onStringValue = [this](uint16_t, const std::string& key, const std::string& value,
                                      uint32_t) { Push(QueuedString{key, value}); };
    tcpClient_.onConsoleLine = [this](const std::string& line) { Push(QueuedConsole{line}); };
    tcpClient_.onStats = [this](const ivp::ClientStats& s) {
        Push(QueuedStats{s.rx_hz,
                       s.rx_bytes_per_sec,
                       s.good_frames,
                       s.bad_frames,
                       s.reject_crc,
                       s.reject_hdr,
                       s.reject_len,
                       s.reject_decode,
                       0,
                       s.last_seq});
    };
}

RuntimeController::~RuntimeController() {
    legacyClient_.stop();
    ivpClient_.stop();
    tcpClient_.Stop();
}

void RuntimeController::Start() {
    StartActiveLink();

    drainTimer_ = new QTimer(this);
    drainTimer_->setInterval(33);  // ~30 Hz GUI updates
    connect(drainTimer_, &QTimer::timeout, this, &RuntimeController::DrainQueue);
    drainTimer_->start();
}

void RuntimeController::StartActiveLink() {
    if (linkOverride_) {
        tcpClient_.Start(overrideHost_, overridePort_);
        return;
    }
    if (simulate_) {
        if (!simTimer_) {
            simTimer_ = new QTimer(this);
            simTimer_->setInterval(10);  // 100 Hz
            connect(simTimer_, &QTimer::timeout, this, &RuntimeController::TickSimulator);
        }
        simTimer_->start();
        return;
    }
    if (UsingTcp()) {
        tcpClient_.Start(tcpHost_, tcpPort_);
    } else if (protocol_ == Protocol::Legacy) {
        legacyClient_.start(port_.toStdString());
    } else {
        ivpClient_.start(port_.toStdString());
    }
}

void RuntimeController::StopActiveLink() {
    if (simTimer_) {
        simTimer_->stop();
    }
    legacyClient_.stop();
    ivpClient_.stop();
    tcpClient_.Stop();
}

void RuntimeController::ConnectTcpOverride(const QString& host, int port) {
    QString normalized = host.trimmed();
    // A listener may announce a wildcard bind address; connect via loopback.
    if (normalized == QStringLiteral("0.0.0.0")) {
        normalized = QStringLiteral("127.0.0.1");
    } else if (normalized == QStringLiteral("::")) {
        normalized = QStringLiteral("::1");
    }
    if (linkOverride_ && overrideHost_ == normalized && overridePort_ == port) {
        return;  // already attached to exactly this endpoint
    }
    overrideHost_ = normalized;
    overridePort_ = port;
    linkOverride_ = true;
    if (suspended_) {
        return;
    }
    StopActiveLink();
    tcpClient_.Start(overrideHost_, overridePort_);
}

void RuntimeController::ClearLinkOverride() {
    if (!linkOverride_) {
        return;
    }
    linkOverride_ = false;
    StopActiveLink();
    overrideHost_.clear();
    overridePort_ = 0;
    if (!suspended_) {
        StartActiveLink();
    }
}

bool RuntimeController::IsTcpConnected() const {
    return tcpClient_.IsConnected();
}

void RuntimeController::SetPort(const QString& port) {
    const QString normalized = port.trimmed();
    if (normalized.isEmpty() || normalized == port_) {
        return;
    }

    // Only the serial link is re-targeted here; a TCP override or the
    // simulated feed keeps running untouched.
    const bool restartSerial = !simulate_ && !linkOverride_ && !suspended_;
    if (restartSerial) {
        StopActiveLink();
    }
    port_ = normalized;
    if (restartSerial) {
        StartActiveLink();
    }
}

bool RuntimeController::SendLine(const std::string& line) {
    if (suspended_ || (simulate_ && !linkOverride_)) {
        return false;
    }
    if (linkOverride_ || UsingTcp()) {
        return tcpClient_.SendLine(line);
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

bool RuntimeController::SendCommandRaw(const std::string& line,
                                       const std::string& source) {
    const std::string label = source == "mcp" ? "MCP" : source == "cli" ? "CLI" : "API";
    store_.AddConsoleLine("[" + label + "] > " + ConsoleSafe(line));
    const bool ok = SendLine(line);
    store_.AddCommand(line, source, ok);
    store_.AddConsoleLine("[" + label + "] " + (ok ? "sent" : "FAILED to send"));
    emit storeChanged();
    return ok;
}

void RuntimeController::AddAutomationLine(const std::string& line) {
    store_.AddConsoleLine(line);
    emit storeChanged();
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
    if (simulate_ && !linkOverride_) {
        return;
    }
    if (linkOverride_ || UsingTcp()) {
        tcpClient_.Stop();
    } else if (protocol_ == Protocol::Legacy) {
        legacyClient_.suspend();
    } else {
        ivpClient_.stop();
    }
}

void RuntimeController::ResumeAfterFlash() {
    if (!suspended_) {
        return;
    }
    suspended_ = false;
    store_.SetSuspended(false);
    if (simulate_ && !linkOverride_) {
        // The simulated feed keeps running across a flash suspend, so there
        // is normally nothing to restart. If it is down, a live-link override
        // was cleared while suspended: start whatever link is current rather
        // than leaving a dead feed.
        if (!simTimer_ || !simTimer_->isActive()) {
            StartActiveLink();
        }
        return;
    }
    if (linkOverride_ || UsingTcp()) {
        tcpClient_.Start(linkOverride_ ? overrideHost_ : tcpHost_,
                         linkOverride_ ? overridePort_ : tcpPort_);
    } else if (protocol_ == Protocol::Legacy) {
        legacyClient_.resume();
    } else {
        ivpClient_.start(port_.toStdString());
    }
}

void RuntimeController::Push(PendingItem item) {
    pending_.Push(std::move(item));
}

void RuntimeController::NoteQueueBacklog(uint64_t coalesced, uint64_t dropped) {
    if (coalesced == 0 && dropped == 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (lastQueueNotice_.time_since_epoch() != std::chrono::steady_clock::duration::zero()
        && now - lastQueueNotice_ < std::chrono::seconds(5)) {
        return;
    }
    lastQueueNotice_ = now;
    store_.AddConsoleLine(
        "runtime: GUI was busy; queue backlog resolved by coalescing "
        + std::to_string(coalesced) + " and dropping " + std::to_string(dropped)
        + " telemetry value(s) (latest values always win; console and stats are never dropped)");
}

void RuntimeController::DrainQueue() {
    uint64_t coalesced = 0;
    uint64_t dropped = 0;
    const std::vector<PendingItem> items = pending_.Drain(coalesced, dropped);
    if (items.empty()) {
        NoteQueueBacklog(coalesced, dropped);
        return;
    }

    for (const auto& item : items) {
        std::visit(
            [this](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, QueuedF32>) {
                    store_.AddF32(v.key, v.value, v.tsec);
                } else if constexpr (std::is_same_v<T, QueuedString>) {
                    store_.AddString(v.key, v.value);
                } else if constexpr (std::is_same_v<T, QueuedConsole>) {
                    store_.AddConsoleLine(v.text);
                    store_.MarkLastCommandReceived();
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

    NoteQueueBacklog(coalesced, dropped);
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
        Push(QueuedF32{w.name, value, t});
    }

    // Occasional console output so the console path is exercised.
    if (simTick_ % 100 == 0) {
        Push(QueuedConsole{"sim: tick " + std::to_string(simTick_)});
    }

    // Stats every second.
    if (simTick_ % 100 == 0) {
        Push(QueuedStats{100.0f,
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

QString RuntimeController::Port() const {
    if (linkOverride_) {
        return QStringLiteral("sim %1:%2").arg(overrideHost_).arg(overridePort_);
    }
    if (!UsingTcp()) {
        return port_;
    }
    return QStringLiteral("tcp %1:%2").arg(tcpHost_).arg(tcpPort_);
}

}  // namespace NodeGUI::runtime
