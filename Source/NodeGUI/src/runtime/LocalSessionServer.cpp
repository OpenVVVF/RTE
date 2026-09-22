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

using json = nlohmann::json;

namespace NodeGUI::runtime {
namespace {

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
            result = {{"app", "RTE Studio"}, {"device_port", devicePort_},
                      {"connected", !devicePort_.empty() && !stats.suspended},
                      {"transport", transportProvider_ ? transportProvider_() : "unknown"},
                      {"suspended", stats.suspended}, {"rx_hz", stats.rxHz},
                      {"external_writes_enabled", externalDeviceWritesEnabled_}};
        } else if (method == "device.telemetry") {
            const auto view = store_.GetDeviceView();
            result = {{"rx_hz", view.stats.rxHz}, {"suspended", view.stats.suspended},
                      {"rx_bytes_per_sec", view.stats.rxBytesPerSec},
                      {"good_frames", view.stats.goodFrames}, {"bad_frames", view.stats.badFrames},
                      {"reject_crc", view.stats.rejectCrc}, {"reject_header", view.stats.rejectHdr},
                      {"reject_length", view.stats.rejectLen},
                      {"reject_payload", view.stats.rejectPayloadParse},
                      {"reject_unknown_id", view.stats.rejectUnknownId},
                      {"last_sequence", view.stats.lastSeq},
                      {"signals", view.latest}, {"strings", view.latestStr}};
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
            result = {{"signal", signal}, {"samples", std::move(samples)}};
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
            result = {{"signal", signal}, {"samples", std::move(samples)}};
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
