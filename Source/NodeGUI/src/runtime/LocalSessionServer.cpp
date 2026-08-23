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

json ConsoleJson(const TelemetrySnapshot& snapshot, std::uint64_t since,
                 std::size_t maximum) {
    json lines = json::array();
    const auto first = snapshot.console.size() > maximum
        ? snapshot.console.size() - maximum : 0;
    for (std::size_t index = first; index < snapshot.console.size(); ++index) {
        const auto& line = snapshot.console[index];
        if (line.seq > since) lines.push_back({{"seq", line.seq}, {"text", line.text}});
    }
    return {{"lines", std::move(lines)},
            {"latest_seq", snapshot.console.empty() ? 0 : snapshot.console.back().seq}};
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

void LocalSessionServer::SetCommandHandler(
    std::function<bool(const std::string&)> handler) {
    commandHandler_ = std::move(handler);
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
        const TelemetrySnapshot snapshot = store_.Snapshot();
        json result;
        if (method == "device.status") {
            result = {{"app", "RTE Studio"}, {"device_port", devicePort_},
                      {"connected", !devicePort_.empty() && !snapshot.suspended},
                      {"suspended", snapshot.suspended}, {"rx_hz", snapshot.rxHz},
                      {"external_writes_enabled", externalDeviceWritesEnabled_}};
        } else if (method == "device.telemetry") {
            result = {{"rx_hz", snapshot.rxHz}, {"suspended", snapshot.suspended},
                      {"signals", snapshot.latest}, {"strings", snapshot.latestStr}};
        } else if (method == "device.console") {
            const std::uint64_t since = params.value("since", std::uint64_t{0});
            const auto lines = std::clamp(params.value("lines", std::size_t{100}),
                                          std::size_t{1}, std::size_t{1000});
            result = ConsoleJson(snapshot, since, lines);
        } else if (method == "device.command") {
            if (!externalDeviceWritesEnabled_) {
                return json{{"ok", false},
                            {"error", "external device writes are disabled in RTE Studio preferences"}}.dump();
            }
            const std::string command = params.value("command", "");
            if (command.empty()) {
                return json{{"ok", false}, {"error", "command is empty"}}.dump();
            }
            if (snapshot.suspended || !commandHandler_ || !commandHandler_(command)) {
                return json{{"ok", false}, {"error", "device command could not be sent"}}.dump();
            }
            result = {{"sent", true}, {"command", command}};
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
