#include "telemetry_publisher.h"

#include "sim_runtime.h"

#include "inverter_protocol/packet_builder.h"
#include "inverter_protocol/protocol.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

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
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace hostsim {
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

bool WriteAll(Socket fd, const uint8_t* data, int n) {
    int total = 0;
    while (total < n) {
#ifdef _WIN32
        const int wrote =
            ::send(fd, reinterpret_cast<const char*>(data + total), n - total, 0);
        if (wrote == SOCKET_ERROR) {
            if (WouldBlock()) continue;
            return false;
        }
#else
        const int wrote =
            static_cast<int>(::send(fd, data + total, static_cast<size_t>(n - total), 0));
        if (wrote < 0) {
            if (WouldBlock()) continue;
            return false;
        }
#endif
        if (wrote == 0) return false;
        total += wrote;
    }
    return true;
}

} // namespace

struct TelemetryPublisher::Client {
    Socket fd = kInvalid;
    std::string rx;
    bool needs_define = true;
};

TelemetryPublisher::TelemetryPublisher() = default;

TelemetryPublisher::~TelemetryPublisher() { Stop(); }

TelemetryPublisher& GlobalTelemetryPublisher() {
    static TelemetryPublisher pub;
    return pub;
}

bool TelemetryPublisher::IsListening() const { return listen_fd_ != -1; }

bool TelemetryPublisher::HasClient() const { return !clients_.empty(); }

bool TelemetryPublisher::Start(const std::string& host, int port) {
    EnsureWinsock();
    Stop();
    host_ = host;
    port_ = port;

    Socket fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == kInvalid) return false;

    int yes = 1;
#ifdef _WIN32
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
#else
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        CloseSock(fd);
        return false;
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        CloseSock(fd);
        return false;
    }
    if (::listen(fd, 4) != 0) {
        CloseSock(fd);
        return false;
    }
    if (!SetNonBlocking(fd)) {
        CloseSock(fd);
        return false;
    }

    listen_fd_ = static_cast<int>(fd);
    EnsureBuiltinIds();
    quit_requested_ = false;
    std::printf("HostSim live: listening on %s:%d (NodeGUI: --tcp %s:%d --protocol ivp)\n",
                host.c_str(), port, host.c_str(), port);
    std::fflush(stdout);
    return true;
}

void TelemetryPublisher::Stop() {
    for (auto& c : clients_) CloseSock(c.fd);
    clients_.clear();
    if (listen_fd_ != -1) {
        CloseSock(static_cast<Socket>(listen_fd_));
        listen_fd_ = -1;
    }
}

void TelemetryPublisher::EnsureBuiltinIds() {
    std::lock_guard<std::mutex> lock(mu_);
    const char* keys[] = {
        "throttle_a", "throttle_b", "duty_u", "duty_v", "duty_w",
        "i_a", "i_b", "i_c", "theta_e", "omega_e", "vdc_v", "sim_speed",
        "pwm_gate_u", "pwm_gate_v", "pwm_gate_w",
        "pwm_v_u", "pwm_v_v", "pwm_v_w",
        "pwm_v_uv", "pwm_v_vw", "pwm_v_wu", "pwm_telem_hz",
    };
    for (const char* k : keys) {
        auto& s = signals_[k];
        if (s.id == 0) {
            s.id = next_id_++;
            s.defined = false;
            define_dirty_ = true;
        }
    }
}

void TelemetryPublisher::LogF32(const char* key, float value) {
    if (!key || !*key) return;
    std::lock_guard<std::mutex> lock(mu_);
    auto& s = signals_[key];
    if (s.id == 0) {
        s.id = next_id_++;
        s.defined = false;
        define_dirty_ = true;
    }
    s.value = value;
}

void TelemetryPublisher::SetBuiltin(float throttle_a, float throttle_b, float duty_u,
                                    float duty_v, float duty_w, float i_a, float i_b,
                                    float i_c, float theta_e, float omega_e, float vdc) {
    LogF32("throttle_a", throttle_a);
    LogF32("throttle_b", throttle_b);
    LogF32("duty_u", duty_u);
    LogF32("duty_v", duty_v);
    LogF32("duty_w", duty_w);
    LogF32("i_a", i_a);
    LogF32("i_b", i_b);
    LogF32("i_c", i_c);
    LogF32("theta_e", theta_e);
    LogF32("omega_e", omega_e);
    LogF32("vdc_v", vdc);
}

bool TelemetryPublisher::AcceptPending() {
    if (listen_fd_ == -1) return false;
    sockaddr_in peer{};
#ifdef _WIN32
    int plen = sizeof(peer);
#else
    socklen_t plen = sizeof(peer);
#endif
    Socket cfd = ::accept(static_cast<Socket>(listen_fd_),
                          reinterpret_cast<sockaddr*>(&peer), &plen);
    if (cfd == kInvalid) return false;
    SetNonBlocking(cfd);
    Client c;
    c.fd = cfd;
    c.needs_define = true;
    clients_.push_back(std::move(c));
    define_dirty_ = true;
    std::printf("HostSim live: client connected (%zu total)\n", clients_.size());
    return true;
}

void TelemetryPublisher::DropClient(size_t index) {
    if (index >= clients_.size()) return;
    CloseSock(clients_[index].fd);
    clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(index));
    std::printf("HostSim live: client disconnected (%zu remaining)\n", clients_.size());
}

bool TelemetryPublisher::SendFramed(Client& c, const uint8_t* packet, size_t len) {
    const size_t enc_cap = len + (len / 254) + 2;
    std::vector<uint8_t> encoded(enc_cap);
    const size_t enc_len = ivp_cobs_encode(packet, len, encoded.data(), enc_cap);
    if (enc_len == 0) return false;
    encoded[enc_len] = 0;
    return WriteAll(c.fd, encoded.data(), static_cast<int>(enc_len + 1));
}

bool TelemetryPublisher::SendDefine(Client& c, uint32_t time_us) {
    // Snapshot the key set so the frames below can be built without holding
    // the lock across socket writes.
    std::vector<std::pair<uint16_t, std::string>> entries;
    {
        std::lock_guard<std::mutex> lock(mu_);
        entries.reserve(signals_.size());
        for (auto& kv : signals_) {
            entries.emplace_back(kv.second.id, kv.first);
            kv.second.defined = true;
        }
        define_dirty_ = false;
    }

    // A full signal set does not fit in one 240-byte DEFINE payload, so page
    // through it. Stopping at the first frame would leave the tail keys
    // permanently unknown to the client, which then silently drops their DATA
    // items and shows them as zero forever.
    std::size_t index = 0;
    while (index < entries.size()) {
        uint8_t payload[IVP_DEFINE_PAYLOAD_MAX];
        ivp_define_builder_t b;
        if (ivp_telemetry_define_begin(&b, payload, sizeof(payload)) != IVP_OK) return false;

        const std::size_t frame_start = index;
        for (; index < entries.size(); ++index) {
            const std::string& key = entries[index].second;
            const auto key_len =
                static_cast<uint8_t>(std::min<std::size_t>(key.size(), IVP_KEY_MAX_LEN));
            if (ivp_telemetry_define_add_f32(&b, entries[index].first, key.c_str(), key_len) !=
                IVP_OK) {
                break;
            }
        }
        if (index == frame_start) return false;  // one key too large to ever fit

        uint8_t packet[IVP_HEADER_SIZE + IVP_DEFINE_PAYLOAD_MAX + 2];
        size_t packet_len = 0;
        if (ivp_packet_encode(IVP_MSG_TELEMETRY_DEFINE, seq_++, time_us, payload,
                              static_cast<uint16_t>(b.len), packet, sizeof(packet),
                              &packet_len) != IVP_OK) {
            return false;
        }
        if (!SendFramed(c, packet, packet_len)) return false;
    }

    c.needs_define = false;
    return true;
}

bool TelemetryPublisher::SendData(Client& c, uint32_t time_us) {
    uint8_t payload[IVP_DATA_PAYLOAD_MAX];
    ivp_data_builder_t b;
    if (ivp_telemetry_data_begin(&b, payload, sizeof(payload)) != IVP_OK) return false;

    {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& kv : signals_) {
            const auto& sig = kv.second;
            if (ivp_telemetry_data_add_f32(&b, sig.id, sig.value) != IVP_OK) break;
        }
    }

    uint8_t packet[IVP_HEADER_SIZE + IVP_DATA_PAYLOAD_MAX + 2];
    size_t packet_len = 0;
    if (ivp_packet_encode(IVP_MSG_TELEMETRY_DATA, seq_++, time_us, payload,
                          static_cast<uint16_t>(b.len), packet, sizeof(packet),
                          &packet_len) != IVP_OK) {
        return false;
    }
    return SendFramed(c, packet, packet_len);
}

bool TelemetryPublisher::SendDataExcludePrefix(Client& c, uint32_t time_us,
                                               const char* exclude_prefix) {
    if (!exclude_prefix) {
        return SendData(c, time_us);
    }
    const std::string exclude(exclude_prefix);
    uint8_t payload[IVP_DATA_PAYLOAD_MAX];
    ivp_data_builder_t b;
    if (ivp_telemetry_data_begin(&b, payload, sizeof(payload)) != IVP_OK) return false;

    {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& kv : signals_) {
            if (kv.first.compare(0, exclude.size(), exclude) == 0) {
                continue;
            }
            if (ivp_telemetry_data_add_f32(&b, kv.second.id, kv.second.value) != IVP_OK) {
                break;
            }
        }
    }

    if (b.len == 0) return true;

    uint8_t packet[IVP_HEADER_SIZE + IVP_DATA_PAYLOAD_MAX + 2];
    size_t packet_len = 0;
    if (ivp_packet_encode(IVP_MSG_TELEMETRY_DATA, seq_++, time_us, payload,
                          static_cast<uint16_t>(b.len), packet, sizeof(packet),
                          &packet_len) != IVP_OK) {
        return false;
    }
    return SendFramed(c, packet, packet_len);
}

void TelemetryPublisher::PublishPlantCycle(uint32_t time_us, const char* exclude_prefix) {
    while (AcceptPending()) {
    }
    if (clients_.empty()) return;

    for (size_t i = 0; i < clients_.size();) {
        Client& c = clients_[i];
        bool ok = true;
        if (c.needs_define || define_dirty_) {
            ok = SendDefine(c, time_us);
        }
        if (ok) ok = SendDataExcludePrefix(c, time_us, exclude_prefix);
        if (!ok) {
            DropClient(i);
            continue;
        }
        ++i;
    }
}

bool TelemetryPublisher::SendDataPrefix(Client& c, uint32_t time_us,
                                        const char* key_prefix) {
    if (!key_prefix) return false;
    const std::string prefix(key_prefix);
    uint8_t payload[IVP_DATA_PAYLOAD_MAX];
    ivp_data_builder_t b;
    if (ivp_telemetry_data_begin(&b, payload, sizeof(payload)) != IVP_OK) return false;

    {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& kv : signals_) {
            if (kv.first.compare(0, prefix.size(), prefix) != 0) {
                continue;
            }
            if (ivp_telemetry_data_add_f32(&b, kv.second.id, kv.second.value) != IVP_OK) {
                break;
            }
        }
    }

    if (b.len == 0) return true;

    uint8_t packet[IVP_HEADER_SIZE + IVP_DATA_PAYLOAD_MAX + 2];
    size_t packet_len = 0;
    if (ivp_packet_encode(IVP_MSG_TELEMETRY_DATA, seq_++, time_us, payload,
                          static_cast<uint16_t>(b.len), packet, sizeof(packet),
                          &packet_len) != IVP_OK) {
        return false;
    }
    return SendFramed(c, packet, packet_len);
}

void TelemetryPublisher::PublishPrefixCycle(uint32_t time_us, const char* key_prefix) {
    while (AcceptPending()) {
    }
    if (clients_.empty()) return;

    for (size_t i = 0; i < clients_.size();) {
        Client& c = clients_[i];
        bool ok = true;
        if (c.needs_define || define_dirty_) {
            ok = SendDefine(c, time_us);
        }
        if (ok) ok = SendDataPrefix(c, time_us, key_prefix);
        if (!ok) {
            DropClient(i);
            continue;
        }
        ++i;
    }
}

void TelemetryPublisher::PublishCycle(uint32_t time_us) {
    while (AcceptPending()) {
    }
    if (clients_.empty()) return;

    for (size_t i = 0; i < clients_.size();) {
        Client& c = clients_[i];
        bool ok = true;
        if (c.needs_define || define_dirty_) {
            ok = SendDefine(c, time_us);
        }
        if (ok) ok = SendData(c, time_us);
        if (!ok) {
            DropClient(i);
            continue;
        }
        ++i;
    }
}

void TelemetryPublisher::HandleLine(const std::string& raw) {
    std::string line = raw;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
        line.pop_back();
    }
    if (line.empty()) return;

    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;
    if (cmd == "help") {
        std::printf("HostSim commands: throttle a|b <0..1>, duty u|v|w <0..100>, speed <factor>|turbo, pause, resume, clear, help, quit\n");
        std::printf("  speed examples: speed 0.25 | speed 0.5 | speed 1 | speed 2 | speed turbo\n");
        return;
    }
    if (cmd == "speed" || cmd == "realtime") {
        std::string token;
        iss >> token;
        float factor = 1.0f;
        if (token == "turbo" || token == "max") {
            factor = 0.0f;
        } else {
            factor = std::strtof(token.c_str(), nullptr);
        }
        if (factor < 0.0f) factor = 0.0f;
        auto& runtime = GlobalSimRuntime();
        runtime.SetRealtimeFactor(factor);
        runtime.ResetWallClockAnchor();
        if (factor <= 0.0f) {
            std::printf("HostSim: speed = turbo (no wall-clock limit)\n");
        } else {
            std::printf("HostSim: speed = %.3fx\n", static_cast<double>(factor));
        }
        return;
    }
    if (cmd == "pause") {
        paused_ = true;
        std::printf("HostSim: simulation paused\n");
        return;
    }
    if (cmd == "resume" || cmd == "unpause") {
        paused_ = false;
        std::printf("HostSim: simulation resumed\n");
        return;
    }
    if (cmd == "quit" || cmd == "exit" || cmd == "stop") {
        quit_requested_ = true;
        return;
    }
    if (cmd == "clear") {
        ClearThrottleOverrides();
        ClearDutyOverrides();
        std::printf("HostSim: overrides cleared\n");
        return;
    }
    if (cmd == "throttle") {
        std::string which;
        float v = 0.0f;
        iss >> which >> v;
        if (which == "a") {
            throttle_a_override_ = true;
            throttle_a_ = std::clamp(v, 0.0f, 1.0f);
            std::printf("HostSim: throttle_a = %.3f\n", static_cast<double>(throttle_a_));
        } else if (which == "b") {
            throttle_b_override_ = true;
            throttle_b_ = std::clamp(v, 0.0f, 1.0f);
            std::printf("HostSim: throttle_b = %.3f\n", static_cast<double>(throttle_b_));
        } else {
            std::printf("HostSim: usage: throttle a|b <value>\n");
        }
        return;
    }
    if (cmd == "duty") {
        std::string which;
        iss >> which;
        if (which == "clear") {
            ClearDutyOverrides();
            std::printf("HostSim: duty overrides cleared\n");
            return;
        }

        float v = 0.0f;
        iss >> v;
        const float clamped = std::clamp(v, 0.0f, 100.0f);
        if (which == "u") {
            duty_u_override_ = true;
            duty_u_ = clamped;
            std::printf("HostSim: duty_u = %.3f\n", static_cast<double>(duty_u_));
        } else if (which == "v") {
            duty_v_override_ = true;
            duty_v_ = clamped;
            std::printf("HostSim: duty_v = %.3f\n", static_cast<double>(duty_v_));
        } else if (which == "w") {
            duty_w_override_ = true;
            duty_w_ = clamped;
            std::printf("HostSim: duty_w = %.3f\n", static_cast<double>(duty_w_));
        } else {
            std::printf("HostSim: usage: duty u|v|w <value> | duty clear\n");
        }
        return;
    }
    std::printf("HostSim: unknown command '%s' (try help)\n", cmd.c_str());
}

bool TelemetryPublisher::PollCommands() {
    while (AcceptPending()) {
    }

    for (size_t i = 0; i < clients_.size();) {
        Client& c = clients_[i];
        char buf[256];
#ifdef _WIN32
        const int n = ::recv(c.fd, buf, sizeof(buf), 0);
        if (n == SOCKET_ERROR) {
            if (WouldBlock()) {
                ++i;
                continue;
            }
            DropClient(i);
            continue;
        }
#else
        const int n = static_cast<int>(::recv(c.fd, buf, sizeof(buf), 0));
        if (n < 0) {
            if (WouldBlock()) {
                ++i;
                continue;
            }
            DropClient(i);
            continue;
        }
#endif
        if (n == 0) {
            DropClient(i);
            continue;
        }
        c.rx.append(buf, buf + n);
        size_t pos;
        while ((pos = c.rx.find('\n')) != std::string::npos) {
            std::string line = c.rx.substr(0, pos);
            c.rx.erase(0, pos + 1);
            HandleLine(line);
        }
        ++i;
    }
    return !quit_requested_;
}

bool TelemetryPublisher::HasThrottleOverrideA() const { return throttle_a_override_; }
bool TelemetryPublisher::HasThrottleOverrideB() const { return throttle_b_override_; }
float TelemetryPublisher::ThrottleOverrideA() const { return throttle_a_; }
float TelemetryPublisher::ThrottleOverrideB() const { return throttle_b_; }
bool TelemetryPublisher::HasDutyOverrideU() const { return duty_u_override_; }
bool TelemetryPublisher::HasDutyOverrideV() const { return duty_v_override_; }
bool TelemetryPublisher::HasDutyOverrideW() const { return duty_w_override_; }
float TelemetryPublisher::DutyOverrideU() const { return duty_u_; }
float TelemetryPublisher::DutyOverrideV() const { return duty_v_; }
float TelemetryPublisher::DutyOverrideW() const { return duty_w_; }

void TelemetryPublisher::ClearThrottleOverrides() {
    throttle_a_override_ = false;
    throttle_b_override_ = false;
}

void TelemetryPublisher::ClearDutyOverrides() {
    duty_u_override_ = false;
    duty_v_override_ = false;
    duty_w_override_ = false;
}

} // namespace hostsim
