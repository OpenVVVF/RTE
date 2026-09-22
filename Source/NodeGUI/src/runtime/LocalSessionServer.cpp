#include "LocalSessionServer.h"

#include <RTEAutomation/Session.h>

#include <QCoreApplication>
#include <QHostAddress>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QVariant>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

using json = nlohmann::json;

namespace NodeGUI::runtime {
namespace {

constexpr double kReportingTimeoutSeconds = 2.0;
constexpr double kFirmwareMetadataTimeoutSeconds = 15.0;

double SignalTimeout(const std::string& key) {
    return key == "fw_manifest" || key == "fw_graph" || key == "fw_graph_hash"
        ? kFirmwareMetadataTimeoutSeconds : kReportingTimeoutSeconds;
}

std::string MakeToken() {
    std::string result;
    result.reserve(64);
    static constexpr char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        const auto byte = static_cast<unsigned char>(QRandomGenerator::system()->generate() & 0xff);
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0xf]);
    }
    return result;
}

bool ConstantTimeEqual(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) return false;
    unsigned char difference = 0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        difference |= static_cast<unsigned char>(left[i] ^ right[i]);
    }
    return difference == 0;
}

json ConsoleJson(std::vector<ConsoleLine> lines, std::uint64_t latestSeq,
                 std::uint64_t sessionEpoch, std::size_t maximum) {
    json out = json::array();
    // Keep the newest `maximum` matches, mirroring the previous behavior of
    // clamping against the rolling console tail.
    const auto first = lines.size() > maximum ? lines.size() - maximum : 0;
    for (std::size_t index = first; index < lines.size(); ++index) {
        out.push_back({{"seq", lines[index].seq}, {"text", lines[index].text}});
    }
    // session_epoch lets since-polling clients notice a ClearSession(): seq
    // numbers are never reused within a GUI run, but the archive behind them
    // is discarded, so a remembered `since` may straddle nothing.
    return {{"lines", std::move(out)},
            {"latest_seq", latestSeq},
            {"session_epoch", sessionEpoch}};
}

bool Fresh(const TelemetryStore::DeviceView& view, const std::string& key,
           double maximumAge = -1.0) {
    if (maximumAge < 0.0) maximumAge = SignalTimeout(key);
    const auto it = view.ageSeconds.find(key);
    return !view.stats.suspended && view.stats.frameAgeSeconds >= 0.0
        && view.stats.frameAgeSeconds <= kReportingTimeoutSeconds
        && it != view.ageSeconds.end() && it->second <= maximumAge;
}

json FirmwareManifest(const TelemetryStore::DeviceView& view) {
    const auto it = view.latestStr.find("fw_manifest");
    if (it == view.latestStr.end() || !Fresh(view, "fw_manifest")) return nullptr;
    const json manifest = json::parse(it->second, nullptr, false);
    return manifest.is_object() && manifest.value("schema", 0) == 1
        ? manifest : json(nullptr);
}

struct ReportingContext {
    std::unordered_set<std::string> timIsrSignals;
    std::string controlState;
    bool focStateKnown = false;
    bool focRunning = false;
};

ReportingContext MakeReportingContext(const TelemetryStore::DeviceView& view,
                                      const json& manifest) {
    ReportingContext context;
    if (Fresh(view, "control_state"))
        context.controlState = view.latestStr.at("control_state");
    if (Fresh(view, "foc_running")) {
        context.focStateKnown = true;
        context.focRunning = view.latest.at("foc_running") > 0.5f;
    }
    if (manifest.is_object() && manifest.contains("signals")
        && manifest["signals"].is_array()) {
        for (const auto& signal : manifest["signals"])
            if (signal.is_object() && signal.value("domain", "") == "tim_isr"
                && signal.contains("name") && signal["name"].is_string())
                context.timIsrSignals.insert(signal["name"].get<std::string>());
    }
    return context;
}

bool IsLegacyFocSignal(const std::string& key) {
    return (key.rfind("foc_", 0) == 0 && key != "foc_running")
        || key.rfind("obs_", 0) == 0 || key.rfind("est_", 0) == 0
        || key == "sample_gap_ticks";
}

const char* ReportingState(const TelemetryStore::DeviceView& view,
                           const ReportingContext& context, const std::string& key) {
    if (view.stats.suspended) return "suspended";
    if (view.stats.frameAgeSeconds < 0.0
        || view.stats.frameAgeSeconds > kReportingTimeoutSeconds)
        return "link_silent";
    if (context.timIsrSignals.count(key) && !context.controlState.empty()
        && context.controlState != "RUNNING") return "control_stopped";
    if (context.focStateKnown && !context.focRunning && IsLegacyFocSignal(key))
        return "foc_stopped";
    const auto age = view.ageSeconds.find(key);
    return age != view.ageSeconds.end() && age->second <= SignalTimeout(key)
        ? "live" : "stopped_reporting";
}

json SignalStatus(const TelemetryStore::DeviceView& view,
                  const ReportingContext& context, const std::string& key) {
    const auto age = view.ageSeconds.find(key);
    const char* state = ReportingState(view, context, key);
    return {{"state", state}, {"fresh", std::string_view(state) == "live"},
            {"timeout_s", SignalTimeout(key)},
            {"age_s", age == view.ageSeconds.end() ? json(nullptr) : json(age->second)}};
}

}  // namespace

LocalSessionServer::LocalSessionServer(TelemetryStore& store, QObject* parent)
    : QObject(parent), store_(store), server_(new QTcpServer(this)) {
    connect(server_, &QTcpServer::newConnection, this,
            [this] { AcceptConnections(); });
}

LocalSessionServer::~LocalSessionServer() {
    Stop();
}

bool LocalSessionServer::Start(std::string* error) {
    if (server_->isListening()) return true;
    token_ = MakeToken();
    if (!server_->listen(QHostAddress::LocalHost, 0)) {
        if (error) *error = server_->errorString().toStdString();
        return false;
    }
    RTEAutomation::SessionDescriptor descriptor;
    descriptor.app = "RTE Studio";
    descriptor.port = static_cast<int>(server_->serverPort());
    descriptor.token = token_;
    descriptor.pid = static_cast<std::uint64_t>(QCoreApplication::applicationPid());
    if (!RTEAutomation::WriteSessionDescriptor(descriptor, error)) {
        server_->close();
        return false;
    }
    return true;
}

void LocalSessionServer::Stop() {
    if (server_->isListening()) server_->close();
    RTEAutomation::RemoveSessionDescriptor(
        static_cast<std::uint64_t>(QCoreApplication::applicationPid()));
}

bool LocalSessionServer::IsRunning() const { return server_->isListening(); }
int LocalSessionServer::Port() const { return static_cast<int>(server_->serverPort()); }

void LocalSessionServer::SetDevicePort(std::string port) {
    devicePort_ = std::move(port);
}

void LocalSessionServer::SetTransportProvider(std::function<std::string()> provider) {
    transportProvider_ = std::move(provider);
}

void LocalSessionServer::SetCommandHandler(
    std::function<bool(const std::string&, const std::string&)> handler) {
    commandHandler_ = std::move(handler);
}

void LocalSessionServer::SetActivityHandler(
    std::function<void(const std::string&)> handler) {
    activityHandler_ = std::move(handler);
}

void LocalSessionServer::SetFlashLeaseHandler(std::function<void(bool)> handler) {
    flashLeaseHandler_ = std::move(handler);
}

void LocalSessionServer::SetExternalDeviceWritesEnabled(bool enabled) {
    externalDeviceWritesEnabled_ = enabled;
}

void LocalSessionServer::AcceptConnections() {
    while (QTcpSocket* socket = server_->nextPendingConnection()) {
        connect(socket, &QTcpSocket::readyRead, this,
                [this, socket] { ReadRequest(socket); });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }
}

void LocalSessionServer::ReadRequest(QTcpSocket* socket) {
    QByteArray data = socket->property("rteSessionBuffer").toByteArray();
    data += socket->readAll();
    if (data.size() > 4 * 1024 * 1024) {
        socket->write("{\"ok\":false,\"error\":\"request too large\"}\n");
        socket->disconnectFromHost();
        return;
    }
    const qsizetype newline = data.indexOf('\n');
    if (newline < 0) {
        socket->setProperty("rteSessionBuffer", data);
        return;
    }
    const std::string response = HandleRequest(data.left(newline).toStdString()) + "\n";
    socket->write(response.data(), static_cast<qint64>(response.size()));
    socket->disconnectFromHost();
}

std::string LocalSessionServer::HandleRequest(const std::string& line) const {
    try {
        const json request = json::parse(line);
        if (!ConstantTimeEqual(request.value("token", ""), token_)) {
            return json{{"ok", false}, {"error", "authentication failed"}}.dump();
        }
        const std::string method = request.value("method", "");
        const json params = request.value("params", json::object());
        // No hoisted Snapshot(): each endpoint fetches only what it needs.
        // Snapshot() deep-copies every rolling history (up to 12000 points
        // per signal), which made these 30+ Hz polls absurdly expensive.
        json result;
        if (method == "device.status") {
            const auto stats = store_.GetStatsLine();
            const bool responding = !stats.suspended && stats.frameAgeSeconds >= 0.0
                && stats.frameAgeSeconds <= kReportingTimeoutSeconds;
            result = {{"app", "RTE Studio"}, {"device_port", devicePort_},
                      {"connected", responding}, {"responding", responding},
                      {"configured", !devicePort_.empty()},
                      {"transport", transportProvider_ ? transportProvider_() : "unknown"},
                      {"suspended", stats.suspended}, {"rx_hz", stats.rxHz},
                      {"frame_age_s", stats.frameAgeSeconds < 0.0
                          ? json(nullptr) : json(stats.frameAgeSeconds)},
                      {"external_writes_enabled", externalDeviceWritesEnabled_}};
        } else if (method == "device.telemetry") {
            const auto view = store_.GetDeviceView();
            const json manifest = FirmwareManifest(view);
            const auto reporting = MakeReportingContext(view, manifest);
            std::unordered_map<std::string, std::string> owners;
            if (manifest.is_object() && manifest.contains("signals")
                && manifest["signals"].is_array()) {
                for (const auto& declared : manifest["signals"]) {
                    if (!declared.is_object() || !declared.contains("name")
                        || !declared["name"].is_string()) continue;
                    owners[declared["name"].get<std::string>()] = declared.value(
                        "source_node", declared.value("logger_node", "graph"));
                }
            }
            json groups = json::object(), numericValues = json::object();
            json strings = json::object(), lastKnown = json::object();
            json signalStatus = json::object(), stopped = json::array();
            auto owner = [&](const std::string& key) -> std::string {
                const auto it = owners.find(key);
                if (it != owners.end()) return it->second;
                if (key.rfind("fw_", 0) == 0) return "firmware_build";
                if (key.rfind("fault_", 0) == 0 || key.rfind("control_", 0) == 0
                    || key.rfind("gate_", 0) == 0 || key == "pwm_moe")
                    return "firmware_control";
                return "observed_only";
            };
            for (const auto& [key, value] : view.latest) {
                signalStatus[key] = SignalStatus(view, reporting, key);
                const bool fresh = signalStatus[key]["fresh"];
                numericValues[key] = fresh ? json(value) : json(nullptr);
                if (!fresh) { lastKnown[key] = value; stopped.push_back(key); }
                groups[owner(key)][key] = numericValues[key];
            }
            for (const auto& [key, value] : view.latestStr) {
                signalStatus[key] = SignalStatus(view, reporting, key);
                const bool fresh = signalStatus[key]["fresh"];
                strings[key] = fresh ? json(value) : json(nullptr);
                if (!fresh) { lastKnown[key] = value; stopped.push_back(key); }
                groups[owner(key)][key] = strings[key];
            }
            result = {{"rx_hz", view.stats.rxHz}, {"suspended", view.stats.suspended},
                      {"rx_bytes_per_sec", view.stats.rxBytesPerSec},
                      {"good_frames", view.stats.goodFrames}, {"bad_frames", view.stats.badFrames},
                      {"reject_crc", view.stats.rejectCrc}, {"reject_header", view.stats.rejectHdr},
                      {"reject_length", view.stats.rejectLen},
                      {"reject_payload", view.stats.rejectPayloadParse},
                      {"reject_unknown_id", view.stats.rejectUnknownId},
                      {"last_sequence", view.stats.lastSeq},
                      {"frame_age_s", view.stats.frameAgeSeconds < 0.0
                          ? json(nullptr) : json(view.stats.frameAgeSeconds)},
                      {"age_s", view.ageSeconds},
                      {"signals", std::move(numericValues)}, {"strings", std::move(strings)},
                      {"signal_status", std::move(signalStatus)},
                      {"stopped_signals", std::move(stopped)},
                      {"last_known_values", std::move(lastKnown)},
                      {"reporting_timeout_s", kReportingTimeoutSeconds},
                      {"firmware_metadata_timeout_s", kFirmwareMetadataTimeoutSeconds},
                      {"value_meaning", "null means no current report; inspect signal_status and last_known_values. A stopped report is not a measured zero."},
                      {"groups", std::move(groups)},
                      {"reject_unknown_id_meaning",
                       "data ID has no received DEFINE; persistent growth suggests dropped definitions or transport loss"}};
        } else if (method == "device.build_info") {
            const auto view = store_.GetDeviceView();
            const json manifest = FirmwareManifest(view);
            result = {{"available", !manifest.is_null()}, {"manifest", manifest},
                      {"source", "running firmware telemetry"},
                      {"reason", manifest.is_null()
                          ? "no fresh firmware manifest; image may predate build identity or link is down"
                          : ""}};
        } else if (method == "device.catalog") {
            const auto view = store_.GetDeviceView();
            const json manifest = FirmwareManifest(view);
            const auto reporting = MakeReportingContext(view, manifest);
            const std::string filter = params.value("filter", "");
            const std::string requested = params.value("signal", "");
            json entries = json::array();
            std::unordered_set<std::string> listed;
            if (manifest.is_object() && manifest.contains("signals")
                && manifest["signals"].is_array()) {
                for (const auto& declared : manifest["signals"]) {
                    if (!declared.is_object() || !declared.contains("name")
                        || !declared["name"].is_string()) continue;
                    const std::string name = declared["name"];
                    listed.insert(name);
                    if ((!filter.empty() && name.find(filter) == std::string::npos)
                        || (!requested.empty() && name != requested)) continue;
                    json entry = declared;
                    const auto age = view.ageSeconds.find(name);
                    entry["age_s"] = age == view.ageSeconds.end() ? json(nullptr) : json(age->second);
                    entry["state"] = age == view.ageSeconds.end()
                        ? "configured_not_streaming" : ReportingState(view, reporting, name);
                    entries.push_back(std::move(entry));
                }
            }
            auto addObserved = [&](const std::string& name, const char* kind) {
                if (listed.count(name) || (!filter.empty() && name.find(filter) == std::string::npos)
                    || (!requested.empty() && name != requested)) return;
                entries.push_back({{"name", name}, {"kind", kind},
                    {"state", ReportingState(view, reporting, name)},
                    {"age_s", view.ageSeconds.at(name)}, {"source", "observed_only"}});
            };
            for (const auto& [name, value] : view.latest) addObserved(name, "number");
            for (const auto& [name, value] : view.latestStr) addObserved(name, "string");
            result = {{"build_verified", !manifest.is_null()}, {"signals", std::move(entries)}};
            if (!requested.empty() && result["signals"].empty())
                result["state"] = manifest.is_null() ? "unknown" : "not_in_build";
        } else if (method == "device.control_status") {
            const auto view = store_.GetDeviceView();
            const auto reporting = MakeReportingContext(view, FirmwareManifest(view));
            auto stringValue = [&](const std::string& key) -> json {
                const auto it = view.latestStr.find(key);
                return it == view.latestStr.end() || !Fresh(view, key)
                    ? json(nullptr) : json(it->second);
            };
            auto numberValue = [&](const std::string& key) -> json {
                const auto it = view.latest.find(key);
                return it == view.latest.end() || !Fresh(view, key)
                    ? json(nullptr) : json(it->second);
            };
            result = {{"fresh", Fresh(view, "control_state") && Fresh(view, "fault_flags_hex")},
                      {"reporting_state", ReportingState(view, reporting, "control_state")},
                      {"state", stringValue("control_state")},
                      {"fault_flags_hex", stringValue("fault_flags_hex")},
                      {"fault_names", stringValue("fault_active_names")},
                      {"pwm_moe", numberValue("pwm_moe")},
                      {"gate_ready", numberValue("gate_ready")},
                      {"gate_fault", numberValue("gate_fault")},
                      {"age_s", view.ageSeconds}};
        } else if (method == "device.snapshot") {
            const auto view = store_.GetDeviceView();
            const auto reporting = MakeReportingContext(view, FirmwareManifest(view));
            const std::string bundle = params.value("bundle", "foc");
            std::vector<std::string> keys;
            if (params.contains("signals") && params["signals"].is_array())
                keys = params["signals"].get<std::vector<std::string>>();
            if (keys.empty() && bundle == "foc") {
                if (view.latest.count("cg_id_a"))
                    keys = {"cg_id_a", "cg_iq_a", "cg_vd_v", "cg_vq_v", "cg_theta_rad",
                            "cg_vdc_v", "cg_iu_a", "cg_iv_a", "cg_iw_a"};
                else
                    keys = {"foc_id_cmd", "foc_id", "foc_iq_cmd", "foc_iq", "foc_vd",
                            "foc_vq", "foc_elec_angle", "foc_speed", "foc_vdc",
                            "foc_iu", "foc_iv", "foc_iw"};
                keys.insert(keys.end(), {"control_state", "fault_flags_hex",
                    "fault_active_names", "pwm_moe", "gate_ready", "gate_fault"});
            }
            if (keys.empty() || keys.size() > 32)
                return json{{"ok", false}, {"error", "snapshot requires 1 to 32 signal names or bundle=foc"}}.dump();
            json values = json::object(), missing = json::array();
            for (const auto& key : keys) {
                const auto number = view.latest.find(key);
                const auto string = view.latestStr.find(key);
                const json status = SignalStatus(view, reporting, key);
                const bool fresh = status["fresh"].get<bool>();
                if (number != view.latest.end()) values[key] = {
                    {"value", fresh ? json(number->second) : json(nullptr)},
                    {"last_value", number->second}, {"kind", "number"},
                    {"state", status["state"]}, {"fresh", fresh},
                    {"age_s", view.ageSeconds.at(key)}};
                else if (string != view.latestStr.end()) values[key] = {
                    {"value", fresh ? json(string->second) : json(nullptr)},
                    {"last_value", string->second}, {"kind", "string"},
                    {"state", status["state"]}, {"fresh", fresh},
                    {"age_s", view.ageSeconds.at(key)}};
                else missing.push_back(key);
            }
            result = {{"bundle", bundle}, {"values", std::move(values)},
                      {"missing", std::move(missing)},
                      {"frame_age_s", view.stats.frameAgeSeconds < 0.0
                          ? json(nullptr) : json(view.stats.frameAgeSeconds)}};
        } else if (method == "device.histories") {
            if (!params.contains("signals") || !params["signals"].is_array()
                || params["signals"].empty() || params["signals"].size() > 8)
                return json{{"ok", false}, {"error", "signals must contain 1 to 8 names"}}.dump();
            const auto keys = params["signals"].get<std::vector<std::string>>();
            const std::size_t limit = std::clamp(params.value("limit", std::size_t{2000}),
                                                  std::size_t{1}, std::size_t{4000});
            const double window = std::clamp(params.value("window_s", 5.0), 0.05, 30.0);
            const auto view = store_.GetDeviceView();
            const auto reporting = MakeReportingContext(view, FirmwareManifest(view));
            const auto histories = store_.CopyHistories(keys);
            double endTime = 0.0;
            double freshestAge = std::numeric_limits<double>::infinity();
            for (const auto& [key, history] : histories) {
                if (history.t.empty()) continue;
                const auto age = view.ageSeconds.find(key);
                if (age != view.ageSeconds.end() && age->second < freshestAge) {
                    freshestAge = age->second;
                    endTime = double(history.t.back()) + age->second;
                } else if (freshestAge == std::numeric_limits<double>::infinity()) {
                    endTime = std::max(endTime, double(history.t.back()));
                }
            }
            json series = json::object(), status = json::object();
            json missing = json::array();
            for (const auto& key : keys) {
                const auto it = histories.find(key);
                if (it == histories.end()) { missing.push_back(key); continue; }
                status[key] = SignalStatus(view, reporting, key);
                const auto& history = it->second;
                json samples = json::array();
                const std::size_t first = history.t.size() > limit
                    ? history.t.size() - limit : 0;
                for (std::size_t i = first; i < history.t.size() && i < history.y.size(); ++i)
                    if (history.t[i] >= endTime - window && history.t[i] <= endTime)
                        samples.push_back({{"time_s", history.t[i]}, {"value", history.y[i]}});
                series[key] = std::move(samples);
            }
            result = {{"series", std::move(series)}, {"missing", std::move(missing)},
                      {"signal_status", std::move(status)},
                      {"window_end_s", endTime}, {"window_s", window},
                      {"time_basis", "host seconds since runtime start"},
                      {"sample_alignment", "individual timestamps; do not assume exact simultaneity"}};
        } else if (method == "device.history") {
            const std::string signal = params.value("signal", "");
            const std::size_t limit = std::clamp(params.value("limit", std::size_t{1000}),
                                                 std::size_t{1}, std::size_t{12000});
            std::deque<float> times, values;
            if (signal.empty() || !store_.CopyHistory(signal, times, values)) {
                return json{{"ok", false}, {"error", "unknown numeric signal: " + signal}}.dump();
            }
            json samples = json::array();
            const std::size_t first = times.size() > limit ? times.size() - limit : 0;
            for (std::size_t i = first; i < times.size() && i < values.size(); ++i)
                samples.push_back({{"time_s", times[i]}, {"value", values[i]}});
            const auto view = store_.GetDeviceView();
            const auto reporting = MakeReportingContext(view, FirmwareManifest(view));
            result = {{"signal", signal}, {"samples", std::move(samples)},
                      {"status", SignalStatus(view, reporting, signal)}};
        } else if (method == "device.string_history") {
            const std::string signal = params.value("signal", "");
            const std::size_t limit = std::clamp(params.value("limit", std::size_t{1000}),
                                                 std::size_t{1}, std::size_t{1000});
            std::vector<SessionStringSample> history;
            if (signal.empty() || !store_.CopyStringHistory(signal, limit, history)) {
                return json{{"ok", false}, {"error", "unknown string signal: " + signal}}.dump();
            }
            json samples = json::array();
            for (const auto& sample : history)
                samples.push_back({{"time_s", sample.tsec}, {"value", sample.value}});
            const auto view = store_.GetDeviceView();
            const auto reporting = MakeReportingContext(view, FirmwareManifest(view));
            result = {{"signal", signal}, {"samples", std::move(samples)},
                      {"status", SignalStatus(view, reporting, signal)}};
        } else if (method == "device.console") {
            const std::uint64_t since = params.value("since", std::uint64_t{0});
            const auto lines = std::clamp(params.value("lines", std::size_t{100}),
                                          std::size_t{1}, std::size_t{1000});
            result = ConsoleJson(store_.ConsoleSince(since),
                                 store_.LatestConsoleSeq(),
                                 store_.SessionEpoch(), lines);
        } else if (method == "device.command") {
            const std::string source = params.value("source", "api");
            const std::string command = params.value("command", "");
            if (source != "api" && source != "mcp" && source != "cli") {
                return json{{"ok", false}, {"error", "invalid command source"}}.dump();
            }
            if (!externalDeviceWritesEnabled_) {
                const std::string label = source == "mcp" ? "MCP"
                    : source == "cli" ? "CLI" : "API";
                if (activityHandler_)
                    activityHandler_("[" + label + "] blocked command "
                                     + json(command).dump(-1, ' ', true)
                                     + ": external device writes disabled");
                return json{{"ok", false},
                            {"error", "external device writes are disabled in RTE Studio preferences"}}.dump();
            }
            if (command.empty()) {
                return json{{"ok", false}, {"error", "command is empty"}}.dump();
            }
            const auto consoleSince = store_.LatestConsoleSeq();
            if (!commandHandler_ || !commandHandler_(command, source)) {
                return json{{"ok", false}, {"error", "device command could not be sent"}}.dump();
            }
            result = {{"sent", true}, {"command", command},
                      {"console_since", consoleSince}};
        } else if (method == "automation.activity") {
            const std::string action = params.value("action", "");
            const std::string detail = params.value("detail", "");
            const std::string state = params.value("state", "");
            if (action.empty() || action.size() > 80 || detail.size() > 500
                || (state != "started" && state != "completed" && state != "failed")
                || action.find_first_of("\r\n\t") != std::string::npos
                || detail.find_first_of("\r\n\t") != std::string::npos) {
                return json{{"ok", false}, {"error", "invalid automation activity"}}.dump();
            }
            if (activityHandler_) {
                activityHandler_("[MCP] " + action + (detail.empty() ? "" : " " + detail)
                                 + " — " + state);
            }
            result = {{"recorded", static_cast<bool>(activityHandler_)}};
        } else if (method == "device.flash.begin") {
            if (!externalDeviceWritesEnabled_) {
                return json{{"ok", false},
                            {"error", "external device writes are disabled in RTE Studio preferences"}}.dump();
            }
            if (devicePort_.empty() || !flashLeaseHandler_) {
                return json{{"ok", false}, {"error", "no device is available for flashing"}}.dump();
            }
            flashLeaseHandler_(true);
            result = {{"device_port", devicePort_}};
        } else if (method == "device.flash.end") {
            if (flashLeaseHandler_) flashLeaseHandler_(false);
            result = {{"released", true}};
        } else {
            return json{{"ok", false}, {"error", "unknown session method: " + method}}.dump();
        }
        return json{{"ok", true}, {"result", std::move(result)}}.dump();
    } catch (const std::exception& exception) {
        return json{{"ok", false}, {"error", std::string("invalid request: ") + exception.what()}}.dump();
    }
}

}  // namespace NodeGUI::runtime
