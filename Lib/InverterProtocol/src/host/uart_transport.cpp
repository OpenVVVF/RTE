#include "inverter_protocol/host/uart_transport.h"

#include "inverter_protocol/protocol.h"

#include <cerrno>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#endif

namespace ivp {

/* ========================================================================
 * SerialPort implementation
 * ======================================================================== */

#ifdef _WIN32
struct SerialPort::Impl {
    HANDLE h = INVALID_HANDLE_VALUE;
};
#else
struct SerialPort::Impl {
    int fd = -1;
};
#endif

SerialPort::SerialPort() : impl_(new Impl()) {}
SerialPort::~SerialPort() { close(); delete impl_; }

bool SerialPort::isOpen() const {
#ifdef _WIN32
    return impl_->h != INVALID_HANDLE_VALUE;
#else
    return impl_->fd >= 0;
#endif
}

void SerialPort::close() {
#ifdef _WIN32
    if (impl_->h != INVALID_HANDLE_VALUE) {
        ::CloseHandle(impl_->h);
        impl_->h = INVALID_HANDLE_VALUE;
    }
#else
    if (impl_->fd >= 0) {
        ::close(impl_->fd);
        impl_->fd = -1;
    }
#endif
}

bool SerialPort::open(const std::string& port, int baud) {
    (void)baud; /* The existing device link runs at 460800. */
    close();

#ifdef _WIN32
    std::string full = "\\\\.\\" + port;
    HANDLE h = CreateFileA(full.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) { CloseHandle(h); return false; }

    dcb.BaudRate = 460800;
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    if (!SetCommState(h, &dcb)) { CloseHandle(h); return false; }

    COMMTIMEOUTS t{};
    t.ReadIntervalTimeout = 1;
    t.ReadTotalTimeoutConstant = 1;
    SetCommTimeouts(h, &t);

    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    impl_->h = h;
    return true;
#else
    int fd = ::open(port.c_str(), O_RDWR | O_NOCTTY);
    if (fd < 0) return false;

    termios tty{};
    if (tcgetattr(fd, &tty) != 0) { ::close(fd); return false; }

    cfsetispeed(&tty, B460800);
    cfsetospeed(&tty, B460800);
    cfmakeraw(&tty);

    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag &= ~HUPCL;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 1;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) { ::close(fd); return false; }

    tcflush(fd, TCIOFLUSH);
    impl_->fd = fd;
    return true;
#endif
}

int SerialPort::read(uint8_t* buf, int cap) {
    if (!isOpen() || !buf || cap <= 0) return 0;
#ifdef _WIN32
    DWORD got = 0;
    if (!ReadFile(impl_->h, buf, static_cast<DWORD>(cap), &got, nullptr)) return 0;
    return static_cast<int>(got);
#else
    int n = static_cast<int>(::read(impl_->fd, buf, static_cast<size_t>(cap)));
    return n > 0 ? n : 0;
#endif
}

bool SerialPort::write(const uint8_t* data, int n) {
    if (!isOpen() || !data || n <= 0) return false;
#ifdef _WIN32
    int total = 0;
    while (total < n) {
        DWORD wrote = 0;
        if (!WriteFile(impl_->h, data + total, static_cast<DWORD>(n - total), &wrote, nullptr))
            return false;
        if (wrote == 0) return false;
        total += static_cast<int>(wrote);
    }
    return true;
#else
    int total = 0;
    while (total < n) {
        int wrote = static_cast<int>(::write(impl_->fd, data + total, static_cast<size_t>(n - total)));
        if (wrote < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (wrote == 0) return false;
        total += wrote;
    }
    return true;
#endif
}

bool SerialPort::drain() {
    if (!isOpen()) return false;
#ifdef _WIN32
    return FlushFileBuffers(impl_->h) != 0;
#else
    return ::tcdrain(impl_->fd) == 0;
#endif
}

/* ========================================================================
 * UartTransport implementation
 * ======================================================================== */

UartTransport::UartTransport() = default;
UartTransport::~UartTransport() = default;

bool UartTransport::open(const std::string& port, int baud) {
    return port_.open(port, baud);
}

void UartTransport::close() { port_.close(); }
bool UartTransport::isOpen() const { return port_.isOpen(); }

bool UartTransport::sendPacket(const uint8_t* packet, size_t len) {
    if (!packet || len == 0) return false;

    /* Worst-case COBS expansion: len + ceil(len/254) + 1. */
    size_t enc_cap = len + (len / 254) + 2;
    uint8_t* encoded = new uint8_t[enc_cap];

    size_t enc_len = ivp_cobs_encode(packet, len, encoded, enc_cap);
    if (enc_len == 0) { delete[] encoded; return false; }

    encoded[enc_len] = 0;
    bool ok = port_.write(encoded, static_cast<int>(enc_len + 1));
    delete[] encoded;
    return ok;
}

int UartTransport::receivePacket(uint8_t* out, size_t cap) {
    if (!out || cap == 0) return -1;

    // rx_buf_ persists across calls: one read() chunk usually contains
    // several frames (or a partial one), and any bytes past the first
    // delimiter must be kept for the next call rather than dropped.
    int ready = extractFrame(out, cap);
    if (ready != 0) return ready;

    uint8_t raw[RX_RAW_CAP];
    int n = port_.read(raw, static_cast<int>(RX_RAW_CAP));
    if (n <= 0) return 0;

    if (rx_len_ + static_cast<size_t>(n) > sizeof(rx_buf_)) {
        // No delimiter within a whole buffer: garbage stream, resync.
        rx_len_ = 0;
        return -1;
    }
    std::memcpy(rx_buf_ + rx_len_, raw, static_cast<size_t>(n));
    rx_len_ += static_cast<size_t>(n);

    return extractFrame(out, cap);
}

int UartTransport::extractFrame(uint8_t* out, size_t cap) {
    // Skip leading delimiters (empty frames between packets).
    size_t skip = 0;
    while (skip < rx_len_ && rx_buf_[skip] == 0x00) ++skip;
    if (skip > 0) {
        std::memmove(rx_buf_, rx_buf_ + skip, rx_len_ - skip);
        rx_len_ -= skip;
    }
    if (rx_len_ == 0) return 0;

    for (size_t p = 0; p < rx_len_; ++p) {
        if (rx_buf_[p] != 0x00) continue;

        uint8_t decoded[RX_FRAME_CAP];
        const size_t dec_len = ivp_cobs_decode(rx_buf_, p, decoded, RX_FRAME_CAP);

        // Consume the frame bytes and the delimiter, keep the rest.
        std::memmove(rx_buf_, rx_buf_ + p + 1, rx_len_ - p - 1);
        rx_len_ -= p + 1;

        if (dec_len == 0) return -1;
        if (dec_len < IVP_HEADER_SIZE + 2) return -1;
        if (dec_len > cap) return -1;

        std::memcpy(out, decoded, dec_len);
        return static_cast<int>(dec_len);
    }

    return 0;  // no complete frame buffered yet
}

bool UartTransport::sendLine(const std::string& line) {
    std::string out = line;
    if (out.empty()) return false;
    if (out.back() != '\n' && out.back() != '\r') out.push_back('\n');
    if (!port_.write(reinterpret_cast<const uint8_t*>(out.data()), static_cast<int>(out.size())))
        return false;
    return port_.drain();
}

} // namespace ivp
