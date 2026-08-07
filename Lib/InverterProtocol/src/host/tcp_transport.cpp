#include "inverter_protocol/host/tcp_transport.h"

#include "inverter_protocol/protocol.h"

#include <cstring>
#include <chrono>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ivp {
namespace {

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalid = INVALID_SOCKET;
inline void CloseSock(Socket s) {
    if (s != kInvalid) closesocket(s);
}
inline bool WouldBlock() {
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINTR;
}
struct WinsockInit {
    WinsockInit() {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    ~WinsockInit() { WSACleanup(); }
};
inline void EnsureWinsock() {
    static WinsockInit init;
    (void)init;
}
#else
using Socket = int;
constexpr Socket kInvalid = -1;
inline void CloseSock(Socket s) {
    if (s != kInvalid) ::close(s);
}
inline bool WouldBlock() {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}
inline void EnsureWinsock() {}
#endif

bool SetNonBlocking(Socket s) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

/* Upper bound for writeRaw() retrying on WouldBlock; a persistently
 * unwritable peer must fail the write instead of spinning forever. */
constexpr auto kWriteTimeout = std::chrono::seconds(2);

} // namespace

struct TcpTransport::Impl {
    Socket fd = kInvalid;
};

TcpTransport::TcpTransport() : impl_(new Impl()) {}
TcpTransport::~TcpTransport() {
    close();
    delete impl_;
}

bool TcpTransport::isOpen() const { return impl_->fd != kInvalid; }

void TcpTransport::close() {
    CloseSock(impl_->fd);
    impl_->fd = kInvalid;
    frame_len_ = 0;
    rx_queue_.clear();
}

bool TcpTransport::open(const std::string& host, int port) {
    EnsureWinsock();
    close();

    /* Resolve both IPv4 literals and hostnames (e.g. "localhost"). */
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* res = nullptr;
    const std::string port_str = std::to_string(port);
    if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0)
        return false;

    Socket fd = kInvalid;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == kInvalid) continue;
        if (::connect(fd, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) break;
        CloseSock(fd);
        fd = kInvalid;
    }
    ::freeaddrinfo(res);
    if (fd == kInvalid) return false;

    if (!SetNonBlocking(fd)) {
        CloseSock(fd);
        return false;
    }

    impl_->fd = fd;
    frame_len_ = 0;
    rx_queue_.clear();
    return true;
}

int TcpTransport::readRaw(uint8_t* buf, int cap) {
    if (!isOpen() || !buf || cap <= 0) return 0;
#ifdef _WIN32
    const int n = ::recv(impl_->fd, reinterpret_cast<char*>(buf), cap, 0);
    if (n == SOCKET_ERROR) return WouldBlock() ? 0 : -1;
    return n;
#else
    const int n = static_cast<int>(::recv(impl_->fd, buf, static_cast<size_t>(cap), 0));
    if (n < 0) return WouldBlock() ? 0 : -1;
    return n;
#endif
}

bool TcpTransport::writeRaw(const uint8_t* data, int n) {
    if (!isOpen() || !data || n <= 0) return false;
    const auto deadline = std::chrono::steady_clock::now() + kWriteTimeout;
    int total = 0;
    while (total < n) {
#ifdef _WIN32
        const int wrote = ::send(impl_->fd, reinterpret_cast<const char*>(data + total),
                                 n - total, 0);
        const bool failed = (wrote == SOCKET_ERROR);
#else
        const int wrote = static_cast<int>(
            ::send(impl_->fd, data + total, static_cast<size_t>(n - total), 0));
        const bool failed = (wrote < 0);
#endif
        if (failed) {
            if (WouldBlock()) {
                if (std::chrono::steady_clock::now() >= deadline) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            return false;
        }
        if (wrote == 0) return false;
        total += wrote;
    }
    return true;
}

bool TcpTransport::sendPacket(const uint8_t* packet, size_t len) {
    if (!packet || len == 0) return false;
    const size_t enc_cap = len + (len / 254) + 2;
    uint8_t* encoded = new uint8_t[enc_cap];
    const size_t enc_len = ivp_cobs_encode(packet, len, encoded, enc_cap);
    if (enc_len == 0) {
        delete[] encoded;
        return false;
    }
    encoded[enc_len] = 0;
    const bool ok = writeRaw(encoded, static_cast<int>(enc_len + 1));
    delete[] encoded;
    return ok;
}

void TcpTransport::feedBytes(const uint8_t* data, int n) {
    if (!data || n <= 0) {
        return;
    }

    for (int i = 0; i < n; ++i) {
        const uint8_t b = data[i];
        if (b == 0x00) {
            if (frame_len_ == 0) {
                continue;
            }

            std::vector<uint8_t> decoded(RX_FRAME_CAP);
            const size_t dec_len =
                ivp_cobs_decode(frame_buf_, frame_len_, decoded.data(), RX_FRAME_CAP);
            frame_len_ = 0;
            if (dec_len == 0 || dec_len < IVP_HEADER_SIZE + 2) {
                continue;
            }
            decoded.resize(dec_len);
            rx_queue_.push_back(std::move(decoded));
            continue;
        }

        if (frame_len_ < RX_FRAME_CAP) {
            frame_buf_[frame_len_++] = b;
        } else {
            frame_len_ = 0;
        }
    }
}

int TcpTransport::receivePacket(uint8_t* out, size_t cap) {
    if (!out || cap == 0) return -1;

    while (rx_queue_.empty()) {
        uint8_t raw[RX_RAW_CAP];
        const int n = readRaw(raw, static_cast<int>(RX_RAW_CAP));
        if (n < 0) return -1;
        if (n == 0) return 0;
        feedBytes(raw, n);
    }

    const auto& front = rx_queue_.front();
    if (front.size() > cap) {
        rx_queue_.pop_front();
        return -1;
    }

    std::memcpy(out, front.data(), front.size());
    const int len = static_cast<int>(front.size());
    rx_queue_.pop_front();
    return len;
}

bool TcpTransport::sendLine(const std::string& line) {
    std::string out = line;
    if (out.empty()) return false;
    if (out.back() != '\n' && out.back() != '\r') out.push_back('\n');
    return writeRaw(reinterpret_cast<const uint8_t*>(out.data()),
                    static_cast<int>(out.size()));
}

} // namespace ivp
