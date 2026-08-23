#pragma once

#include "TelemetryStore.h"

#include <QObject>

#include <functional>
#include <string>

class QTcpServer;
class QTcpSocket;

namespace NodeGUI::runtime {

// RTE Studio owns live device state. This loopback-only, authenticated
// newline-JSON endpoint lets the finite `rte` worker and `rte mcp` query that
// state without putting build/generation logic in the GUI process.
class LocalSessionServer final : public QObject {
public:
    explicit LocalSessionServer(TelemetryStore& store, QObject* parent = nullptr);
    ~LocalSessionServer() override;

    bool Start(std::string* error = nullptr);
    void Stop();
    bool IsRunning() const;
    int Port() const;

    void SetDevicePort(std::string port);
    void SetCommandHandler(std::function<bool(const std::string&)> handler);
    void SetFlashLeaseHandler(std::function<void(bool)> handler);
    void SetExternalDeviceWritesEnabled(bool enabled);

private:
    void AcceptConnections();
    void ReadRequest(QTcpSocket* socket);
    std::string HandleRequest(const std::string& line) const;

    TelemetryStore& store_;
    QTcpServer* server_ = nullptr;
    std::string token_;
    std::string devicePort_;
    std::function<bool(const std::string&)> commandHandler_;
    std::function<void(bool)> flashLeaseHandler_;
    bool externalDeviceWritesEnabled_ = false;
};

}  // namespace NodeGUI::runtime
