#include "GatewayApiServer.h"

#include "Sha256.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace rte::runtime {
namespace {

json StatsJson(const TelemetryStats& stats) {
    return {{"rx_hz", stats.rxHz},
            {"rx_bytes_per_sec", stats.rxBytesPerSec},
            {"good_frames", stats.goodFrames},
            {"bad_frames", stats.badFrames},
            {"reject_crc", stats.rejectCrc},
            {"reject_header", stats.rejectHdr},
            {"reject_length", stats.rejectLen},
            {"reject_payload_parse", stats.rejectPayloadParse},
            {"reject_unknown_id", stats.rejectUnknownId},
            {"last_sequence", stats.lastSeq},
            {"suspended", stats.suspended}};
}

json SnapshotJson(const TelemetrySnapshot& snapshot) {
    json result = {{"latest", snapshot.latest},
                   {"latest_strings", snapshot.latestStr},
                   {"stats", StatsJson(TelemetryStats{
                       snapshot.rxHz, snapshot.rxBytesPerSec,
                       snapshot.goodFrames, snapshot.badFrames,
                       snapshot.rejectCrc, snapshot.rejectHdr,
                       snapshot.rejectLen, snapshot.rejectPayloadParse,
                       snapshot.rejectUnknownId, snapshot.lastSeq,
                       snapshot.suspended})}};
    return result;
}

void JsonResponse(httplib::Response& response, int status, const json& body) {
    response.status = status;
    response.set_content(body.dump(), "application/json");
    response.set_header("Cache-Control", "no-store");
}

void ErrorResponse(httplib::Response& response,
                   int status,
                   std::string code,
                   std::string message,
                   json details = nullptr) {
    json error = {{"code", std::move(code)}, {"message", std::move(message)}};
    if (!details.is_null()) error["details"] = std::move(details);
    JsonResponse(response, status, {{"error", std::move(error)}});
}

std::string RandomId() {
    std::array<unsigned char, 16> bytes{};
    std::random_device random;
    for (auto& byte : bytes) byte = static_cast<unsigned char>(random());
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out << '-';
        out << std::setw(2) << static_cast<unsigned>(bytes[i]);
    }
    return out.str();
}

uint64_t UnsignedParam(const httplib::Request& request,
                       const char* name,
                       uint64_t fallback) {
    if (!request.has_param(name)) return fallback;
    uint64_t value = fallback;
    const auto& text = request.get_param_value(name);
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} && end == text.data() + text.size() ? value : fallback;
}

std::string SseMessage(uint64_t id, const char* type, const json& data) {
    std::string result = "id: " + std::to_string(id) + "\nevent: " + type
                         + "\ndata: " + data.dump() + "\n\n";
    return result;
}

}  // namespace

struct GatewayApiServer::Impl {
    FirmwareUpdater& updater;
    TelemetryStore& store;
    GatewayApiOptions options;
    httplib::Server server;
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<int> actualPort{0};
    std::atomic<std::size_t> activeStreams{0};
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

    mutable std::mutex configMutex;
    std::string devicePort;
    std::string protocol = "legacy";
    std::function<bool(const std::string&)> commandHandler;

    mutable std::mutex leaseMutex;
    std::string leaseId;
    std::string leaseOwner;
    std::chrono::steady_clock::time_point leaseExpiry{};

    mutable std::mutex flashMutex;
    std::string flashJobId;
    std::string flashFilename;
    std::string flashSha256;

    Impl(FirmwareUpdater& updaterIn,
         TelemetryStore& storeIn,
         GatewayApiOptions optionsIn)
        : updater(updaterIn), store(storeIn), options(std::move(optionsIn)) {
        server.set_payload_max_length(options.maxFirmwareBytes);
        server.new_task_queue = [count = options.workerThreads] {
            return new httplib::ThreadPool(std::max<std::size_t>(count, 4));
        };
        RegisterRoutes();
    }

    void ExpireLeaseLocked() const {
        auto* self = const_cast<Impl*>(this);
        if (!self->leaseId.empty()
            && std::chrono::steady_clock::now() >= self->leaseExpiry) {
            self->leaseId.clear();
            self->leaseOwner.clear();
        }
    }

    json LeaseJson() const {
        std::lock_guard lock(leaseMutex);
        ExpireLeaseLocked();
        if (leaseId.empty()) return {{"held", false}};
        const auto remaining = std::max<int64_t>(
            0, std::chrono::duration_cast<std::chrono::milliseconds>(
                   leaseExpiry - std::chrono::steady_clock::now()).count());
        return {{"held", true},
                {"id", leaseId},
                {"owner", leaseOwner},
                {"ttl_ms", remaining}};
    }

    bool HasLease(const httplib::Request& request) const {
        const std::string supplied = request.get_header_value("X-RTE-Lease-ID");
        std::lock_guard lock(leaseMutex);
        ExpireLeaseLocked();
        return !supplied.empty() && supplied == leaseId;
    }

    json FlashJson() const {
        const FlashStatus status = updater.status();
        std::lock_guard lock(flashMutex);
        return {{"job_id", flashJobId},
                {"filename", flashFilename},
                {"sha256", flashSha256},
                {"state", FirmwareUpdater::stateString(status.state)},
                {"busy", status.busy},
                {"progress", status.progress},
                {"last_error", status.last_error},
                {"log", status.log}};
    }

    void RequireLease(const httplib::Request& request,
                      httplib::Response& response,
                      const std::function<void()>& action) {
        if (!HasLease(request)) {
            ErrorResponse(response, 409, "lease_required",
                          "Acquire the operator lease before mutating the device",
                          LeaseJson());
            return;
        }
        action();
    }

    void RegisterRoutes() {
        server.Get("/api/v1/health", [this](const auto&, auto& response) {
            const auto stats = store.GetStatsLine();
            JsonResponse(response, 200,
                         {{"ok", !stats.suspended},
                          {"serial_suspended", stats.suspended},
                          {"flash_busy", updater.status().busy}});
        });

        server.Get("/api/v1/info", [this](const auto&, auto& response) {
            std::lock_guard lock(configMutex);
            const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - started).count();
            JsonResponse(response, 200,
                         {{"app", "rte-gateway"},
                          {"api_version", "v1"},
                          {"device_port", devicePort},
                          {"protocol", protocol},
                          {"http_port", actualPort.load()},
                          {"uptime_seconds", uptime}});
        });

        server.Get("/api/v1/state", [this](const auto&, auto& response) {
            json body = SnapshotJson(store.Snapshot());
            body["event_sequence"] = store.LatestEventSeq();
            body["lease"] = LeaseJson();
            body["flash"] = FlashJson();
            JsonResponse(response, 200, body);
        });

        server.Get("/api/v1/telemetry/history", [this](const auto& request, auto& response) {
            const auto snapshot = store.Snapshot();
            const uint64_t limit = std::clamp<uint64_t>(
                UnsignedParam(request, "limit", 2000), 1, store.MaxSamples());
            const std::string signal = request.has_param("signal")
                                           ? request.get_param_value("signal") : "";
            json signals = json::object();
            for (const auto& [name, history] : snapshot.hist) {
                if (!signal.empty() && name != signal) continue;
                const std::size_t begin = history.t.size() > limit
                                              ? history.t.size() - limit : 0;
                json samples = json::array();
                for (std::size_t i = begin; i < history.t.size(); ++i) {
                    samples.push_back({{"t", history.t[i]}, {"value", history.y[i]}});
                }
                signals[name] = std::move(samples);
            }
            JsonResponse(response, 200, {{"signals", std::move(signals)}});
        });

        auto consoleHandler = [this](const auto& request, auto& response) {
            const uint64_t since = UnsignedParam(request, "since", 0);
            const uint64_t limit = std::clamp<uint64_t>(
                UnsignedParam(request, "limit", 200), 1,
                store.ConsoleCapLines());
            auto lines = store.ConsoleSince(since);
            if (lines.size() > limit) lines.erase(lines.begin(), lines.end() - limit);
            json result = json::array();
            for (const auto& line : lines) {
                result.push_back({{"seq", line.seq}, {"text", line.text}});
            }
            JsonResponse(response, 200,
                         {{"lines", std::move(result)},
                          {"latest_seq", store.LatestConsoleSeq()}});
        };
        server.Get("/api/v1/console", consoleHandler);

        server.Get("/api/v1/control/lease", [this](const auto&, auto& response) {
            JsonResponse(response, 200, LeaseJson());
        });
        server.Post("/api/v1/control/lease", [this](const auto& request, auto& response) {
            const json body = json::parse(request.body, nullptr, false);
            const std::string owner = body.is_object()
                ? body.value("owner", std::string("anonymous")) : "anonymous";
            std::lock_guard lock(leaseMutex);
            ExpireLeaseLocked();
            if (!leaseId.empty()) {
                ErrorResponse(response, 409, "lease_held",
                              "Another client currently controls the device",
                              {{"owner", leaseOwner}});
                return;
            }
            leaseId = RandomId();
            leaseOwner = owner.empty() ? "anonymous" : owner;
            leaseExpiry = std::chrono::steady_clock::now() + options.leaseTtl;
            JsonResponse(response, 201,
                         {{"id", leaseId}, {"owner", leaseOwner},
                          {"ttl_ms", options.leaseTtl.count() * 1000}});
        });
        server.Put(R"(/api/v1/control/lease/([0-9a-f-]+))",
                   [this](const auto& request, auto& response) {
            std::lock_guard lock(leaseMutex);
            ExpireLeaseLocked();
            if (leaseId.empty() || request.matches[1] != leaseId) {
                ErrorResponse(response, 404, "lease_not_found", "The lease no longer exists");
                return;
            }
            leaseExpiry = std::chrono::steady_clock::now() + options.leaseTtl;
            JsonResponse(response, 200,
                         {{"id", leaseId}, {"owner", leaseOwner},
                          {"ttl_ms", options.leaseTtl.count() * 1000}});
        });
        server.Delete(R"(/api/v1/control/lease/([0-9a-f-]+))",
                      [this](const auto& request, auto& response) {
            std::lock_guard lock(leaseMutex);
            ExpireLeaseLocked();
            if (leaseId.empty() || request.matches[1] != leaseId) {
                ErrorResponse(response, 404, "lease_not_found", "The lease no longer exists");
                return;
            }
            leaseId.clear();
            leaseOwner.clear();
            JsonResponse(response, 200, {{"released", true}});
        });

        server.Post("/api/v1/commands", [this](const auto& request, auto& response) {
            RequireLease(request, response, [&] {
                const json body = json::parse(request.body, nullptr, false);
                if (!body.is_object() || !body.contains("command")
                    || !body["command"].is_string()) {
                    ErrorResponse(response, 400, "invalid_command",
                                  "Expected a JSON string field named command");
                    return;
                }
                const std::string command = body["command"].get<std::string>();
                std::function<bool(const std::string&)> handler;
                {
                    std::lock_guard lock(configMutex);
                    handler = commandHandler;
                }
                const uint64_t before = store.LatestConsoleSeq();
                const bool sent = handler && handler(command);
                store.AddCommand(command, "api", sent);
                if (!sent) {
                    ErrorResponse(response, 503, "command_failed",
                                  "The command could not be written to the device");
                    return;
                }
                const int waitMs = std::clamp(body.value("wait_ms", 100), 0, 5000);
                if (waitMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
                json lines = json::array();
                for (const auto& line : store.ConsoleSince(before)) {
                    lines.push_back({{"seq", line.seq}, {"text", line.text}});
                }
                JsonResponse(response, 200, {{"sent", true}, {"lines", std::move(lines)}});
            });
        });

        server.Post("/api/v1/flash/jobs", [this](const auto& request, auto& response) {
            RequireLease(request, response, [&] {
                if (request.body.empty()) {
                    ErrorResponse(response, 400, "empty_firmware", "Firmware body is empty");
                    return;
                }
                if (updater.status().busy) {
                    ErrorResponse(response, 409, "flash_busy", "A flash job is already running");
                    return;
                }
                const std::string expected = request.get_header_value("X-RTE-SHA256");
                const std::string actual = Sha256Hex(request.body.data(), request.body.size());
                if (expected.empty() || expected != actual) {
                    ErrorResponse(response, 400, "digest_mismatch",
                                  "X-RTE-SHA256 is required and must match the firmware body",
                                  {{"actual", actual}});
                    return;
                }
                const std::string id = RandomId();
                const fs::path path = fs::temp_directory_path() / ("rte-fw-" + id + ".bin");
                std::ofstream file(path, std::ios::binary | std::ios::trunc);
                file.write(request.body.data(), static_cast<std::streamsize>(request.body.size()));
                file.close();
                if (!file) {
                    ErrorResponse(response, 500, "firmware_write_failed",
                                  "Could not stage the firmware upload");
                    return;
                }
                std::string port;
                {
                    std::lock_guard lock(configMutex);
                    port = devicePort;
                }
                const bool autoGpio = request.get_header_value("X-RTE-Auto-GPIO") != "false";
                if (!updater.queueFlash(FlashJob{path.string(), port, autoGpio, true}, false)) {
                    std::error_code ec;
                    fs::remove(path, ec);
                    ErrorResponse(response, 409, "flash_busy", "A flash job is already running");
                    return;
                }
                {
                    std::lock_guard lock(flashMutex);
                    flashJobId = id;
                    flashFilename = request.get_header_value("X-RTE-Filename");
                    flashSha256 = actual;
                }
                JsonResponse(response, 202,
                             {{"job_id", id}, {"sha256", actual},
                              {"status_url", "/api/v1/flash/jobs/" + id}});
            });
        });
        server.Get(R"(/api/v1/flash/jobs/([0-9a-f-]+))",
                   [this](const auto& request, auto& response) {
            const json status = FlashJson();
            if (status.value("job_id", "") != request.matches[1]) {
                ErrorResponse(response, 404, "flash_job_not_found", "Unknown flash job");
                return;
            }
            JsonResponse(response, 200, status);
        });

        server.Get("/api/v1/events", [this](const auto& request, auto& response) {
            if (activeStreams.fetch_add(1) >= options.maxStreams) {
                activeStreams.fetch_sub(1);
                ErrorResponse(response, 503, "stream_limit", "Too many event streams");
                return;
            }
            uint64_t last = 0;
            const std::string lastHeader = request.get_header_value("Last-Event-ID");
            if (!lastHeader.empty()) {
                std::from_chars(lastHeader.data(), lastHeader.data() + lastHeader.size(), last);
            }
            auto state = std::make_shared<uint64_t>(last);
            auto first = std::make_shared<bool>(true);
            auto lastLease = std::make_shared<std::string>();
            auto lastFlash = std::make_shared<std::string>();
            auto lastHeartbeat = std::make_shared<std::chrono::steady_clock::time_point>(
                std::chrono::steady_clock::now());
            auto lastStateCheck = std::make_shared<std::chrono::steady_clock::time_point>(
                std::chrono::steady_clock::now());
            response.set_header("Cache-Control", "no-cache");
            response.set_header("X-Accel-Buffering", "no");
            response.set_chunked_content_provider(
                "text/event-stream",
                [this, state, first, lastLease, lastFlash, lastHeartbeat,
                 lastStateCheck](std::size_t, httplib::DataSink& sink) {
                    if (!running.load() || !sink.is_writable()) return false;
                    std::string output;
                    if (*first) {
                        json snapshot = SnapshotJson(store.Snapshot());
                        snapshot["lease"] = LeaseJson();
                        snapshot["flash"] = FlashJson();
                        *lastLease = snapshot["lease"].dump();
                        *lastFlash = snapshot["flash"].dump();
                        output += SseMessage(store.LatestEventSeq(), "snapshot", snapshot);
                        if (*state == 0) *state = store.LatestEventSeq();
                        *first = false;
                    }
                    store.WaitForEvents(*state, std::chrono::milliseconds(20));
                    bool reset = false;
                    const auto events = store.EventsSince(*state, 256, reset);
                    if (reset) {
                        json snapshot = SnapshotJson(store.Snapshot());
                        snapshot["reset"] = true;
                        output += SseMessage(store.LatestEventSeq(), "snapshot", snapshot);
                        *state = store.LatestEventSeq();
                    } else if (!events.empty()) {
                        json telemetry = json::array();
                        json strings = json::array();
                        json console = json::array();
                        json stats;
                        for (const auto& event : events) {
                            *state = event.seq;
                            switch (event.kind) {
                                case TelemetryEventKind::Float:
                                    telemetry.push_back({{"signal", event.key},
                                                         {"t", event.tsec},
                                                         {"value", event.value}});
                                    break;
                                case TelemetryEventKind::String:
                                    strings.push_back({{"signal", event.key},
                                                       {"value", event.text}});
                                    break;
                                case TelemetryEventKind::Console:
                                    console.push_back({{"seq", std::stoull(event.key)},
                                                       {"text", event.text}});
                                    break;
                                case TelemetryEventKind::Stats: stats = StatsJson(event.stats); break;
                                case TelemetryEventKind::Suspended:
                                    stats = StatsJson(store.GetStatsLine()); break;
                                case TelemetryEventKind::Reset: break;
                            }
                        }
                        if (!telemetry.empty() || !strings.empty()) {
                            output += SseMessage(*state, "telemetry",
                                                 {{"samples", telemetry}, {"strings", strings}});
                        }
                        if (!console.empty()) output += SseMessage(*state, "console", console);
                        if (!stats.is_null()) output += SseMessage(*state, "stats", stats);
                    }
                    const auto now = std::chrono::steady_clock::now();
                    if (now - *lastStateCheck >= std::chrono::milliseconds(100)) {
                        const std::string lease = LeaseJson().dump();
                        if (lease != *lastLease) {
                            output += "event: lease\ndata: " + lease + "\n\n";
                            *lastLease = lease;
                        }
                        const std::string flash = FlashJson().dump();
                        if (flash != *lastFlash) {
                            output += "event: flash\ndata: " + flash + "\n\n";
                            *lastFlash = flash;
                        }
                        *lastStateCheck = now;
                    }
                    if (output.empty() && now - *lastHeartbeat >= std::chrono::seconds(15)) {
                        output = ": heartbeat\n\n";
                    }
                    if (output.empty()) return true;
                    *lastHeartbeat = now;
                    return sink.write(output.data(), output.size());
                },
                [this](bool) { activeStreams.fetch_sub(1); });
        });

        // One release of read-only route compatibility. Mutating legacy
        // routes deliberately do not bypass operator control.
        server.Get("/api/info", [this](const auto&, auto& response) {
            std::lock_guard lock(configMutex);
            JsonResponse(response, 200,
                         {{"app", "rte-gateway"}, {"api_version", "v1"},
                          {"device_port", devicePort}, {"deprecated", true}});
        });
        server.Get("/api/telemetry", [this](const auto&, auto& response) {
            json body = SnapshotJson(store.Snapshot());
            body["deprecated"] = true;
            JsonResponse(response, 200, body);
        });
        server.Get("/api/console", consoleHandler);
        server.Get("/flash/status", [this](const auto&, auto& response) {
            json body = FlashJson();
            body["deprecated"] = true;
            JsonResponse(response, 200, body);
        });
        auto gone = [](const auto&, auto& response) {
            ErrorResponse(response, 410, "legacy_mutation_removed",
                          "Use the leased /api/v1 mutation endpoint");
        };
        server.Post("/api/command", gone);
        server.Post("/flash", gone);

        server.set_error_handler([](const auto&, auto& response) {
            if (response.status == 404) {
                ErrorResponse(response, 404, "not_found", "No such gateway endpoint");
            }
        });
    }
};

GatewayApiServer::GatewayApiServer(FirmwareUpdater& updater,
                                   TelemetryStore& store,
                                   GatewayApiOptions options)
    : impl_(std::make_unique<Impl>(updater, store, std::move(options))) {}

GatewayApiServer::~GatewayApiServer() { stop(); }

bool GatewayApiServer::start() {
    if (impl_->running.load()) return true;
    int port = impl_->options.port == 0
        ? impl_->server.bind_to_any_port(impl_->options.bindAddress)
        : (impl_->server.bind_to_port(impl_->options.bindAddress, impl_->options.port)
               ? impl_->options.port : -1);
    if (port <= 0) return false;
    impl_->actualPort.store(port);
    impl_->running.store(true);
    impl_->started = std::chrono::steady_clock::now();
    impl_->thread = std::thread([this] {
        impl_->server.listen_after_bind();
        impl_->running.store(false);
    });
    return true;
}

void GatewayApiServer::stop() {
    if (!impl_) return;
    impl_->running.store(false);
    impl_->server.stop();
    if (impl_->thread.joinable()) impl_->thread.join();
}

bool GatewayApiServer::isRunning() const { return impl_->running.load(); }
int GatewayApiServer::actualPort() const { return impl_->actualPort.load(); }

void GatewayApiServer::setDevicePort(std::string port) {
    std::lock_guard lock(impl_->configMutex);
    impl_->devicePort = std::move(port);
    impl_->updater.setCurrentPort(impl_->devicePort);
}

void GatewayApiServer::setProtocol(std::string protocol) {
    std::lock_guard lock(impl_->configMutex);
    impl_->protocol = std::move(protocol);
}

void GatewayApiServer::setCommandHandler(
    std::function<bool(const std::string&)> handler) {
    std::lock_guard lock(impl_->configMutex);
    impl_->commandHandler = std::move(handler);
}

}  // namespace rte::runtime
