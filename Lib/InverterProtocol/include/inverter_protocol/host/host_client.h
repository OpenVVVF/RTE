#pragma once

#include "inverter_protocol/host/tcp_transport.h"
#include "inverter_protocol/host/uart_transport.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ivp {

/* Statistics exposed by the client. */
struct ClientStats {
    uint64_t good_frames = 0;
    uint64_t bad_frames = 0;
    uint64_t reject_crc = 0;
    uint64_t reject_hdr = 0;
    uint64_t reject_len = 0;
    uint64_t reject_decode = 0;
    uint64_t rx_bytes = 0;
    float    rx_hz = 0.0f;
    float    rx_bytes_per_sec = 0.0f;
    uint32_t last_seq = 0;
};

/* Host-side client for the Inverter Protocol.
 *
 * Owns the UART transport and a worker thread. Incoming telemetry values
 * are delivered through callbacks on the worker thread; the caller is
 * responsible for any thread marshalling needed for UI updates.
 */
class InverterClient {
public:
    using F32Callback       = std::function<void(uint16_t id, const std::string& key, float value, uint32_t time_us)>;
    using StringCallback    = std::function<void(uint16_t id, const std::string& key, const std::string& value, uint32_t time_us)>;
    using ConsoleCallback   = std::function<void(const std::string& line)>;
    using CmdRspCallback    = std::function<void(uint8_t req_id, uint8_t status, const std::vector<float>& fargs)>;
    using StatsCallback     = std::function<void(const ClientStats& stats)>;

    InverterClient();
    ~InverterClient();

    InverterClient(const InverterClient&) = delete;
    InverterClient& operator=(const InverterClient&) = delete;

    bool start(const std::string& port, int baud = UartTransport::DEFAULT_BAUD);
    /* Connect over TCP using the same COBS-framed InverterProtocol as UART. */
    bool startTcp(const std::string& host, int port);
    void stop();
    bool isRunning() const;

    /* Backward-compatible text shell command. */
    bool sendCommandLine(const std::string& line);

    /* Binary command request. */
    bool sendCommand(uint8_t opcode, uint8_t req_id);
    bool sendCommand(uint8_t opcode, uint8_t req_id, float a);
    bool sendCommand(uint8_t opcode, uint8_t req_id, float a, float b);

    /* Register callbacks. Call before start(). */
    void onF32Value(F32Callback cb)       { cb_f32_ = std::move(cb); }
    void onStringValue(StringCallback cb) { cb_str_ = std::move(cb); }
    void onConsoleLine(ConsoleCallback cb){ cb_console_ = std::move(cb); }
    void onCommandResponse(CmdRspCallback cb) { cb_rsp_ = std::move(cb); }
    void onStats(StatsCallback cb)        { cb_stats_ = std::move(cb); }

    /* Thread-safe snapshot of counters. */
    ClientStats stats() const;

private:
    enum class LinkKind { Uart, Tcp };

    void threadMainUart(const std::string& port, int baud);
    void threadMainTcp(const std::string& host, int port);
    void pumpTransport();
    bool sendPacket(const uint8_t* packet, size_t len);
    void handleTelemetryData(const uint8_t* payload, uint16_t payload_len, uint32_t time_us);
    void handleCommandResponse(const uint8_t* payload, uint16_t payload_len);

    struct KeyDef {
        uint8_t     type = 0;
        std::string key;
    };

    LinkKind link_ = LinkKind::Uart;
    UartTransport uart_;
    TcpTransport tcp_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    mutable std::mutex reg_mtx_;
    std::unordered_map<uint16_t, KeyDef> registry_;

    mutable std::mutex stats_mtx_;
    ClientStats stats_;

    F32Callback     cb_f32_;
    StringCallback  cb_str_;
    ConsoleCallback cb_console_;
    CmdRspCallback  cb_rsp_;
    StatsCallback   cb_stats_;
};

} // namespace ivp
