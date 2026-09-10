#include "can_bridge.h"

#include "platform_api.h" /* platform_can_send (selftest emitter) */
#include "sim_context.h"  /* SimCanInject */

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <process.h>
#else
#include <cerrno>
#include <chrono>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace hostsim {

CanBridge& GlobalCanBridge() {
    static CanBridge bridge;
    return bridge;
}

void CanBridge::Configure(const CanBridgeConfig& cfg) {
    cfg_ = cfg;
    if (cfg_.instance_id == 0) {
        /* Deliberately pid-derived, not telemetry-port-derived: concurrent
         * instances commonly share the default telemetry port (14608), which
         * would tag their frames identically and make them filter each
         * other out. The full pid (31-bit on Linux) is unique by
         * construction; truncation to a byte would collide after ~256
         * instances. */
#ifdef _WIN32
        cfg_.instance_id = static_cast<uint32_t>(_getpid());
#else
        cfg_.instance_id = static_cast<uint32_t>(::getpid());
#endif
        if (cfg_.instance_id == 0) cfg_.instance_id = 1;
    }
    configured_ = cfg_.hub || cfg_.connect;
}

#ifdef _WIN32

bool CanBridge::Start() {
    if (configured_ || cfg_.selftest) {
        std::fprintf(stderr,
                     "[CAN bridge] not supported on Windows; bridge disabled\n");
    }
    return false;
}

void CanBridge::Poll(float) {}
void CanBridge::Publish(uint8_t, uint32_t, bool, const uint8_t*, uint8_t) {}
void CanBridge::Shutdown() {}

#else // !_WIN32

namespace {

constexpr uint32_t kMagic = 0x314E4143u; /* "CAN1" on the wire */
constexpr uint16_t kPayloadLen = 24;
constexpr size_t kRecordLen = 2 + kPayloadLen;
constexpr float kSelftestPeriodS = 0.1f;
constexpr uint32_t kSelftestId = 0x123u;
constexpr uint64_t kConnectTimeoutMs = 5000;

uint16_t GetLe16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}
uint32_t GetLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
void PutLe16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
void PutLe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

bool SetNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void TuneSocket(int fd) {
    SetNonBlocking(fd);
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}

} // namespace

bool CanBridge::Start() {
    if (!configured_) {
        if (cfg_.selftest) {
            std::fprintf(stderr,
                         "[CAN bridge] selftest requested but neither "
                         "--can-bridge-listen nor --can-bridge-connect given; "
                         "selftest frames go nowhere\n");
        }
        return false;
    }
    const bool ok = cfg_.hub ? StartHub() : StartSpoke();
    if (ok && cfg_.selftest) {
        std::printf("[CAN bridge] selftest on: bus=0 id=0x%X every %.0f ms "
                    "(sim time)\n",
                    kSelftestId,
                    static_cast<double>(kSelftestPeriodS * 1000.0f));
        std::fflush(stdout);
    }
    return ok;
}

bool CanBridge::StartHub() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd_ < 0) {
        std::fprintf(stderr, "[CAN bridge] socket() failed: %s\n",
                     std::strerror(errno));
        return false;
    }
    int yes = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(cfg_.port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr),
               sizeof(addr)) != 0 ||
        ::listen(listen_fd_, 8) != 0 || !SetNonBlocking(listen_fd_)) {
        std::fprintf(stderr,
                     "[CAN bridge] hub listen on :%d failed: %s; running "
                     "unbridged\n",
                     cfg_.port, std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    is_hub_ = true;
    enabled_ = true;
    std::printf("[CAN bridge] hub on :%d id=%u\n", cfg_.port, cfg_.instance_id);
    std::fflush(stdout);
    return true;
}

bool CanBridge::StartSpoke() {
    hub_link_.fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (hub_link_.fd < 0) {
        std::fprintf(stderr, "[CAN bridge] socket() failed: %s\n",
                     std::strerror(errno));
        return false;
    }
    TuneSocket(hub_link_.fd);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(cfg_.port));
    if (inet_pton(AF_INET, cfg_.host.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr,
                     "[CAN bridge] connect host \"%s\" is not a dotted IPv4 "
                     "address; running unbridged\n",
                     cfg_.host.c_str());
        ::close(hub_link_.fd);
        hub_link_.fd = -1;
        spoke_state_ = SpokeState::Down;
        return false;
    }

    std::printf("[CAN bridge] connecting to %s:%d id=%u\n", cfg_.host.c_str(),
                cfg_.port, cfg_.instance_id);
    std::fflush(stdout);

    const int rc = ::connect(hub_link_.fd, reinterpret_cast<sockaddr*>(&addr),
                             sizeof(addr));
    if (rc == 0) {
        spoke_state_ = SpokeState::Connected;
        std::printf("[CAN bridge] connected to hub %s:%d\n",
                    cfg_.host.c_str(), cfg_.port);
        std::fflush(stdout);
    } else if (errno == EINPROGRESS) {
        spoke_state_ = SpokeState::Pending;
        connect_start_ms_ = NowMs();
    } else {
        char why[128];
        std::snprintf(why, sizeof(why), "connect failed: %s",
                      std::strerror(errno));
        DropHubLink(why);
        return false;
    }
    enabled_ = true;
    return true;
}

void CanBridge::DropHubLink(const char* reason) {
    if (hub_link_.fd >= 0) {
        ::close(hub_link_.fd);
        hub_link_.fd = -1;
    }
    if (spoke_state_ != SpokeState::Down) {
        std::printf("[CAN bridge] hub %s:%d %s; continuing unbridged\n",
                    cfg_.host.c_str(), cfg_.port, reason);
        std::fflush(stdout);
    }
    spoke_state_ = SpokeState::Down;
}

bool CanBridge::SendRecord(Peer& p, const uint8_t* record, size_t len) {
    if (p.fd < 0) return false;
    const ssize_t n = ::send(p.fd, record, len, MSG_NOSIGNAL);
    if (n == static_cast<ssize_t>(len)) {
        ++tx_frames_;
        return true;
    }
    ++dropped_;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        /* Peer too slow: drop the frame, keep the connection. Frames are
         * latest-value, not a queue — never block the sim for them. */
        return false;
    }
    /* Hard error (incl. a rare partial write): the link is unusable. */
    ::close(p.fd);
    p.fd = -1;
    return false;
}

bool CanBridge::HandleRecord(const uint8_t* record, int origin_fd) {
    const uint8_t* p = record + 2; /* skip length prefix */
    if (GetLe32(p) != kMagic) return false;
    const uint32_t src = GetLe32(p + 4);
    const uint32_t id = GetLe32(p + 8);
    const uint8_t bus = p[12];
    const bool ext = p[13] != 0;
    uint8_t dlc = p[14];
    if (dlc > 8) dlc = 8;
    const uint8_t* data = p + 16;

    if (src == cfg_.instance_id) {
        /* Looped back to the origin: never duplicate own frames into RX. */
        ++filtered_;
        return true;
    }

    SimCanInject(bus, id, ext, data, dlc);
    ++rx_frames_;

    /* Receive witness: always on (greppable), one line per bridged frame. */
    std::printf("[CAN bridge] rx bus=%u id=0x%X dlc=%u src=%u data=",
                static_cast<unsigned>(bus), static_cast<unsigned>(id),
                static_cast<unsigned>(dlc), static_cast<unsigned>(src));
    for (uint8_t i = 0; i < dlc; ++i) {
        std::printf("%s%02X", i == 0 ? "" : " ", data[i]);
    }
    std::printf("\n");
    std::fflush(stdout);

    if (origin_fd >= 0) {
        /* Hub: forward the verbatim record (origin tag preserved) to every
         * other spoke. SendRecord only marks a failed peer dead (fd = -1);
         * erasing is deferred to SweepDeadPeers() because `record` aliases
         * the origin peer's rx buffer and the caller holds a Peer& into
         * peers_ — erasing mid-loop would invalidate both. */
        for (auto& other : peers_) {
            if (other.fd < 0 || other.fd == origin_fd) continue;
            SendRecord(other, record, kRecordLen);
        }
    }
    return true;
}

void CanBridge::ServicePeer(Peer& p) {
    if (p.fd < 0) return;
    bool alive = true;
    bool framing_ok = true;
    while (alive && framing_ok) {
        /* Drain every complete record already buffered before reading more:
         * a burst between app-tick polls is then limited only by what a
         * single iteration can read, not by the buffer size, and the buffer
         * never holds more than one partial record. */
        size_t off = 0;
        while (p.rx_len - off >= 2) {
            const uint16_t plen = GetLe16(p.rx + off);
            if (plen != kPayloadLen) {
                framing_ok = false; /* unknown protocol; drop the peer */
                break;
            }
            if (p.rx_len - off < 2u + static_cast<size_t>(plen)) break; /* wait for the remainder */
            if (!HandleRecord(p.rx + off, is_hub_ ? p.fd : -1)) {
                framing_ok = false; /* bad magic */
                break;
            }
            off += 2 + plen;
        }
        if (!framing_ok) break;
        if (off > 0 && off < p.rx_len) {
            std::memmove(p.rx, p.rx + off, p.rx_len - off);
        }
        p.rx_len -= off;
        if (p.rx_len == sizeof(p.rx)) {
            /* A full buffer after draining means one unterminated record
             * spans everything: genuinely unparseable framing, not load. */
            framing_ok = false;
            break;
        }

        const ssize_t n =
            ::recv(p.fd, p.rx + p.rx_len, sizeof(p.rx) - p.rx_len, 0);
        if (n > 0) {
            p.rx_len += static_cast<size_t>(n);
        } else if (n == 0) {
            alive = false; /* orderly EOF; any buffered records were drained above */
        } else if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            break;
        } else {
            alive = false; /* hard error */
        }
    }

    if (!alive || !framing_ok) {
        ::close(p.fd);
        p.fd = -1; /* dead marker; SweepDeadPeers does the erase */
        p.rx_len = 0;
    }
}

void CanBridge::SweepDeadPeers() {
    const size_t before = peers_.size();
    peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
                                [](const Peer& p) { return p.fd < 0; }),
                 peers_.end());
    if (peers_.size() != before) {
        std::printf("[CAN bridge] peer disconnected (%zu remaining)\n",
                    peers_.size());
        std::fflush(stdout);
    }
}

void CanBridge::AcceptAll() {
    while (true) {
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        const int fd =
            ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (fd < 0) return; /* EAGAIN: nothing pending */
        TuneSocket(fd);
        Peer p;
        p.fd = fd;
        peers_.push_back(p);
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        std::printf("[CAN bridge] peer connected from %s:%u (%zu peers)\n", ip,
                    static_cast<unsigned>(ntohs(peer.sin_port)),
                    peers_.size());
        std::fflush(stdout);
    }
}

void CanBridge::CheckConnect() {
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(hub_link_.fd, &wfds);
    timeval tv{0, 0};
    const int rc = ::select(hub_link_.fd + 1, nullptr, &wfds, nullptr, &tv);
    if (rc > 0) {
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(hub_link_.fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err == 0) {
            spoke_state_ = SpokeState::Connected;
            std::printf("[CAN bridge] connected to hub %s:%d\n",
                        cfg_.host.c_str(), cfg_.port);
            std::fflush(stdout);
            return;
        }
        char why[128];
        std::snprintf(why, sizeof(why), "connect failed: %s",
                      std::strerror(err));
        DropHubLink(why);
        return;
    }
    if (NowMs() - connect_start_ms_ > kConnectTimeoutMs) {
        DropHubLink("connect timed out");
    }
}

void CanBridge::RunSelftest(float now_s) {
    if (!cfg_.selftest || now_s + 1e-9f < next_selftest_s_) return;
    next_selftest_s_ += kSelftestPeriodS;
    uint8_t data[8];
    for (uint32_t i = 0; i < 8; ++i) {
        data[i] = static_cast<uint8_t>(selftest_step_ + i);
    }
    ++selftest_step_;
    /* Bus 0 is deliberately not a local bus (Gen6 numbering is 1-based): it
     * exists only on the bridge as the selftest/diagnostic channel and never
     * enters the local latest-frame store. */
    platform_can_send(0, kSelftestId, false, data, 8);
}

void CanBridge::Poll(float now_s) {
    if (!enabled_) return;

    if (is_hub_) {
        AcceptAll();
        for (auto& p : peers_) {
            ServicePeer(p);
        }
        /* Dead peers were only marked while servicing (no erases happened
         * above — ServicePeer/HandleRecord may alias into peers_); erase
         * them now that nothing holds a reference. */
        SweepDeadPeers();
    } else {
        if (spoke_state_ == SpokeState::Pending) {
            CheckConnect();
        }
        if (spoke_state_ == SpokeState::Connected) {
            const int fd_before = hub_link_.fd;
            ServicePeer(hub_link_);
            if (hub_link_.fd < 0 && fd_before >= 0) {
                DropHubLink("disconnected");
            }
        }
    }

    RunSelftest(now_s);
}

void CanBridge::Publish(uint8_t bus, uint32_t id, bool ext,
                        const uint8_t* data, uint8_t dlc) {
    if (!enabled_) return;

    uint8_t rec[kRecordLen];
    PutLe16(rec, kPayloadLen);
    PutLe32(rec + 2, kMagic);
    PutLe32(rec + 6, cfg_.instance_id);
    PutLe32(rec + 10, id);
    rec[14] = bus;
    rec[15] = ext ? 1 : 0;
    rec[16] = dlc > 8 ? 8 : dlc;
    rec[17] = 0;
    for (uint8_t i = 0; i < 8; ++i) {
        rec[18 + i] = (data && i < rec[16]) ? data[i] : 0;
    }

    if (cfg_.debug) {
        std::printf("[CAN bridge] tx bus=%u id=0x%X dlc=%u\n",
                    static_cast<unsigned>(bus), static_cast<unsigned>(id),
                    static_cast<unsigned>(rec[16]));
        std::fflush(stdout);
    }

    if (is_hub_) {
        for (auto& p : peers_) {
            SendRecord(p, rec, kRecordLen);
        }
        SweepDeadPeers();
    } else if (spoke_state_ == SpokeState::Connected) {
        SendRecord(hub_link_, rec, kRecordLen);
        if (hub_link_.fd < 0) {
            DropHubLink("send failed");
        }
    }
}

void CanBridge::Shutdown() {
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    for (auto& p : peers_) {
        if (p.fd >= 0) ::close(p.fd);
    }
    peers_.clear();
    if (hub_link_.fd >= 0) {
        ::close(hub_link_.fd);
        hub_link_.fd = -1;
    }
    if (!enabled_) return;
    enabled_ = false;
    std::printf("[CAN bridge] stats: tx=%llu rx=%llu dropped=%llu "
                "filtered=%llu\n",
                static_cast<unsigned long long>(tx_frames_),
                static_cast<unsigned long long>(rx_frames_),
                static_cast<unsigned long long>(dropped_),
                static_cast<unsigned long long>(filtered_));
    std::fflush(stdout);
}

#endif // _WIN32

} // namespace hostsim
