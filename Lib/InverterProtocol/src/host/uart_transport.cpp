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

    uint8_t raw[RX_RAW_CAP];
    int n = port_.read(raw, static_cast<int>(RX_RAW_CAP));
    if (n <= 0) return 0;

    for (int i = 0; i < n; ++i) {
        uint8_t b = raw[i];

        if (b == 0x00) {
            if (frame_len_ == 0) continue; /* empty frame, keep going */

            uint8_t decoded[RX_FRAME_CAP];
            size_t dec_len = ivp_cobs_decode(frame_buf_, frame_len_, decoded, RX_FRAME_CAP);
            frame_len_ = 0;

            if (dec_len == 0) return -1;
            if (dec_len < IVP_HEADER_SIZE + 2) return -1;
            if (dec_len > cap) return -1;

            std::memcpy(out, decoded, dec_len);
            return static_cast<int>(dec_len);
        } else {
            if (frame_len_ < RX_FRAME_CAP) {
                frame_buf_[frame_len_++] = b;
            } else {
                frame_len_ = 0; /* oversize, drop and resync */
            }
        }
    }

    return 0;
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
