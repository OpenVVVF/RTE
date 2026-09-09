/*
 * sil_live_server.cpp — see sil_live_server.h.
 *
 * Socket plumbing mirrors HostSim's telemetry_publisher.cpp (non-blocking
 * listen socket, per-client drain).  Unlike HostSim — which re-frames a
 * host-side key/value map — this server is a pure byte proxy: the firmware's
 * own COBS-framed InverterProtocol UART bytes are forwarded verbatim, so the
 * TCP stream is bit-identical to the hardware USART3 wire.  Client→server
 * bytes (RTEStudio text console lines, which end with '\n') are likewise
 * forwarded verbatim into the firmware's huart3 IT-RX model
 * (silUartRxEnqueue); the Gen6FW CommandShell accepts both '\n' and '\r\n'
 * line endings, so no translation is needed.
 */
#include "sil_live_server.h"
#include "sil_hooks.h"   /* silUartRxEnqueue */

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using Socket = int;
constexpr Socket kInvalid = -1;

/* Bytes held for connected clients between polls.  At the firmware's default
 * telemetry rates this never exceeds a few hundred bytes; the cap only
 * exists to bound memory if poll() stops being called. */
constexpr size_t kPendingCap = 1u << 20;

/* send() attempts per client per poll before treating it as stalled; a
 * client that stays stalled for this many polls is dropped.  Polls run once
 * per app-loop iteration (1 kHz at --realtime 1.0), so 500 stalls ~ 0.5 s
 * of a completely wedged client. */
constexpr int kSendAttemptsPerPoll = 64;
constexpr int kMaxStallPolls = 500;

struct Client {
    Socket fd = kInvalid;
    std::string rx;      /* demux buffer for client->device text lines */
    int stall_polls = 0; /* polls where the socket buffer stayed full */
};

struct LiveServer {
    int listen_fd = -1;
    std::vector<Client> clients;
    std::vector<uint8_t> pending;  /* firmware UART TX bytes not yet flushed */
    std::mutex mu;                 /* guards pending (feed runs fw-context) */
};

LiveServer g_live;

bool SetNonBlocking(Socket s) {
    const int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool WouldBlock() {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

void CloseSock(Socket s) {
    if (s != kInvalid) ::close(s);
}

/* Write all bytes within a bounded number of send() attempts.
 * Returns false if the socket errored; on a full socket buffer it returns
 * true but sets progressed=false. */
bool WriteAll(Socket fd, const uint8_t* data, size_t n, bool& progressed) {
    progressed = false;
    size_t total = 0;
    for (int attempt = 0; attempt < kSendAttemptsPerPoll && total < n; ++attempt) {
#ifdef MSG_NOSIGNAL
        const ssize_t wrote =
            ::send(fd, data + total, n - total, MSG_NOSIGNAL);
#else
        const ssize_t wrote = ::send(fd, data + total, n - total, 0);
#endif
        if (wrote < 0) {
            if (WouldBlock()) continue;
            return false;
        }
        if (wrote == 0) break;
        progressed = true;
        total += static_cast<size_t>(wrote);
    }
    if (total < n) progressed = false; /* did not finish this poll */
    return true;
}

void HandleRxLine(LiveServer&, const std::string& raw) {
    std::string line = raw;
    while (!line.empty() &&
           (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
        line.pop_back();
    }
    if (line.empty()) return;
    std::printf("[SIL live] -> shell: %s\n", line.c_str());
    std::fflush(stdout);
}

void DropClient(LiveServer& srv, size_t index) {
    CloseSock(srv.clients[index].fd);
    srv.clients.erase(srv.clients.begin() + static_cast<std::ptrdiff_t>(index));
    std::printf("[SIL live] client disconnected (%zu remaining)\n",
                srv.clients.size());
    std::fflush(stdout);
}

} // namespace

bool sil_live_start(const char* host, uint16_t port) {
    sil_live_stop();

    Socket fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == kInvalid) {
        std::fprintf(stderr, "[SIL live] socket() failed: %s\n",
                     std::strerror(errno));
        return false;
    }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host != nullptr ? host : "127.0.0.1",
                  &addr.sin_addr) != 1 ||
        ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(fd, 4) != 0 || !SetNonBlocking(fd)) {
        std::fprintf(stderr, "[SIL live] cannot listen on %s:%u: %s "
                     "(continuing without live link)\n",
                     host != nullptr ? host : "127.0.0.1",
                     static_cast<unsigned>(port), std::strerror(errno));
        CloseSock(fd);
        return false;
    }

    g_live.listen_fd = fd;
    std::printf("[SIL live] listening on %s:%u "
                "(RTEStudio: --tcp %s:%u --protocol ivp)\n",
                host != nullptr ? host : "127.0.0.1",
                static_cast<unsigned>(port), host != nullptr ? host : "127.0.0.1",
                static_cast<unsigned>(port));
    std::fflush(stdout);
    return true;
}

void sil_live_stop() {
    for (auto& c : g_live.clients) CloseSock(c.fd);
    g_live.clients.clear();
    if (g_live.listen_fd != -1) {
        CloseSock(static_cast<Socket>(g_live.listen_fd));
        g_live.listen_fd = -1;
    }
    {
        std::lock_guard<std::mutex> lock(g_live.mu);
        g_live.pending.clear();
    }
}

bool sil_live_active() {
    return g_live.listen_fd != -1;
}

void sil_live_feed_tx(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) return;
    if (g_live.listen_fd == -1 || g_live.clients.empty()) return;

    std::lock_guard<std::mutex> lock(g_live.mu);
    auto& p = g_live.pending;
    if (p.size() + len > kPendingCap) {
        /* Client is not draining; drop oldest bytes (clients resync on the
         * next 0x00 COBS delimiter and CRC-check each frame). */
        const size_t drop = p.size() + len - kPendingCap;
        p.erase(p.begin(), p.begin() + static_cast<std::ptrdiff_t>(drop));
    }
    p.insert(p.end(), data, data + len);
}

void sil_live_poll() {
    if (g_live.listen_fd == -1) return;

    /* Accept all pending connects. */
    for (;;) {
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        Socket cfd = ::accept(static_cast<Socket>(g_live.listen_fd),
                              reinterpret_cast<sockaddr*>(&peer), &plen);
        if (cfd == kInvalid) break;
        SetNonBlocking(cfd);
        Client c;
        c.fd = cfd;
        g_live.clients.push_back(std::move(c));
        std::printf("[SIL live] client connected (%zu total)\n",
                    g_live.clients.size());
        std::fflush(stdout);
    }

    /* Drain client -> server bytes: forward every byte verbatim into the
     * modeled huart3 IT-RX FIFO (the firmware CommandShell path) and mirror
     * complete text lines to stdout. */
    for (size_t i = 0; i < g_live.clients.size();) {
        Client& c = g_live.clients[i];
        char buf[256];
        bool drop = false;
        for (;;) {
            const ssize_t n = ::recv(c.fd, buf, sizeof(buf), 0);
            if (n < 0) {
                drop = !WouldBlock();
                break;
            }
            if (n == 0) {
                drop = true;
                break;
            }
            silUartRxEnqueue(reinterpret_cast<const uint8_t*>(buf),
                             static_cast<size_t>(n));
            c.rx.append(buf, buf + n);
            size_t pos;
            while ((pos = c.rx.find('\n')) != std::string::npos) {
                std::string line = c.rx.substr(0, pos);
                c.rx.erase(0, pos + 1);
                HandleRxLine(g_live, line);
            }
            if (c.rx.size() > 4096) c.rx.erase(0, c.rx.size() - 1024);
        }
        if (drop) {
            DropClient(g_live, i);
            continue;
        }
        ++i;
    }

    /* Flush queued firmware UART bytes to every client. */
    std::vector<uint8_t> bytes;
    {
        std::lock_guard<std::mutex> lock(g_live.mu);
        bytes.swap(g_live.pending);
    }
    if (bytes.empty()) return;

    for (size_t i = 0; i < g_live.clients.size();) {
        Client& c = g_live.clients[i];
        bool progressed = false;
        const bool ok = WriteAll(c.fd, bytes.data(), bytes.size(), progressed);
        if (!ok) {
            DropClient(g_live, i);
            continue;
        }
        c.stall_polls = progressed ? 0 : c.stall_polls + 1;
        if (c.stall_polls >= kMaxStallPolls) {
            std::printf("[SIL live] dropping stalled client\n");
            std::fflush(stdout);
            DropClient(g_live, i);
            continue;
        }
        ++i;
    }
}
