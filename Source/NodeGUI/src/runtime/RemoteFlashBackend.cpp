#include "RemoteFlashBackend.h"

#include <QFile>
#include <QString>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>

#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>

namespace NodeGUI::runtime {

namespace {

struct HttpResult {
    int code = -1;  // -1 = transport failure
    std::string body;
};

// Minimal blocking HTTP/1.0 client (loopback/LAN, small bodies except the
// firmware upload which streams from memory).
HttpResult httpRequest(const std::string& host,
                       int port,
                       const std::string& method,
                       const std::string& path,
                       const std::string& body,
                       const std::string& contentType,
                       int timeoutMs) {
    HttpResult result;

    char portStr[16];
    std::snprintf(portStr, sizeof(portStr), "%d", port);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    if (getaddrinfo(host.c_str(), portStr, &hints, &addresses) != 0) {
        return result;
    }

    int fd = -1;
    for (addrinfo* ai = addresses; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;

        // Non-blocking connect with timeout: connect() to an unreachable
        // remote host blocks for minutes by default.
        const int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            pollfd pfd{fd, POLLOUT, 0};
            const int timeoutMs = 1000;
            if (::poll(&pfd, 1, timeoutMs) == 1 && (pfd.revents & POLLOUT)) {
                int err = 0;
                socklen_t len = sizeof(err);
                ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                rc = (err == 0) ? 0 : -1;
            } else {
                rc = -1;
            }
        }
        fcntl(fd, F_SETFL, flags);

        if (rc == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd < 0) {
        return result;
    }

    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    std::ostringstream request;
    request << method << " " << path << " HTTP/1.0\r\n"
            << "Host: " << host << "\r\n"
            << "Connection: close\r\n";
    if (!body.empty()) {
        request << "Content-Type: " << contentType << "\r\n"
                << "Content-Length: " << body.size() << "\r\n";
    }
    request << "\r\n";
    request << body;
    const std::string wire = request.str();

    size_t sent = 0;
    while (sent < wire.size()) {
        const ssize_t n = ::send(fd, wire.data() + sent, wire.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            ::close(fd);
            return result;
        }
        sent += static_cast<size_t>(n);
    }

    std::string response;
    char buf[8192];
    while (true) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        response.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);

    // "HTTP/1.1 202 Accepted\r\n..." -> 202
    const size_t sp = response.find(' ');
    if (sp == std::string::npos) {
        return result;
    }
    result.code = std::atoi(response.c_str() + sp + 1);
    const size_t bodyStart = response.find("\r\n\r\n");
    if (bodyStart != std::string::npos) {
        result.body = response.substr(bodyStart + 4);
    }
    return result;
}

bool decodeFlashStatus(const std::string& body, FlashBackendStatus& status) {
    const nlohmann::json json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded() || !json.is_object()) return false;

    const auto state = json.find("state");
    const auto busy = json.find("busy");
    const auto lastError = json.find("last_error");
    const auto progress = json.find("progress");
    const auto log = json.find("log");

    if (state == json.end() || !state->is_string()
        || busy == json.end() || !busy->is_boolean()
        || lastError == json.end() || !lastError->is_string()
        || progress == json.end() || !progress->is_number_integer()
        || log == json.end() || !log->is_array()) {
        return false;
    }

    FlashBackendStatus decoded;
    decoded.state = state->get<std::string>();
    decoded.busy = busy->get<bool>();
    decoded.lastError = lastError->get<std::string>();
    decoded.progress = progress->get<int>();
    decoded.reachable = true;
    decoded.log.reserve(log->size());
    for (const auto& line : *log) {
        if (line.is_string()) decoded.log.push_back(line.get<std::string>());
    }
    status = std::move(decoded);
    return true;
}

std::string decodeError(const std::string& body) {
    const nlohmann::json json = nlohmann::json::parse(body, nullptr, false);
    if (json.is_discarded() || !json.is_object()) return {};
    const auto error = json.find("error");
    return error != json.end() && error->is_string()
               ? error->get<std::string>()
               : std::string{};
}

}  // namespace

RemoteFlashBackend::RemoteFlashBackend(QString host, int httpPort)
    : host_(std::move(host))
    , httpPort_(httpPort) {
    pollThread_ = std::thread(&RemoteFlashBackend::PollLoop, this);
}

RemoteFlashBackend::~RemoteFlashBackend() {
    run_.store(false);
    if (pollThread_.joinable()) {
        pollThread_.join();
    }
}

void RemoteFlashBackend::SetServer(const QString& host, int httpPort) {
    std::lock_guard lk(mtx_);
    host_ = host;
    httpPort_ = httpPort;
    status_ = FlashBackendStatus{};
}

QString RemoteFlashBackend::Host() const {
    std::lock_guard lk(mtx_);
    return host_;
}

int RemoteFlashBackend::HttpPort() const {
    std::lock_guard lk(mtx_);
    return httpPort_;
}

bool RemoteFlashBackend::QueueFlash(const std::string& firmwarePath, bool autoGpio) {
    std::ifstream file(firmwarePath, std::ios::binary);
    if (!file) {
        std::lock_guard lk(mtx_);
        queueError_ = "cannot read firmware file: " + firmwarePath;
        return false;
    }
    const std::string bytes((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

    QString host;
    int port;
    {
        std::lock_guard lk(mtx_);
        host = host_;
        port = httpPort_;
    }

    const HttpResult result =
        httpRequest(host.toStdString(), port, "POST",
                    autoGpio ? "/flash" : "/flash?auto_gpio=0",
                    bytes, "application/octet-stream", 30000);
    if (result.code != 202) {
        std::lock_guard lk(mtx_);
        queueError_ = result.code < 0
                          ? "flash server unreachable"
                          : "flash rejected (HTTP " + std::to_string(result.code) + "): "
                                + decodeError(result.body);
        status_.lastError = queueError_;
        return false;
    }
    {
        std::lock_guard lk(mtx_);
        queueError_.clear();
    }
    return true;
}

FlashBackendStatus RemoteFlashBackend::Status() {
    std::lock_guard lk(mtx_);
    FlashBackendStatus copy = status_;
    if (!queueError_.empty()) {
        copy.lastError = queueError_;
    }
    return copy;
}

void RemoteFlashBackend::PollLoop() {
    while (run_.load()) {
        QString host;
        int port;
        {
            std::lock_guard lk(mtx_);
            host = host_;
            port = httpPort_;
        }

        const HttpResult result =
            httpRequest(host.toStdString(), port, "GET", "/flash/status", {}, {}, 3000);

        FlashBackendStatus decoded;
        const bool validStatus = result.code == 200
                                 && decodeFlashStatus(result.body, decoded);

        // IMPORTANT: keep the lock scope tight. Holding mtx_ across the
        // inter-poll sleep starves FlashPanel::PollStatus on the GUI thread,
        // which freezes the whole application.
        {
            std::lock_guard lk(mtx_);
            if (validStatus) {
                status_ = std::move(decoded);
            } else {
                status_.reachable = false;
                status_.busy = false;
                status_.state = "Unreachable";
            }
            queueError_.clear();
        }

        for (int i = 0; i < 5 && run_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

}  // namespace NodeGUI::runtime
