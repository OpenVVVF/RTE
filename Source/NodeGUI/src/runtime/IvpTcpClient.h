#pragma once

#include "IvpStreamDecoder.h"

#include <QObject>
#include <QString>
#include <QTcpSocket>
#include <QTimer>

#include <chrono>
#include <string>

namespace NodeGUI::runtime {

// Async TCP client for HostSim --live (COBS-framed InverterProtocol on
// 127.0.0.1:14608 by default). Runs entirely on the GUI thread: QTcpSocket
// signals feed an IvpStreamDecoder, whose callbacks are forwarded through the
// public std::function slots. While running, the client retries the
// connection 2 s after any disconnect or failed attempt. Text shell commands
// (e.g. HostSim's "throttle a 0.5", "duty u 60", "pause") go out with
// SendLine().
class IvpTcpClient : public QObject {
    Q_OBJECT

public:
    explicit IvpTcpClient(QObject* parent = nullptr);
    ~IvpTcpClient() override;

    void Start(const QString& host, int port);
    void Stop();

    // Text shell command channel (appends '\n' when missing). Returns false
    // when not connected or the write fails.
    bool SendLine(const std::string& line);

    bool IsConnected() const;
    QString Endpoint() const;

    // Same callback shapes as ivp::InverterClient; all fire on the GUI
    // thread. Register before Start().
    IvpStreamDecoder::F32Callback onF32Value;
    IvpStreamDecoder::StringCallback onStringValue;
    IvpStreamDecoder::ConsoleCallback onConsoleLine;
    IvpStreamDecoder::StatsCallback onStats;

private slots:
    void ConnectNow();
    void OnConnected();
    void OnReadyRead();
    void OnDisconnected();
    void OnSocketError(QAbstractSocket::SocketError error);
    void OnStatsTick();

private:
    static constexpr int kReconnectMs = 2000;
    static constexpr int kMaxReadChunk = 64 * 1024;

    void ScheduleReconnect(const QString& reason = {});

    QTcpSocket socket_;
    QTimer reconnectTimer_;
    QTimer statsTimer_;
    QString host_;
    int port_ = 0;
    bool running_ = false;
    bool announcedFirstFrame_ = false;
    bool connectionErrorLogged_ = false;
    IvpStreamDecoder decoder_;
    std::chrono::steady_clock::time_point windowStart_;
};

}  // namespace NodeGUI::runtime
