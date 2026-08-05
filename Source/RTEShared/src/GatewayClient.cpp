#include "GatewayClient.h"

#include "Sha256.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

using json = nlohmann::json;

namespace rte::runtime {
namespace {

std::string NormalizeUrl(std::string url) {
    while (!url.empty() && url.back() == '/') url.pop_back();
    if (url.find("://") == std::string::npos) url = "http://" + url;
    return url;
}

std::string ResponseError(const httplib::Result& result) {
    if (!result) return "gateway request failed: " + httplib::to_string(result.error());
    const json body = json::parse(result->body, nullptr, false);
    if (body.is_object() && body.contains("error")) {
        const auto& error = body["error"];
        return error.value("message", result->body);
    }
    return result->body.empty() ? ("HTTP " + std::to_string(result->status))
                                : result->body;
}

GatewayClientStats ParseStats(const json& value) {
    GatewayClientStats result;
    result.rxHz = value.value("rx_hz", 0.0f);
    result.rxBytesPerSec = value.value("rx_bytes_per_sec", 0.0f);
    result.goodFrames = value.value("good_frames", uint64_t{0});
    result.badFrames = value.value("bad_frames", uint64_t{0});
    result.rejectCrc = value.value("reject_crc", uint64_t{0});
    result.rejectHdr = value.value("reject_header", uint64_t{0});
    result.rejectLen = value.value("reject_length", uint64_t{0});
    result.rejectPayloadParse = value.value("reject_payload_parse", uint64_t{0});
    result.rejectUnknownId = value.value("reject_unknown_id", uint64_t{0});
    result.lastSeq = value.value("last_sequence", uint32_t{0});
    result.suspended = value.value("suspended", false);
    return result;
}

}  // namespace

GatewayClient::GatewayClient(std::string baseUrl)
    : baseUrl_(NormalizeUrl(std::move(baseUrl))) {}

GatewayClient::~GatewayClient() {
    stopEvents();
    bool owned = false;
    {
        std::lock_guard lock(mutex_);
        owned = ownsLease_;
    }
    if (owned) releaseLease();
}

void GatewayClient::setBaseUrl(std::string baseUrl) {
    const bool restart = eventsRun_.load();
    if (restart) stopEvents();
    releaseLease();
    {
        std::lock_guard lock(mutex_);
        baseUrl_ = NormalizeUrl(std::move(baseUrl));
        lastEventId_ = 0;
        lastFlashJobId_.clear();
    }
    if (restart) startEvents();
}

std::string GatewayClient::baseUrl() const {
    std::lock_guard lock(mutex_);
    return baseUrl_;
}

bool GatewayClient::startEvents() {
    if (eventsRun_.exchange(true)) return true;
    eventThread_ = std::thread(&GatewayClient::eventLoop, this);
    renewalThread_ = std::thread(&GatewayClient::renewalLoop, this);
    return true;
}

void GatewayClient::stopEvents() {
    eventsRun_.store(false);
    {
        std::lock_guard lock(activeClientMutex_);
        if (activeHttpClient_) static_cast<httplib::Client*>(activeHttpClient_)->stop();
    }
    if (eventThread_.joinable()) eventThread_.join();
    if (renewalThread_.joinable()) renewalThread_.join();
}

bool GatewayClient::acquireLease(const std::string& owner, std::string* error) {
    httplib::Client client(baseUrl());
    client.set_connection_timeout(2, 0);
    const auto result = client.Post("/api/v1/control/lease",
                                    json({{"owner", owner}}).dump(),
                                    "application/json");
    if (!result || result->status != 201) {
        if (error) *error = ResponseError(result);
        return false;
    }
    const json body = json::parse(result->body, nullptr, false);
    if (!body.is_object() || !body.contains("id")) {
        if (error) *error = "gateway returned an invalid lease";
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        leaseId_ = body["id"].get<std::string>();
        leaseOwner_ = body.value("owner", owner);
        ownsLease_ = true;
    }
    if (onLease) onLease(true, leaseOwner());
    return true;
}

bool GatewayClient::releaseLease(std::string* error) {
    std::string id;
    {
        std::lock_guard lock(mutex_);
        id = leaseId_;
    }
    if (id.empty()) return true;
    httplib::Client client(baseUrl());
    const auto result = client.Delete("/api/v1/control/lease/" + id);
    const bool success = result && (result->status == 200 || result->status == 404);
    if (!success && error) *error = ResponseError(result);
    clearLease();
    return success;
}

bool GatewayClient::hasLease() const {
    std::lock_guard lock(mutex_);
    return !leaseId_.empty();
}

std::string GatewayClient::leaseId() const {
    std::lock_guard lock(mutex_);
    return leaseId_;
}

std::string GatewayClient::leaseOwner() const {
    std::lock_guard lock(mutex_);
    return leaseOwner_;
}

void GatewayClient::useLease(std::string id, std::string owner) {
    std::lock_guard lock(mutex_);
    leaseId_ = std::move(id);
    leaseOwner_ = std::move(owner);
    ownsLease_ = false;
}

bool GatewayClient::sendCommand(const std::string& command,
                                std::vector<std::string>* output,
                                std::string* error,
                                int waitMs) {
    const std::string id = leaseId();
    if (id.empty()) {
        if (error) *error = "operator control has not been acquired";
        return false;
    }
    httplib::Client client(baseUrl());
    httplib::Headers headers{{"X-RTE-Lease-ID", id}};
    const auto result = client.Post("/api/v1/commands", headers,
                                    json({{"command", command}, {"wait_ms", waitMs}}).dump(),
                                    "application/json");
    if (!result || result->status != 200) {
        if (error) *error = ResponseError(result);
        if (result && result->status == 409) clearLease();
        return false;
    }
    if (output) {
        const json body = json::parse(result->body, nullptr, false);
        for (const auto& line : body.value("lines", json::array())) {
            output->push_back(line.value("text", ""));
        }
    }
    return true;
}

bool GatewayClient::flashFile(const std::string& path,
                              bool autoGpio,
                              std::string* jobId,
                              std::string* error) {
    const std::string id = leaseId();
    if (id.empty()) {
        if (error) *error = "operator control has not been acquired";
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "could not open firmware: " + path;
        return false;
    }
    std::string body((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
    httplib::Headers headers{{"X-RTE-Lease-ID", id},
                             {"X-RTE-SHA256", Sha256Hex(body.data(), body.size())},
                             {"X-RTE-Filename", std::filesystem::path(path).filename().string()},
                             {"X-RTE-Auto-GPIO", autoGpio ? "true" : "false"}};
    httplib::Client client(baseUrl());
    client.set_write_timeout(30, 0);
    const auto result = client.Post("/api/v1/flash/jobs", headers, body,
                                    "application/octet-stream");
    if (!result || result->status != 202) {
        if (error) *error = ResponseError(result);
        if (result && result->status == 409
            && ResponseError(result).find("lease") != std::string::npos) clearLease();
        return false;
    }
    const json response = json::parse(result->body, nullptr, false);
    const std::string newId = response.value("job_id", "");
    {
        std::lock_guard lock(mutex_);
        lastFlashJobId_ = newId;
    }
    if (jobId) *jobId = newId;
    return !newId.empty();
}

GatewayFlashStatus GatewayClient::flashStatus(const std::string& requestedJobId) const {
    std::string id = requestedJobId;
    if (id.empty()) {
        std::lock_guard lock(mutex_);
        id = lastFlashJobId_;
    }
    GatewayFlashStatus status;
    if (id.empty()) {
        status.lastError = "no flash job has been submitted";
        return status;
    }
    httplib::Client client(baseUrl());
    client.set_connection_timeout(2, 0);
    const auto result = client.Get("/api/v1/flash/jobs/" + id);
    if (!result || result->status != 200) {
        status.lastError = ResponseError(result);
        return status;
    }
    const json body = json::parse(result->body, nullptr, false);
    status.reachable = true;
    status.jobId = body.value("job_id", id);
    status.state = body.value("state", "Unknown");
    status.busy = body.value("busy", false);
    status.progress = body.value("progress", -1);
    status.lastError = body.value("last_error", "");
    status.log = body.value("log", std::vector<std::string>{});
    return status;
}

std::string GatewayClient::infoJson(std::string* error) const {
    httplib::Client client(baseUrl());
    const auto result = client.Get("/api/v1/info");
    if (!result || result->status != 200) {
        if (error) *error = ResponseError(result);
        return {};
    }
    return result->body;
}

std::string GatewayClient::stateJson(std::string* error) const {
    httplib::Client client(baseUrl());
    const auto result = client.Get("/api/v1/state");
    if (!result || result->status != 200) {
        if (error) *error = ResponseError(result);
        return {};
    }
    return result->body;
}

std::string GatewayClient::consoleJson(uint64_t since,
                                       std::size_t limit,
                                       std::string* error) const {
    httplib::Client client(baseUrl());
    const auto result = client.Get("/api/v1/console?since=" + std::to_string(since)
                                   + "&limit=" + std::to_string(limit));
    if (!result || result->status != 200) {
        if (error) *error = ResponseError(result);
        return {};
    }
    return result->body;
}

void GatewayClient::eventLoop() {
    unsigned retry = 0;
    while (eventsRun_.load()) {
        httplib::Client client(baseUrl());
        client.set_read_timeout(30, 0);
        {
            std::lock_guard lock(activeClientMutex_);
            activeHttpClient_ = &client;
        }
        httplib::Headers headers;
        {
            std::lock_guard lock(mutex_);
            if (lastEventId_ != 0) {
                headers.emplace("Last-Event-ID", std::to_string(lastEventId_));
            }
        }
        std::string buffer;
        const auto result = client.Get(
            "/api/v1/events", headers,
            [this, &buffer](const char* data, std::size_t size) {
                if (!eventsRun_.load()) return false;
                buffer.append(data, size);
                for (;;) {
                    const std::size_t end = buffer.find("\n\n");
                    if (end == std::string::npos) break;
                    const std::string message = buffer.substr(0, end);
                    buffer.erase(0, end + 2);
                    std::string type;
                    std::string payload;
                    std::istringstream lines(message);
                    for (std::string line; std::getline(lines, line);) {
                        if (line.rfind("id: ", 0) == 0) {
                            const std::string id = line.substr(4);
                            try {
                                std::lock_guard lock(mutex_);
                                lastEventId_ = std::stoull(id);
                            } catch (...) {}
                        } else if (line.rfind("event: ", 0) == 0) {
                            type = line.substr(7);
                        } else if (line.rfind("data: ", 0) == 0) {
                            payload += line.substr(6);
                        }
                    }
                    if (!type.empty() && !payload.empty()) dispatchEvent(type, payload);
                }
                return true;
            });
        {
            std::lock_guard lock(activeClientMutex_);
            activeHttpClient_ = nullptr;
        }
        if (!eventsRun_.load()) break;
        if (onConnection) onConnection(false, ResponseError(result));
        const auto delay = std::chrono::milliseconds(
            std::min(10000U, 500U << std::min(retry++, 4U)));
        std::this_thread::sleep_for(delay);
    }
}

void GatewayClient::renewalLoop() {
    while (eventsRun_.load()) {
        for (int i = 0; i < 50 && eventsRun_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!eventsRun_.load()) break;
        const std::string id = leaseId();
        if (id.empty()) continue;
        httplib::Client client(baseUrl());
        const auto result = client.Put("/api/v1/control/lease/" + id,
                                       "", "application/json");
        if (!result || result->status != 200) clearLease();
    }
}

void GatewayClient::dispatchEvent(const std::string& type, const std::string& data) {
    const json body = json::parse(data, nullptr, false);
    if (!body.is_object() && !body.is_array()) return;
    if (type == "snapshot") {
        const json latest = body.value("latest", json::object());
        for (auto it = latest.begin(); it != latest.end(); ++it) {
            if (onF32) onF32(it.key(), it.value().get<float>(), 0.0f);
        }
        const json strings = body.value("latest_strings", json::object());
        for (auto it = strings.begin(); it != strings.end(); ++it) {
            if (onString) onString(it.key(), it.value().get<std::string>());
        }
        if (body.contains("stats") && onStats) onStats(ParseStats(body["stats"]));
        if (body.contains("lease") && onLease) {
            const auto& lease = body["lease"];
            const bool globallyHeld = lease.value("held", false);
            const std::string id = lease.value("id", "");
            const std::string localId = leaseId();
            onLease(globallyHeld && !localId.empty() && id == localId,
                    globallyHeld ? lease.value("owner", "") : "");
        }
        if (onConnection) onConnection(true, {});
    } else if (type == "telemetry") {
        for (const auto& sample : body.value("samples", json::array())) {
            if (onF32) onF32(sample.value("signal", ""), sample.value("value", 0.0f),
                             sample.value("t", 0.0f));
        }
        for (const auto& sample : body.value("strings", json::array())) {
            if (onString) onString(sample.value("signal", ""),
                                   sample.value("value", ""));
        }
    } else if (type == "console") {
        for (const auto& line : body) {
            if (onConsole) onConsole(line.value("seq", uint64_t{0}),
                                     line.value("text", ""));
        }
    } else if (type == "stats" && onStats) {
        onStats(ParseStats(body));
    } else if (type == "lease" && onLease) {
        const bool globallyHeld = body.value("held", false);
        const std::string id = body.value("id", "");
        const bool owned = globallyHeld && !leaseId().empty() && id == leaseId();
        onLease(owned, globallyHeld ? body.value("owner", "") : "");
    }
}

void GatewayClient::clearLease() {
    std::string owner;
    {
        std::lock_guard lock(mutex_);
        owner = leaseOwner_;
        leaseId_.clear();
        leaseOwner_.clear();
        ownsLease_ = false;
    }
    if (onLease) onLease(false, owner);
}

}  // namespace rte::runtime
