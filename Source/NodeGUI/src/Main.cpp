#include "MainWindow.h"

#include <QApplication>
#include <QSurfaceFormat>

#include <iostream>
#include <string>

namespace {

void PrintUsage(const char* exe) {
    std::cerr << "usage: " << exe
              << " [graph.json] [--serial <port>] [--tcp <host:port>] "
                 "[--protocol legacy|ivp] [--simulate]\n"
              << "  --serial <port>      telemetry serial port (default /dev/ttyACM0)\n"
              << "  --tcp <host:port>    connect InverterProtocol over TCP "
                 "(implies --protocol ivp)\n"
              << "                       e.g. --tcp 127.0.0.1:14608 for HostSim --live\n"
              << "  --protocol <mode>    wire protocol: 'legacy' (current firmware, default)\n"
              << "                       or 'ivp' (new InverterProtocol stack)\n"
              << "  --simulate           feed synthetic 100 Hz telemetry instead of the serial port\n";
}

bool ParseHostPort(const std::string& spec, QString* host, int* port) {
    const auto colon = spec.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= spec.size()) {
        return false;
    }
    *host = QString::fromStdString(spec.substr(0, colon));
    *port = std::stoi(spec.substr(colon + 1));
    return *port > 0 && *port < 65536;
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("NodeGUI"));
    app.setOrganizationName(QStringLiteral("RTE"));

    // Vsync for the GPU telemetry plots. The node canvas keeps its raster
    // viewport regardless (see MainWindow).
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(format);

    QString serialPort = QStringLiteral("/dev/ttyACM0");
    QString tcpHost;
    int tcpPort = 0;
    bool simulate = false;
    auto protocol = NodeGUI::runtime::Protocol::Legacy;
    std::string graphPath;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--serial") {
            if (++i >= argc) {
                PrintUsage(argv[0]);
                return 1;
            }
            serialPort = QString::fromStdString(argv[i]);
        } else if (arg == "--tcp") {
            if (++i >= argc) {
                PrintUsage(argv[0]);
                return 1;
            }
            if (!ParseHostPort(argv[i], &tcpHost, &tcpPort)) {
                std::cerr << "invalid --tcp spec (expected host:port)\n";
                PrintUsage(argv[0]);
                return 1;
            }
            protocol = NodeGUI::runtime::Protocol::Inverter;
        } else if (arg == "--protocol") {
            if (++i >= argc) {
                PrintUsage(argv[0]);
                return 1;
            }
            const std::string mode = argv[i];
            if (mode == "legacy") {
                protocol = NodeGUI::runtime::Protocol::Legacy;
            } else if (mode == "ivp") {
                protocol = NodeGUI::runtime::Protocol::Inverter;
            } else {
                std::cerr << "unknown protocol: " << mode << "\n";
                PrintUsage(argv[0]);
                return 1;
            }
        } else if (arg == "--simulate") {
            simulate = true;
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            return 0;
        } else if (graphPath.empty()) {
            graphPath = arg;
        } else {
            PrintUsage(argv[0]);
            return 1;
        }
    }

    NodeGUI::MainWindow window;
    window.SetupRuntime(serialPort, simulate, protocol, tcpHost, tcpPort);
    window.showNormal();

    if (!graphPath.empty()) {
        if (!window.OpenGraph(graphPath)) {
            std::cerr << "Could not open graph: " << graphPath << std::endl;
            return 1;
        }
    }

    return app.exec();
}
