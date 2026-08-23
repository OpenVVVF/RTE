#include <RTEAutomation/Session.h>

#include <RTEAutomation/CachePaths.h>

#include <nlohmann/json.hpp>

#include <array>
#include <fstream>
#include <limits>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace RTEAutomation {
namespace {

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
void CloseSocket(Socket socket) { closesocket(socket); }

class WinsockScope {
public:
    WinsockScope() { valid_ = WSAStartup(MAKEWORD(2, 2), &data_) == 0; }
    ~WinsockScope() { if (valid_) WSACleanup(); }
    bool valid() const { return valid_; }
private:
    WSADATA data_{};
    bool valid_ = false;
};
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
void CloseSocket(Socket socket) { close(socket); }
#endif

void SetError(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}

bool SendAll(Socket socket, const std::string& value) {
    std::size_t sent = 0;
    while (sent < value.size()) {
#ifdef _WIN32
        const int chunk = send(socket, value.data() + sent,
                               static_cast<int>(value.size() - sent), 0);
#else
        const ssize_t chunk = send(socket, value.data() + sent,
                                   value.size() - sent,
#ifdef __APPLE__
                                   0);
#else
                                   MSG_NOSIGNAL);
#endif
#endif
        if (chunk <= 0) return false;
        sent += static_cast<std::size_t>(chunk);
    }
    return true;
}

}  // namespace

fs::path SessionDirectory() {
    return DefaultCacheRoot() / "sessions";
}

fs::path CurrentSessionPath() {
    return SessionDirectory() / "current.json";
}

bool WriteSessionDescriptor(const SessionDescriptor& descriptor, std::string* error) {
    std::error_code ec;
    fs::create_directories(SessionDirectory(), ec);
    if (ec) {
        SetError(error, "could not create session directory: " + ec.message());
        return false;
    }
#ifndef _WIN32
    chmod(SessionDirectory().c_str(), S_IRWXU);
#endif
    const fs::path temporary = CurrentSessionPath().string() + ".tmp";
    std::ofstream out(temporary, std::ios::trunc);
    if (!out) {
        SetError(error, "could not write session descriptor");
        return false;
    }
    out << json{{"version", descriptor.version}, {"app", descriptor.app},
                {"host", descriptor.host}, {"port", descriptor.port},
                {"token", descriptor.token}, {"pid", descriptor.pid}}.dump(2) << '\n';
    out.close();
#ifndef _WIN32
    chmod(temporary.c_str(), S_IRUSR | S_IWUSR);
#endif
    fs::rename(temporary, CurrentSessionPath(), ec);
    if (ec) {
        fs::remove(CurrentSessionPath(), ec);
        ec.clear();
        fs::rename(temporary, CurrentSessionPath(), ec);
    }
    if (ec) {
        SetError(error, "could not publish session descriptor: " + ec.message());
        return false;
    }
    return true;
}

void RemoveSessionDescriptor(std::uint64_t expectedPid) {
    if (expectedPid != 0) {
        std::string ignored;
        const auto current = DiscoverSession({}, &ignored);
        if (!current || current->pid != expectedPid) return;
    }
    std::error_code ec;
    fs::remove(CurrentSessionPath(), ec);
}

std::optional<SessionDescriptor> DiscoverSession(const fs::path& descriptorPath,
                                                  std::string* error) {
    const fs::path path = descriptorPath.empty() ? CurrentSessionPath() : descriptorPath;
    std::ifstream input(path);
    if (!input) {
        SetError(error, "RTE Studio is not running (session descriptor not found at "
                        + path.string() + ")");
        return std::nullopt;
    }
    try {
        const json value = json::parse(input);
        SessionDescriptor descriptor;
        descriptor.version = value.value("version", 0);
        descriptor.app = value.value("app", "");
        descriptor.host = value.value("host", "127.0.0.1");
        descriptor.port = value.value("port", 0);
        descriptor.token = value.value("token", "");
        descriptor.pid = value.value("pid", std::uint64_t{0});
        if (descriptor.version != 1 || descriptor.host != "127.0.0.1"
            || descriptor.port <= 0 || descriptor.port > 65535
            || descriptor.token.empty()) {
            SetError(error, "invalid RTE Studio session descriptor");
            return std::nullopt;
        }
        return descriptor;
    } catch (const std::exception& exception) {
        SetError(error, std::string("could not parse session descriptor: ") + exception.what());
        return std::nullopt;
    }
}

std::optional<json> RequestSession(const SessionDescriptor& descriptor,
                                   const std::string& method,
                                   const json& params,
                                   std::string* error) {
#ifdef _WIN32
    WinsockScope winsock;
    if (!winsock.valid()) {
        SetError(error, "could not initialize Windows sockets");
        return std::nullopt;
    }
#endif
    Socket socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandle == kInvalidSocket) {
        SetError(error, "could not create local session socket");
        return std::nullopt;
    }
#ifdef __APPLE__
    int noSigPipe = 1;
    setsockopt(socketHandle, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));
#endif

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(descriptor.port));
    if (inet_pton(AF_INET, descriptor.host.c_str(), &address.sin_addr) != 1
        || connect(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        CloseSocket(socketHandle);
        SetError(error, "could not connect to RTE Studio; its session may be stale");
        return std::nullopt;
    }

#ifdef _WIN32
    DWORD timeout = 5000;
    setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval timeout{5, 0};
    setsockopt(socketHandle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
    const std::string request = json{{"token", descriptor.token},
                                     {"method", method},
                                     {"params", params}}.dump() + "\n";
    if (!SendAll(socketHandle, request)) {
        CloseSocket(socketHandle);
        SetError(error, "could not send request to RTE Studio");
        return std::nullopt;
    }

    std::string response;
    std::array<char, 4096> buffer{};
    while (response.size() < 4 * 1024 * 1024) {
#ifdef _WIN32
        const int count = recv(socketHandle, buffer.data(), static_cast<int>(buffer.size()), 0);
#else
        const ssize_t count = recv(socketHandle, buffer.data(), buffer.size(), 0);
#endif
        if (count <= 0) break;
        response.append(buffer.data(), static_cast<std::size_t>(count));
        const auto newline = response.find('\n');
        if (newline != std::string::npos) {
            response.resize(newline);
            break;
        }
    }
    CloseSocket(socketHandle);
    if (response.empty()) {
        SetError(error, "RTE Studio closed the session without a response");
        return std::nullopt;
    }
    try {
        const json envelope = json::parse(response);
        if (!envelope.value("ok", false)) {
            SetError(error, envelope.value("error", "RTE Studio rejected the request"));
            return std::nullopt;
        }
        return envelope.value("result", json::object());
    } catch (const std::exception& exception) {
        SetError(error, std::string("invalid response from RTE Studio: ") + exception.what());
        return std::nullopt;
    }
}

}  // namespace RTEAutomation
