#include "LegacyTelemetryClient.h"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <termios.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

namespace NodeGUI::runtime {

namespace {

// ---------------- Protocol constants (unchanged from the original) ----------------
constexpr uint32_t MAGIC   = 0x544C4D31u; // "TLM1"
constexpr uint8_t  VERSION = 1;
constexpr int      BAUD_RATE = 460800;    // fixed serial baud

enum MsgType : uint8_t {
    MSG_DATA   = 1,
    MSG_DEFINE = 2,
};

enum ValueType : uint8_t {
    VT_F32      = 1,
    VT_STR      = 2,   // complete short string
    VT_STR_FRAG = 3,   // fragment of a longer string
};

// Fragment flags for VT_STR_FRAG payloads
enum StrFrag : uint8_t {
    SF_START    = 0x01,
    SF_END      = 0x02,
    SF_COMPLETE = 0x03, // START | END
};

#pragma pack(push, 1)
struct TelemetryHeader {
    uint32_t magic;       // MAGIC
    uint8_t  version;     // VERSION
    uint8_t  msg_type;    // MsgType
    uint16_t payload_len; // bytes
    uint32_t seq;
    uint32_t time_us;     // pico time_us_32()
};
#pragma pack(pop)

// ---------------- CRC16-CCITT (0x1021, init 0xFFFF) ----------------
uint16_t crc16_ccitt(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

// ---------------- COBS decode ----------------
size_t cobs_decode(const uint8_t* in, size_t len, uint8_t* out, size_t out_cap) {
    size_t r = 0, w = 0;
    while (r < len) {
        uint8_t code = in[r++];
        if (code == 0) throw std::runtime_error("COBS: zero code");
        for (uint8_t i = 1; i < code; ++i) {
            if (r >= len) throw std::runtime_error("COBS: overrun");
            if (w >= out_cap) throw std::runtime_error("COBS: out_cap");
            out[w++] = in[r++];
        }
        if (code != 0xFF && r < len) {
            if (w >= out_cap) throw std::runtime_error("COBS: out_cap");
            out[w++] = 0;
        }
    }
    return w;
}

// ---------------- Payload decoding helpers ----------------
inline uint16_t rd_u16(const uint8_t*& p, const uint8_t* end) {
    if (end - p < 2) throw std::runtime_error("u16");
    uint16_t v = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    p += 2;
    return v;
}
inline uint8_t rd_u8(const uint8_t*& p, const uint8_t* end) {
    if (end - p < 1) throw std::runtime_error("u8");
    return *p++;
}
inline float rd_f32(const uint8_t*& p, const uint8_t* end) {
    if (end - p < 4) throw std::runtime_error("f32");
    float f;
    std::memcpy(&f, p, 4);
    p += 4;
    return f;
}

} // namespace

// ---------------- SerialPort ----------------
LegacyTelemetryClient::SerialPort::~SerialPort() { close(); }

bool LegacyTelemetryClient::SerialPort::isOpen() const {
    return h_ >= 0;
}

void LegacyTelemetryClient::SerialPort::close() {
    if (h_ >= 0) {
        ::close(h_);
        h_ = -1;
    }
}

bool LegacyTelemetryClient::SerialPort::open(const std::string& port, int baud) {
    (void)baud; // fixed at 460800 below
    close();

    int fd = ::open(port.c_str(), O_RDWR | O_NOCTTY);
    if (fd < 0) return false;

    termios tty{};
    if (tcgetattr(fd, &tty) != 0) { ::close(fd); return false; }

    cfsetispeed(&tty, B460800);
    cfsetospeed(&tty, B460800);
    cfmakeraw(&tty);

    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag &= ~HUPCL;          // Don't drop DTR on close (avoid device reset on reconnect)
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 1;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) { ::close(fd); return false; }

    // Flush any stale data in both directions before use.
    tcflush(fd, TCIOFLUSH);
    h_ = fd;
    return true;
}

int LegacyTelemetryClient::SerialPort::read(uint8_t* buf, int cap) {
    if (!isOpen()) return 0;
    int n = (int)::read(h_, buf, (size_t)cap);
    return n > 0 ? n : 0;
}

bool LegacyTelemetryClient::SerialPort::write(const uint8_t* data, int n) {
    if (!isOpen() || !data || n <= 0) return false;
    int total = 0;
    while (total < n) {
        int wrote = (int)::write(h_, data + total, (size_t)(n - total));
        if (wrote < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (wrote == 0) return false;
        total += wrote;
    }
    return true;
}

bool LegacyTelemetryClient::SerialPort::drain() {
    if (!isOpen()) return false;
    return ::tcdrain(h_) == 0;
}

// ---------------- TcpStream ----------------
LegacyTelemetryClient::TcpStream::~TcpStream() { close(); }

bool LegacyTelemetryClient::TcpStream::isOpen() const {
    return fd_ >= 0;
}

void LegacyTelemetryClient::TcpStream::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool LegacyTelemetryClient::TcpStream::open(const std::string& host, int port) {
    close();

    char portStr[16];
    std::snprintf(portStr, sizeof(portStr), "%d", port);

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), portStr, &hints, &result) != 0) return false;

    int fd = -1;
    for (addrinfo* ai = result; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;

        // Non-blocking connect with a 500 ms timeout: a plain connect() to an
        // unreachable host blocks for minutes and stalls the reader thread.
        const int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            pollfd pfd{fd, POLLOUT, 0};
            if (::poll(&pfd, 1, 500) == 1 && (pfd.revents & POLLOUT)) {
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
    freeaddrinfo(result);
    if (fd < 0) return false;

    // Match the serial read pacing: reads return promptly with whatever is
    // available, timing out after 100 ms.
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    fd_ = fd;
    return true;
}

int LegacyTelemetryClient::TcpStream::read(uint8_t* buf, int cap) {
    if (!isOpen()) return 0;
    const ssize_t n = ::recv(fd_, buf, (size_t)cap, 0);
    if (n == 0) {
        // Orderly close by the bridge: drop the connection so the reader's
        // reconnect logic kicks in.
        close();
        return 0;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        close();
        return 0;
    }
    return (int)n;
}

bool LegacyTelemetryClient::TcpStream::write(const uint8_t* data, int n) {
    if (!isOpen() || !data || n <= 0) return false;
    int total = 0;
    while (total < n) {
        const ssize_t wrote = ::send(fd_, data + total, (size_t)(n - total), MSG_NOSIGNAL);
        if (wrote < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (wrote == 0) return false;
        total += (int)wrote;
    }
    return true;
}

bool LegacyTelemetryClient::TcpStream::drain() {
    return isOpen();
}

// ---------------- LegacyTelemetryClient ----------------
LegacyTelemetryClient::~LegacyTelemetryClient() { stop(); }

bool LegacyTelemetryClient::start(const std::string& port) {
    stop();
    run_.store(true);
    thr_ = std::thread(&LegacyTelemetryClient::threadMain, this,
                       port, BAUD_RATE, std::ref(serial_));
    return true;
}

bool LegacyTelemetryClient::startTcp(const std::string& host, int port) {
    stop();
    run_.store(true);
    thr_ = std::thread(&LegacyTelemetryClient::threadMain, this,
                       host, port, std::ref(tcp_));
    return true;
}

void LegacyTelemetryClient::stop() {
    run_.store(false);
    if (thr_.joinable()) thr_.join();
    std::lock_guard<std::mutex> lk(serial_mtx_);
    serial_.close();
    tcp_.close();
}

void LegacyTelemetryClient::suspend() {
    suspended_.store(true);
    {
        std::lock_guard<std::mutex> lk(serial_mtx_);
        serial_.close();
        tcp_.close();
    }
    // Give the reader thread time to see the close and back off.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

void LegacyTelemetryClient::resume() {
    suspended_.store(false);
}

bool LegacyTelemetryClient::isSuspended() const {
    return suspended_.load();
}

bool LegacyTelemetryClient::sendLine(const std::string& line) {
    std::string out = line;
    if (out.empty()) return false;
    if (out.back() != '\n' && out.back() != '\r') out.push_back('\n');
    std::lock_guard<std::mutex> lk(serial_mtx_);
    ByteStream* stream = tcp_.isOpen() ? static_cast<ByteStream*>(&tcp_)
                                       : static_cast<ByteStream*>(&serial_);
    if (!stream->write((const uint8_t*)out.data(), (int)out.size())) return false;
    return stream->drain();
}

void LegacyTelemetryClient::ingestF32(const std::string& key, float v, float tsec) {
    if (onF32) onF32(key, v, tsec);
}

void LegacyTelemetryClient::ingestStr(const std::string& key, const std::string& v) {
    // Console text arrives as the string signal "print"; everything else is a
    // plain string signal.
    if (key == "print") {
        if (onConsole) onConsole(v);
    } else {
        if (onString) onString(key, v);
    }
}

void LegacyTelemetryClient::onDefine(uint16_t id, uint8_t type, const char* key, uint8_t key_len) {
    if (!key || key_len == 0) return;
    KeyDef def;
    def.type = type;
    def.key.assign(key, key + key_len);
    id_to_key_[id] = std::move(def);
}

bool LegacyTelemetryClient::lookupKey(uint16_t id, KeyDef& out) const {
    auto it = id_to_key_.find(id);
    if (it == id_to_key_.end()) return false;
    out = it->second;
    return true;
}

void LegacyTelemetryClient::parseDefinePayload(const uint8_t* payload, size_t len) {
    const uint8_t* p = payload;
    const uint8_t* end = payload + len;

    uint8_t n = rd_u8(p, end);

    for (uint8_t i = 0; i < n; ++i) {
        uint16_t id = rd_u16(p, end);
        uint8_t type = rd_u8(p, end);
        uint8_t klen = rd_u8(p, end);
        if ((size_t)(end - p) < klen) throw std::runtime_error("klen");
        onDefine(id, type, (const char*)p, klen);
        p += klen;
    }
}

void LegacyTelemetryClient::parseDataPayload(const uint8_t* payload, size_t len, float tsec) {
    const uint8_t* p = payload;
    const uint8_t* end = payload + len;

    uint8_t n = rd_u8(p, end);

    for (uint8_t i = 0; i < n; ++i) {
        uint16_t id = rd_u16(p, end);
        uint8_t wire_type = rd_u8(p, end);

        KeyDef def;
        if (!lookupKey(id, def)) {
            st_.rejectUnknownId++;
            // Skip value based on wire_type (so we stay in sync)
            if (wire_type == VT_F32) {
                (void)rd_f32(p, end);
            } else if (wire_type == VT_STR) {
                uint8_t sl = rd_u8(p, end);
                if ((size_t)(end - p) < sl) throw std::runtime_error("str");
                p += sl;
            } else if (wire_type == VT_STR_FRAG) {
                (void)rd_u8(p, end); // frag flags
                uint8_t sl = rd_u8(p, end);
                if ((size_t)(end - p) < sl) throw std::runtime_error("str frag");
                p += sl;
            } else {
                throw std::runtime_error("bad type");
            }
            continue;
        }

        // Trust DEFINE type, but still advance using the on-wire encoding.
        if (def.type == VT_F32) {
            // If wire_type isn't VT_F32, try to skip safely and count it.
            if (wire_type != VT_F32) {
                st_.rejectPayloadParse++;
                if (wire_type == VT_STR) {
                    uint8_t sl = rd_u8(p, end);
                    if ((size_t)(end - p) < sl) throw std::runtime_error("str");
                    p += sl;
                } else if (wire_type == VT_STR_FRAG) {
                    (void)rd_u8(p, end); // frag flags
                    uint8_t sl = rd_u8(p, end);
                    if ((size_t)(end - p) < sl) throw std::runtime_error("str frag");
                    p += sl;
                } else {
                    throw std::runtime_error("wire type");
                }
                continue;
            }
            float v = rd_f32(p, end);
            ingestF32(def.key, v, tsec);
        } else if (def.type == VT_STR) {
            if (wire_type == VT_F32) {
                st_.rejectPayloadParse++;
                (void)rd_f32(p, end);
                continue;
            }
            if (wire_type != VT_STR && wire_type != VT_STR_FRAG) {
                st_.rejectPayloadParse++;
                throw std::runtime_error("wire type");
            }

            if (wire_type == VT_STR) {
                uint8_t sl = rd_u8(p, end);
                if ((size_t)(end - p) < sl) throw std::runtime_error("str");
                std::string s((const char*)p, (const char*)p + sl);
                p += sl;
                partial_str_.erase(def.key);
                ingestStr(def.key, s);
            } else { // VT_STR_FRAG
                uint8_t frag = rd_u8(p, end);
                uint8_t sl = rd_u8(p, end);
                if ((size_t)(end - p) < sl) throw std::runtime_error("str frag");
                std::string chunk((const char*)p, (const char*)p + sl);
                p += sl;

                auto& part = partial_str_[def.key];
                if (frag & SF_START) {
                    part.buf.clear();
                }
                part.buf += chunk;
                part.last_tsec = tsec;

                if (frag & SF_END) {
                    ingestStr(def.key, part.buf);
                    partial_str_.erase(def.key);
                }
            }
        } else {
            st_.rejectPayloadParse++;
            throw std::runtime_error("unknown def type");
        }
    }
}

void LegacyTelemetryClient::threadMain(const std::string& target, int arg, ByteStream& stream) {
    auto reopen_and_settle = [&](bool first_time) {
        {
            std::lock_guard<std::mutex> lk(serial_mtx_);
            stream.close();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(first_time ? 200 : 150));

        while (run_.load()) {
            bool ok = false;
            {
                std::lock_guard<std::mutex> lk(serial_mtx_);
                ok = stream.open(target, arg);
            }
            if (ok) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (!run_.load()) return false;

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return true;
    };

    if (!reopen_and_settle(true)) return;

    std::vector<uint8_t> frame;
    frame.reserve(1024);

    uint8_t decoded[4096];
    uint8_t buf[512];

    auto t0 = std::chrono::steady_clock::now();
    uint64_t frames_in_window = 0;
    auto hz_window_start = t0;

    uint64_t bytes_in_window = 0;

    auto last_good_frame = std::chrono::steady_clock::now();

    auto reset_parser = [&]() {
        frame.clear();
        frames_in_window = 0;
        hz_window_start = std::chrono::steady_clock::now();
        bytes_in_window = 0;
        last_good_frame = std::chrono::steady_clock::now();
    };

    while (run_.load()) {
        if (suspended_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // ~1 Hz stats window. The original only re-evaluated the window right
        // after a good frame (so rx_hz froze when the link went quiet); here we
        // evaluate it on every loop iteration so onStats keeps ticking at ~1 Hz
        // even with no traffic. The window math itself is unchanged.
        {
            auto now = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(now - hz_window_start).count();
            if (dt >= 1.0f) {
                st_.rxHz = frames_in_window / dt;
                st_.rxBytesPerSec = bytes_in_window / dt;
                frames_in_window = 0;
                hz_window_start = now;
                bytes_in_window = 0;
                if (onStats) onStats(st_);
            }
        }

        {
            auto now = std::chrono::steady_clock::now();
            if (now - last_good_frame > std::chrono::seconds(2)) {
                if (!reopen_and_settle(false)) break;
                reset_parser();
            }
        }

        int n = 0;
        {
            std::lock_guard<std::mutex> lk(serial_mtx_);
            n = stream.read(buf, (int)sizeof(buf));
        }
        if (n == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        bytes_in_window += (uint64_t)n;

        for (int i = 0; i < n; ++i) {
            uint8_t b = buf[i];

            if (b == 0x00) {
                if (!frame.empty()) {
                    // NOTE (port fix): the original catch-all bumped
                    // reject_payload_parse for *every* failed frame, including
                    // crc/hdr/len failures that had already incremented their
                    // own counter (double-counting), and it never counted
                    // bad_frames for COBS-decode or payload-parse failures.
                    // Here each failed frame increments exactly one reject
                    // counter and bad_frames exactly once.
                    bool reject_counted = false;
                    try {
                        size_t dec_len = cobs_decode(frame.data(), frame.size(), decoded, sizeof(decoded));

                        if (dec_len < sizeof(TelemetryHeader) + 2) {
                            st_.rejectLen++; st_.badFrames++; reject_counted = true;
                            throw std::runtime_error("short");
                        }

                        uint16_t rx_crc = (uint16_t)decoded[dec_len - 2] |
                                          ((uint16_t)decoded[dec_len - 1] << 8);
                        uint16_t calc = crc16_ccitt(decoded, dec_len - 2);
                        if (rx_crc != calc) {
                            st_.rejectCrc++; st_.badFrames++; reject_counted = true;
                            throw std::runtime_error("crc");
                        }

                        TelemetryHeader h{};
                        std::memcpy(&h, decoded, sizeof(h));
                        if (h.magic != MAGIC || h.version != VERSION ||
                            (h.msg_type != MSG_DATA && h.msg_type != MSG_DEFINE)) {
                            st_.rejectHdr++; st_.badFrames++; reject_counted = true;
                            throw std::runtime_error("hdr");
                        }

                        if (sizeof(TelemetryHeader) + h.payload_len + 2 != dec_len) {
                            st_.rejectLen++; st_.badFrames++; reject_counted = true;
                            throw std::runtime_error("len");
                        }

                        auto now = std::chrono::steady_clock::now();
                        float tsec = std::chrono::duration<float>(now - t0).count();

                        const uint8_t* payload = decoded + sizeof(TelemetryHeader);
                        const size_t plen = h.payload_len;

                        if (h.msg_type == MSG_DEFINE) {
                            parseDefinePayload(payload, plen);
                        } else {
                            parseDataPayload(payload, plen, tsec);
                        }

                        // Drop partial strings that haven't seen a fragment in 2s
                        for (auto it = partial_str_.begin(); it != partial_str_.end(); ) {
                            if (tsec - it->second.last_tsec > 2.0f) {
                                it = partial_str_.erase(it);
                            } else {
                                ++it;
                            }
                        }

                        st_.lastSeq = h.seq;
                        st_.goodFrames++;

                        last_good_frame = std::chrono::steady_clock::now();
                        frames_in_window++;
                    } catch (...) {
                        // keep going; bad frames happen during reconnect/startup.
                        // COBS-decode and payload-parse failures land here.
                        if (!reject_counted) {
                            st_.rejectPayloadParse++;
                            st_.badFrames++;
                        }
                    }

                    frame.clear();
                }
            } else {
                if (frame.size() < 4096) frame.push_back(b);
                else frame.clear(); // oversize -> drop
            }
        }
    }
}

} // namespace NodeGUI::runtime
