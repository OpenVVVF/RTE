#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace ivp {

/* Cross-platform serial port wrapper. Supports POSIX and Windows. */
class SerialPort {
public:
    SerialPort();
    ~SerialPort();

    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    bool open(const std::string& port, int baud = 460800);
    void close();
    bool isOpen() const;

    /* Read up to `cap` bytes. Returns bytes read (>=0). */
    int read(uint8_t* buf, int cap);

    /* Write exactly `n` bytes. Returns true on full success. */
    bool write(const uint8_t* data, int n);

    /* Drain the transmit buffer. */
    bool drain();

private:
    struct Impl;
    Impl* impl_;
};

/* UART transport adapter for the Inverter Protocol.
 *
 * Encapsulates COBS framing with 0x00 delimiters around ivp_packet_encode
 * output. The receive side accumulates raw bytes and emits complete,
 * CRC-verified packets.
 */
class UartTransport {
public:
    static constexpr int DEFAULT_BAUD = 460800;
    static constexpr size_t RX_FRAME_CAP = 4096;
    static constexpr size_t RX_RAW_CAP = 512;

    UartTransport();
    ~UartTransport();

    UartTransport(const UartTransport&) = delete;
    UartTransport& operator=(const UartTransport&) = delete;

    bool open(const std::string& port, int baud = DEFAULT_BAUD);
    void close();
    bool isOpen() const;

    /* Send a complete packet buffer. COBS-encodes and appends 0x00. */
    bool sendPacket(const uint8_t* packet, size_t len);

    /* Try to receive one complete packet into `out`.
     * Returns packet length on success, 0 if no complete packet is available,
     * or -1 on a framing/CRC error (caller should resync). */
    int receivePacket(uint8_t* out, size_t cap);

    /* Send a text command line followed by \n. Used by the text shell. */
    bool sendLine(const std::string& line);

private:
    SerialPort port_;
    uint8_t frame_buf_[RX_FRAME_CAP];
    size_t frame_len_ = 0;
};

} // namespace ivp
