#include "IvpTcpClient.h"

#include <cstdio>

namespace NodeGUI::runtime {

IvpTcpClient::IvpTcpClient(QObject* parent)
    : QObject(parent) {
    decoder_.onF32Value = [this](uint16_t id, const std::string& key, float value, uint32_t time_us) {
        if (onF32Value) onF32Value(id, key, value, time_us);
    };
    decoder_.onStringValue = [this](uint16_t id, const std::string& key,
                                    const std::string& value, uint32_t time_us) {
        if (onStringValue) onStringValue(id, key, value, time_us);
    };
    decoder_.onConsoleLine = [this](const std::string& line) {
        if (onConsoleLine) onConsoleLine(line);
    };

    reconnectTimer_.setSingleShot(true);
    statsTimer_.setInterval(1000);  // ~1 Hz, ticks even while the link is idle.

    connect(&socket_, &QTcpSocket::connected, this, &IvpTcpClient::OnConnected);
    connect(&socket_, &QTcpSocket::readyRead, this, &IvpTcpClient::OnReadyRead);
    connect(&socket_, &QTcpSocket::disconnected, this, &IvpTcpClient::OnDisconnected);
    // Failed reconnect attempts (connection refused) do not re-emit
    // disconnected, so schedule the next retry from errorOccurred too.
    connect(&socket_, &QAbstractSocket::errorOccurred, this,
            &IvpTcpClient::OnSocketError);
    connect(&reconnectTimer_, &QTimer::timeout, this, &IvpTcpClient::ConnectNow);
    connect(&statsTimer_, &QTimer::timeout, this, &IvpTcpClient::OnStatsTick);
}

IvpTcpClient::~IvpTcpClient() {
    Stop();
}

void IvpTcpClient::Start(const QString& host, int port) {
    Stop();
    host_ = host;
    port_ = port;
    running_ = true;
    announcedFirstFrame_ = false;
    // A previous endpoint's buffered bytes and id→key registry must not leak
    // into a fresh link; the peer re-announces its DEFINEs after accept.
    decoder_.Reset();
    windowStart_ = std::chrono::steady_clock::now();
    std::fprintf(stderr,
                 "IvpTcpClient: connecting to %s:%d (COBS-framed InverterProtocol)\n",
                 qPrintable(host_), port_);
    ConnectNow();
}

void IvpTcpClient::Stop() {
    running_ = false;
    reconnectTimer_.stop();
    statsTimer_.stop();
    if (socket_.state() != QAbstractSocket::UnconnectedState) {
        // Prevent a reconnect triggered by our own teardown.
        socket_.abort();
    }
}

bool IvpTcpClient::SendLine(const std::string& line) {
    if (line.empty() || !IsConnected()) {
        return false;
    }
    std::string out = line;
    if (out.back() != '\n' && out.back() != '\r') {
        out.push_back('\n');
    }
    const qint64 wrote = socket_.write(out.data(), static_cast<qint64>(out.size()));
    // A queued write is success: flush() reports false whenever the kernel
    // buffer still holds bytes, which is normal back-pressure, not failure.
    return wrote == static_cast<qint64>(out.size());
}

bool IvpTcpClient::IsConnected() const {
    return socket_.state() == QAbstractSocket::ConnectedState;
}

QString IvpTcpClient::Endpoint() const {
    return QStringLiteral("%1:%2").arg(host_).arg(port_);
}

void IvpTcpClient::ConnectNow() {
    if (!running_ || IsConnected()) {
        return;
    }
    socket_.abort();
    socket_.connectToHost(host_, static_cast<quint16>(port_));
}

void IvpTcpClient::OnConnected() {
    connectionErrorLogged_ = false;
    std::fprintf(stderr, "IvpTcpClient: connected to %s\n", qPrintable(Endpoint()));
    // Same reset as Start(): a reconnect after a drop may have died mid-frame
    // and the peer restarts its DEFINE announcements for this new connection.
    decoder_.Reset();
    windowStart_ = std::chrono::steady_clock::now();
    statsTimer_.start();
}

void IvpTcpClient::OnReadyRead() {
    while (socket_.bytesAvailable() > 0) {
        const QByteArray chunk = socket_.read(kMaxReadChunk);
        if (chunk.isEmpty()) {
            break;
        }

        const uint64_t goodBefore = decoder_.Stats().good_frames;
        decoder_.FeedBytes(reinterpret_cast<const uint8_t*>(chunk.constData()),
                           static_cast<size_t>(chunk.size()));

        if (!announcedFirstFrame_ && decoder_.Stats().good_frames > goodBefore) {
            announcedFirstFrame_ = true;
            std::fprintf(stderr,
                         "IvpTcpClient: first telemetry frame decoded (good=%llu bad=%llu)\n",
                         static_cast<unsigned long long>(decoder_.Stats().good_frames),
                         static_cast<unsigned long long>(decoder_.Stats().bad_frames));
        }
    }
}

void IvpTcpClient::OnDisconnected() {
    statsTimer_.stop();
    ScheduleReconnect(QStringLiteral("disconnected"));
}

void IvpTcpClient::OnSocketError(QAbstractSocket::SocketError /*error*/) {
    // errorOccurred hits again for every refused retry; complain once per
    // outage (reset in OnConnected) and keep retrying regardless.
    if (!connectionErrorLogged_) {
        connectionErrorLogged_ = true;
        std::fprintf(stderr, "IvpTcpClient: connect to %s failed (%s), retrying\n",
                     qPrintable(Endpoint()), qPrintable(socket_.errorString()));
    }
    ScheduleReconnect();
}

void IvpTcpClient::ScheduleReconnect(const QString& reason) {
    if (!running_) {
        return;
    }
    if (!reason.isEmpty()) {
        std::fprintf(stderr,
                     "IvpTcpClient: %s (%s), retrying in %d ms\n",
                     qPrintable(reason), qPrintable(Endpoint()), kReconnectMs);
    }
    reconnectTimer_.start(kReconnectMs);
}

void IvpTcpClient::OnStatsTick() {
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - windowStart_).count();
    windowStart_ = now;

    decoder_.EmitStats(dt);  // refreshes rolling rx_hz/rx_bytes_per_sec
    if (onStats) {
        onStats(decoder_.Stats());
    }
}

}  // namespace NodeGUI::runtime
