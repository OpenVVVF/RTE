#include "HttpApiServer.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace NodeGUI::runtime {

namespace {

typedef int socket_t;
const socket_t INVALID_SOCKET_VAL = -1;

// Minimal JSON string escaping (ported from the old client's json_escape.h).
std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", c);
                    out += esc;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

std::string currentThreadIdStr() {
    std::ostringstream oss;
    oss << std::this_thread::get_id();
    return oss.str();
}

std::string makeTempFwPath() {
    std::error_code ec;
    fs::path tmp = fs::temp_directory_path(ec);
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    std::string name = "rte_fw_" + std::to_string(ms) + "_" + currentThreadIdStr() + ".bin";
    return (tmp / name).string();
}

void sendResponse(socket_t client, int code, const std::string& body) {
    const char* status = "OK";
    if (code == 202) status = "Accepted";
    else if (code == 400) status = "Bad Request";
    else if (code == 405) status = "Method Not Allowed";
    else if (code == 503) status = "Service Unavailable";
    else if (code == 500) status = "Internal Server Error";

    std::ostringstream resp;
    resp << "HTTP/1.1 " << code << " " << status << "\r\n";
    resp << "Content-Type: application/json\r\n";
    resp << "Content-Length: " << body.size() << "\r\n";
    resp << "Connection: close\r\n";
    resp << "\r\n";
    resp << body;

    std::string s = resp.str();
    send(client, s.c_str(), s.size(), MSG_NOSIGNAL);
}

// Build {"lines":[{"seq":N,"text":"..."}...],"latest_seq":M} from the console
// buffer: lines with seq > since, capped to the last max_lines of them.
std::string consoleToJson(const TelemetrySnapshot& st, uint64_t since, size_t max_lines) {
    std::vector<const ConsoleLine*> sel;
    for (const auto& ln : st.console) {
        if (ln.seq > since) sel.push_back(&ln);
    }
    if (sel.size() > max_lines) sel.erase(sel.begin(), sel.end() - max_lines);

    std::string resp = "{\"lines\":[";
    bool first = true;
    for (const ConsoleLine* ln : sel) {
        resp += first ? "{\"seq\":" : ",{\"seq\":";
        resp += std::to_string(ln->seq) + ",\"text\":\"" + jsonEscape(ln->text) + "\"}";
        first = false;
    }
    resp += "],\"latest_seq\":"
        + std::to_string(st.console.empty() ? 0 : st.console.back().seq) + "}";
    return resp;
}

// Accept {"cmd":"..."} (naive extraction) or a raw command line as the body.
std::string extractCommand(const std::string& body) {
    size_t start = body.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    if (body[start] != '{') {
        size_t end = body.find_last_not_of(" \t\r\n");
        return body.substr(start, end - start + 1);
    }
    size_t k = body.find("\"cmd\"");
    if (k == std::string::npos) return "";
    k = body.find(':', k + 5);
    if (k == std::string::npos) return "";
    k = body.find('"', k + 1);
    if (k == std::string::npos) return "";
    std::string out;
    for (size_t i = k + 1; i < body.size(); ++i) {
        char c = body[i];
        if (c == '\\' && i + 1 < body.size()) {
            char e = body[++i];
            switch (e) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                default: out += e; break;
            }
        } else if (c == '"') {
            break;
        } else {
            out += c;
        }
    }
    return out;
}

}  // namespace

HttpApiServer::HttpApiServer(FirmwareUpdater& updater, TelemetryStore& store,
                             std::string port)
    : updater_(updater), store_(store), port_(std::move(port)) {}

HttpApiServer::~HttpApiServer() {
    stop();
}

bool HttpApiServer::start() {
    return start(port_);
}

bool HttpApiServer::start(const std::string& port) {
    if (running_.load()) return true;

    port_ = port;
    socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET_VAL) return false;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    int p = 0;
    try {
        p = std::stoi(port_);
    } catch (...) {
        close(fd);
        return false;
    }
    if (p <= 0 || p > 65535) {
        close(fd);
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)p);

    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return false;
    }

    if (listen(fd, 4) != 0) {
        close(fd);
        return false;
    }

    // Read back actual port in case port_ was "0".
    socklen_t addrlen = sizeof(addr);
    if (getsockname(fd, (sockaddr*)&addr, &addrlen) == 0) {
        actual_port_.store((int)ntohs(addr.sin_port));
    } else {
        actual_port_.store(p);
    }

    listen_fd_ = fd;
    stop_.store(false);
    running_.store(true);
    start_time_ = std::chrono::steady_clock::now();
    thread_ = std::thread(&HttpApiServer::threadMain, this);
    return true;
}

void HttpApiServer::stop() {
    stop_.store(true);
    if (listen_fd_ != INVALID_SOCKET_VAL) {
        // Wake the blocking accept() in the server thread, then close.
        shutdown(listen_fd_, SHUT_RDWR);
        close(listen_fd_);
        listen_fd_ = INVALID_SOCKET_VAL;
    }
    if (thread_.joinable()) thread_.join();
    running_.store(false);
}

bool HttpApiServer::restart(const std::string& port) {
    stop();
    return start(port);
}

void HttpApiServer::setDevicePort(const std::string& port) {
    {
        std::lock_guard lock(config_mtx_);
        device_port_ = port;
    }
    updater_.setCurrentPort(port);
}

void HttpApiServer::setCommandHandler(std::function<bool(const std::string&)> handler) {
    std::lock_guard lock(config_mtx_);
    command_handler_ = std::move(handler);
}

void HttpApiServer::setAppName(const std::string& name) {
    std::lock_guard lock(config_mtx_);
    app_name_ = name;
}

void HttpApiServer::threadMain() {
    while (!stop_.load()) {
        sockaddr_in client_addr{};
        socklen_t addrlen = sizeof(client_addr);
        socket_t client = accept(listen_fd_, (sockaddr*)&client_addr, &addrlen);
        if (client == INVALID_SOCKET_VAL) {
            if (stop_.load()) break;  // listen socket shut down by stop()
            continue;                 // EINTR and friends: retry
        }

        // Set receive timeout so a misbehaving client cannot hang us forever.
        timeval tv{};
        tv.tv_sec = 5;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // Read request into buffer.
        std::string buf;
        buf.reserve(4096);
        char tmp[4096];
        bool header_complete = false;
        size_t header_end = 0;
        while (!stop_.load()) {
            int n = (int)recv(client, tmp, sizeof(tmp), 0);
            if (n <= 0) break;
            buf.append(tmp, (size_t)n);

            if (!header_complete) {
                header_end = buf.find("\r\n\r\n");
                if (header_end != std::string::npos) {
                    header_complete = true;
                    header_end += 4;
                }
            }

            if (header_complete) break;
        }

        if (!header_complete) {
            sendResponse(client, 400, "{\"error\":\"incomplete request\"}");
            close(client);
            continue;
        }

        // Parse request line.
        size_t line_end = buf.find("\r\n");
        std::string request_line = buf.substr(0, line_end);
        std::istringstream iss(request_line);
        std::string method, raw_path, version;
        iss >> method >> raw_path >> version;

        // Split query string into path + key/value map.
        std::string path = raw_path;
        std::map<std::string, std::string> query;
        if (size_t q = raw_path.find('?'); q != std::string::npos) {
            path = raw_path.substr(0, q);
            std::string qs = raw_path.substr(q + 1);
            size_t p = 0;
            while (p <= qs.size()) {
                size_t amp = qs.find('&', p);
                std::string pair = qs.substr(p, amp == std::string::npos ? amp : amp - p);
                size_t eq = pair.find('=');
                if (eq != std::string::npos) query[pair.substr(0, eq)] = pair.substr(eq + 1);
                else if (!pair.empty()) query[pair] = "";
                if (amp == std::string::npos) break;
                p = amp + 1;
            }
        }
        auto queryUint = [&](const char* key, uint64_t dflt) -> uint64_t {
            auto it = query.find(key);
            if (it == query.end() || it->second.empty()) return dflt;
            try { return std::stoull(it->second); } catch (...) { return dflt; }
        };

        if (method == "GET" && path == "/flash/status") {
            FlashStatus st = updater_.status();
            std::string resp = std::string("{\"state\":\"")
                + FirmwareUpdater::stateString(st.state)
                + "\",\"busy\":" + (st.busy ? "true" : "false")
                + ",\"last_error\":\"" + jsonEscape(st.last_error) + "\"";
            // Include the tail of the updater log for remote debugging.
            const size_t kLogTail = 30;
            size_t start = st.log.size() > kLogTail ? st.log.size() - kLogTail : 0;
            resp += ",\"log\":[";
            for (size_t i = start; i < st.log.size(); ++i) {
                resp += (i == start ? "\"" : ",\"") + jsonEscape(st.log[i]) + "\"";
            }
            resp += "]}";
            sendResponse(client, 200, resp);
            close(client);
            continue;
        }

        if (method == "GET" && path == "/api/info") {
            double uptime = 0.0;
            if (start_time_.time_since_epoch().count() != 0) {
                uptime = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - start_time_).count();
            }
            std::string app_name, device_port;
            {
                std::lock_guard lock(config_mtx_);
                app_name = app_name_;
                device_port = device_port_;
            }
            std::string resp = std::string()
                + "{\"app\":\"" + jsonEscape(app_name) + "\""
                + ",\"device_port\":\"" + jsonEscape(device_port) + "\""
                + ",\"http_port\":" + std::to_string(actualPort())
                + ",\"uptime_s\":" + std::to_string(uptime)
                + ",\"log_dir\":\"\""  // disk logging was dropped; key kept for compatibility
                + ",\"endpoints\":["
                  "\"GET /api/info\""
                  ",\"GET /api/telemetry\""
                  ",\"GET /api/console?lines=N&since=SEQ\""
                  ",\"POST /api/command?wait_ms=N\""
                  ",\"POST /flash\""
                  ",\"GET /flash/status\""
                  "]}";
            sendResponse(client, 200, resp);
            close(client);
            continue;
        }

        if (method == "GET" && path == "/api/telemetry") {
            TelemetrySnapshot st = store_.Snapshot();
            double unix_s = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count() / 1000.0;
            std::string resp = "{\"t\":" + std::to_string(unix_s)
                + ",\"rx_hz\":" + std::to_string(st.rxHz)
                + ",\"suspended\":" + (st.suspended ? "true" : "false")
                + ",\"signals\":{";
            bool first = true;
            for (const auto& kv : st.latest) {
                resp += (first ? "\"" : ",\"") + jsonEscape(kv.first)
                    + "\":" + std::to_string(kv.second);
                first = false;
            }
            resp += "},\"strings\":{";
            first = true;
            for (const auto& kv : st.latestStr) {
                resp += (first ? "\"" : ",\"") + jsonEscape(kv.first)
                    + "\":\"" + jsonEscape(kv.second) + "\"";
                first = false;
            }
            resp += "}}";
            sendResponse(client, 200, resp);
            close(client);
            continue;
        }

        if (method == "GET" && path == "/api/console") {
            uint64_t since = queryUint("since", 0);
            uint64_t lines = queryUint("lines", 100);
            if (lines == 0) lines = 1;
            if (lines > 1000) lines = 1000;
            TelemetrySnapshot st = store_.Snapshot();
            sendResponse(client, 200, consoleToJson(st, since, (size_t)lines));
            close(client);
            continue;
        }

        bool is_post_flash = (method == "POST" && path == "/flash");
        bool is_post_command = (method == "POST" && path == "/api/command");
        if (!is_post_flash && !is_post_command) {
            sendResponse(client, 405, "{\"error\":\"supported: GET /flash/status, GET /api/info, "
                                      "GET /api/telemetry, GET /api/console, POST /api/command, POST /flash\"}");
            close(client);
            continue;
        }

        // Parse headers.
        size_t pos = line_end + 2;
        size_t content_length = 0;
        bool has_content_length = false;
        while (pos < header_end - 2) {
            size_t next = buf.find("\r\n", pos);
            if (next == std::string::npos) break;
            std::string header = buf.substr(pos, next - pos);
            size_t colon = header.find(':');
            if (colon != std::string::npos) {
                std::string key = header.substr(0, colon);
                std::string value = header.substr(colon + 1);
                // trim
                size_t a = value.find_first_not_of(" \t\r\n");
                size_t b = value.find_last_not_of(" \t\r\n");
                if (a != std::string::npos) value = value.substr(a, b - a + 1);
                std::transform(key.begin(), key.end(), key.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                if (key == "content-length") {
                    try {
                        content_length = std::stoull(value);
                        has_content_length = true;
                    } catch (...) {
                        // fall through to the missing Content-Length 400 below
                    }
                }
            }
            pos = next + 2;
        }

        if (!has_content_length) {
            sendResponse(client, 400, "{\"error\":\"missing Content-Length\"}");
            close(client);
            continue;
        }

        // Read remaining body bytes.
        std::string body;
        body.reserve(content_length);
        size_t have = buf.size() - header_end;
        if (have > 0) body.append(buf.data() + header_end, have);

        while (body.size() < content_length && !stop_.load()) {
            int to_read = (int)std::min<size_t>(sizeof(tmp), content_length - body.size());
            int n = (int)recv(client, tmp, (size_t)to_read, 0);
            if (n <= 0) break;
            body.append(tmp, (size_t)n);
        }

        if (body.size() != content_length) {
            sendResponse(client, 400, "{\"error\":\"short body\"}");
            close(client);
            continue;
        }

        if (is_post_command) {
            std::string cmd = extractCommand(body);
            if (cmd.empty()) {
                sendResponse(client, 400, "{\"error\":\"empty command\"}");
                close(client);
                continue;
            }

            TelemetrySnapshot st = store_.Snapshot();
            if (st.suspended) {
                sendResponse(client, 503, "{\"error\":\"telemetry suspended (firmware update in progress)\"}");
                close(client);
                continue;
            }
            uint64_t before = st.console.empty() ? 0 : st.console.back().seq;

            std::function<bool(const std::string&)> handler;
            {
                std::lock_guard lock(config_mtx_);
                handler = command_handler_;
            }
            if (!handler || !handler(cmd)) {
                sendResponse(client, 503, "{\"error\":\"serial write failed\"}");
                close(client);
                continue;
            }

            // Collect the response: stop after 250 ms of silence once data
            // arrives, or when wait_ms elapses (long commands can be
            // followed up via GET /api/console?since=...).
            uint64_t wait_ms = queryUint("wait_ms", 2000);
            if (wait_ms > 30000) wait_ms = 30000;
            auto deadline = std::chrono::steady_clock::now()
                + std::chrono::milliseconds(wait_ms);
            uint64_t seen = before;
            int quiet_ms = 0;
            while (std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                st = store_.Snapshot();
                uint64_t latest = st.console.empty() ? 0 : st.console.back().seq;
                if (latest > seen) {
                    seen = latest;
                    quiet_ms = 0;
                } else if (seen > before) {
                    quiet_ms += 50;
                    if (quiet_ms >= 250) break;
                }
            }
            sendResponse(client, 200, consoleToJson(st, before, 500));
            close(client);
            continue;
        }

        // Save to temp file.
        std::string tmp_path = makeTempFwPath();
        {
            std::ofstream out(tmp_path, std::ios::binary);
            if (!out) {
                sendResponse(client, 500, "{\"error\":\"cannot write temp file\"}");
                close(client);
                continue;
            }
            out.write(body.data(), (std::streamsize)body.size());
            out.close();
        }

        // Queue flash job using the device port registered with the updater.
        FlashJob job;
        job.firmware_path = tmp_path;
        job.port = updater_.currentPort();
        job.auto_gpio = true;  // default for HTTP; could be exposed later
        job.delete_after_flash = true;

        if (job.port.empty()) {
            fs::remove(tmp_path);
            sendResponse(client, 503, "{\"error\":\"telemetry port not known yet\"}");
            close(client);
            continue;
        }

        if (!updater_.queueFlash(job, false)) {
            fs::remove(tmp_path);
            sendResponse(client, 503, "{\"error\":\"already flashing\"}");
            close(client);
            continue;
        }

        std::string resp = "{\"queued\":true,\"file\":\"" + jsonEscape(tmp_path) + "\"}";
        sendResponse(client, 202, resp);
        close(client);
    }

    running_.store(false);
}

}  // namespace NodeGUI::runtime
