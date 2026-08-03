#include "SerialBridge.h"

#include <inverter_protocol/protocol.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace RTEServer {

namespace {

void setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

}  // namespace

SerialBridge::~SerialBridge() {
    stop();
}

bool SerialBridge::start(const std::string& serialPort, int tcpPort) {
    stop();
    serialPort_ = serialPort;
    tcpPort_ = tcpPort;

    {
        std::lock_guard lk(mtx_);
        if (!openSerialLocked()) {
            return false;
        }

        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ < 0) {
            closeSerialLocked();
            return false;
        }
        int opt = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(static_cast<uint16_t>(tcpPort));
        if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0
            || ::listen(listenFd_, 8) != 0) {
            ::close(listenFd_);
            listenFd_ = -1;
            closeSerialLocked();
            return false;
        }
        setNonBlocking(listenFd_);
    }

    run_.store(true);
    thread_ = std::thread(&SerialBridge::threadMain, this);
    return true;
}

void SerialBridge::stop() {
    run_.store(false);
    if (thread_.joinable()) {
        thread_.join();
    }
    std::lock_guard lk(mtx_);
    for (int fd : clients_) {
        ::close(fd);
    }
    clients_.clear();
    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
    closeSerialLocked();
}

void SerialBridge::suspend() {
    suspended_.store(true);
    std::lock_guard lk(mtx_);
    closeSerialLocked();
}

void SerialBridge::resume() {
    {
        std::lock_guard lk(mtx_);
        openSerialLocked();
    }
    suspended_.store(false);
}

int SerialBridge::clientCount() const {
    std::lock_guard lk(mtx_);
    return static_cast<int>(clients_.size());
}

bool SerialBridge::openSerialLocked() {
    // ivp::SerialPort ignores the baud argument (fixed 460800), which matches
    // the inverter link.
    return serial_.open(serialPort_, 460800);
}

void SerialBridge::closeSerialLocked() {
    serial_.close();
}

void SerialBridge::ScanForDefinesLocked(const uint8_t* data, int n) {
    constexpr size_t kFrameCap = 8192;
    constexpr size_t kCacheCap = 32768;

    for (int i = 0; i < n; ++i) {
        const uint8_t b = data[i];
        if (b != 0x00) {
            if (pending_.size() < kFrameCap) {
                pending_.push_back(b);
            } else {
                pending_.clear();  // oversize: resync
            }
            continue;
        }

        if (pending_.empty()) {
            continue;  // bare delimiter
        }

        uint8_t decoded[4096];
        const size_t len = ivp_cobs_decode(pending_.data(), pending_.size(),
                                           decoded, sizeof(decoded));
        // Keep an encoded copy (with delimiter) for replay regardless.
        if (len >= IVP_HEADER_SIZE + 2) {
            ivp_header_t header;
            if (ivp_header_decode(decoded, &header) == IVP_OK
                && header.msg_type == IVP_MSG_TELEMETRY_DEFINE) {
                if (defineCache_.size() < kCacheCap) {
                    defineCache_.insert(defineCache_.end(), pending_.begin(), pending_.end());
                    defineCache_.push_back(0x00);
                }
            }
        }
        pending_.clear();
    }
}

void SerialBridge::ReplayDefinesLocked(int clientFd) {
    size_t sent = 0;
    while (sent < defineCache_.size()) {
        const ssize_t n = ::send(clientFd, defineCache_.data() + sent,
                                 defineCache_.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                continue;
            }
            return;  // slow or dead client; it will resync on the next boot set
        }
        sent += static_cast<size_t>(n);
    }
}

void SerialBridge::threadMain() {
    while (run_.load()) {
        std::vector<pollfd> fds;
        int serialIndex = -1;
        int listenIndex = -1;
        size_t clientBase = 0;

        {
            std::lock_guard lk(mtx_);

            if (serial_.isOpen() && !suspended_.load()) {
                serialIndex = static_cast<int>(fds.size());
                fds.push_back({serial_.nativeHandle(), POLLIN, 0});
            }
            if (listenFd_ >= 0) {
                listenIndex = static_cast<int>(fds.size());
                fds.push_back({listenFd_, POLLIN, 0});
            }
            clientBase = fds.size();
            for (int fd : clients_) {
                fds.push_back({fd, POLLIN, 0});
            }
        }

        const int ready = ::poll(fds.data(), fds.size(), 200);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) continue;

        // New TCP clients: register, then replay the cached DEFINE set so a
        // mid-stream join can decode data frames right away.
        if (listenIndex >= 0 && (fds[listenIndex].revents & POLLIN)) {
            while (true) {
                const int fd = ::accept(listenFd_, nullptr, nullptr);
                if (fd < 0) break;
                setNonBlocking(fd);
                std::lock_guard lk(mtx_);
                clients_.push_back(fd);
                ReplayDefinesLocked(fd);
            }
        }

        // Serial -> all clients.
        if (serialIndex >= 0 && (fds[serialIndex].revents & POLLIN)) {
            uint8_t buf[4096];
            int n = 0;
            {
                std::lock_guard lk(mtx_);
                n = serial_.read(buf, static_cast<int>(sizeof(buf)));
                if (n > 0) {
                    ScanForDefinesLocked(buf, n);
                }
            }
            if (n > 0) {
                std::lock_guard lk(mtx_);
                for (size_t i = 0; i < clients_.size();) {
                    const ssize_t sent =
                        ::send(clients_[i], buf, static_cast<size_t>(n), MSG_NOSIGNAL);
                    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                        ::close(clients_[i]);
                        clients_.erase(clients_.begin() + static_cast<long>(i));
                    } else {
                        ++i;
                    }
                }
            }
        }

        // Clients -> serial; dead clients are swept afterwards.
        std::vector<int> dead;
        for (size_t i = clientBase; i < fds.size(); ++i) {
            const short revents = fds[i].revents;
            if (revents & POLLIN) {
                uint8_t buf[4096];
                const ssize_t n = ::recv(fds[i].fd, buf, sizeof(buf), 0);
                if (n <= 0) {
                    dead.push_back(fds[i].fd);
                    continue;
                }
                if (!suspended_.load()) {
                    std::lock_guard lk(mtx_);
                    if (serial_.isOpen()) {
                        serial_.write(buf, static_cast<int>(n));
                    }
                }
            }
            if (revents & (POLLERR | POLLHUP)) {
                dead.push_back(fds[i].fd);
            }
        }
        if (!dead.empty()) {
            std::lock_guard lk(mtx_);
            for (int fd : dead) {
                ::close(fd);
                clients_.erase(std::remove(clients_.begin(), clients_.end(), fd),
                               clients_.end());
            }
        }
    }
}

}  // namespace RTEServer
