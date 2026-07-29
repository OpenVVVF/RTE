#pragma once

#include <cstdint>
#include <cstddef>
#include <deque>
#include <string>
#include <vector>

namespace ivp {

/* TCP transport with the same COBS + 0x00 framing as UartTransport.
 * Used so HostSim can publish InverterProtocol over localhost TCP and
 * NodeGUI can connect without a physical serial port. */
class TcpTransport {
public:
    static constexpr size_t RX_FRAME_CAP = 4096;
    static constexpr size_t RX_RAW_CAP = 512;

    TcpTransport();
    ~TcpTransport();

    TcpTransport(const TcpTransport&) = delete;
    TcpTransport& operator=(const TcpTransport&) = delete;

    /* Connect to host:port (e.g. "127.0.0.1", 14608). */
    bool open(const std::string& host, int port);
    void close();
    bool isOpen() const;

    bool sendPacket(const uint8_t* packet, size_t len);
    int receivePacket(uint8_t* out, size_t cap);
    bool sendLine(const std::string& line);

private:
    struct Impl;
    Impl* impl_;
    uint8_t frame_buf_[RX_FRAME_CAP];
    size_t frame_len_ = 0;
    std::deque<std::vector<uint8_t>> rx_queue_;

    void feedBytes(const uint8_t* data, int n);
    int readRaw(uint8_t* buf, int cap);
    bool writeRaw(const uint8_t* data, int n);
};

} // namespace ivp
