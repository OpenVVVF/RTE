#pragma once

// Serial-over-TCP bridge: owns the inverter's UART and exposes it to any
// number of TCP clients. Serial bytes are broadcast to every client; bytes a
// client sends are written to the UART. The wire format is the raw serial
// stream, so clients use the exact same parser as a local connection.
//
// Pure std C++20 + POSIX (poll(2), sockets, termios via ivp::SerialPort).

#include <inverter_protocol/host/uart_transport.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace RTEServer {

class SerialBridge {
public:
    SerialBridge() = default;
    ~SerialBridge();  // stops

    SerialBridge(const SerialBridge&) = delete;
    SerialBridge& operator=(const SerialBridge&) = delete;

    // Opens the UART and starts listening for TCP clients on tcpPort
    // (loopback + LAN). Returns false if the serial port cannot be opened.
    bool start(const std::string& serialPort, int tcpPort);
    void stop();

    // Releases the serial port so the firmware updater can use it, keeping
    // clients connected (their reads just go quiet). resume() reopens it.
    void suspend();
    void resume();
    bool isSuspended() const { return suspended_.load(); }

    int clientCount() const;

private:
    void threadMain();

    void closeSerialLocked();
    bool openSerialLocked();

    // Watches the serial stream for DEFINE frames (the signal-name registry)
    // and caches them, so clients that connect mid-stream can be brought up
    // to date: the firmware defines its signals once at boot, and without a
    // replay a late client can never decode data frames.
    void ScanForDefinesLocked(const uint8_t* data, int n);
    void ReplayDefinesLocked(int clientFd);

    std::string serialPort_;
    int tcpPort_ = 0;

    std::atomic<bool> run_{false};
    std::atomic<bool> suspended_{false};
    std::thread thread_;

    // All of these are guarded by mtx_ (the poll loop touches them too).
    mutable std::mutex mtx_;
    ivp::SerialPort serial_;
    int listenFd_ = -1;
    std::vector<int> clients_;

    // DEFINE replay state (mtx_): frame reassembly + cached replay bytes.
    std::vector<uint8_t> pending_;
    std::vector<uint8_t> defineCache_;
};

}  // namespace RTEServer
