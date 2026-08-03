#include "MainWindow.h"

#include <QApplication>
#include <QSurfaceFormat>

#include <iostream>
#include <string>

namespace {

void PrintUsage(const char* exe) {
    std::cerr << "usage: " << exe
              << " [graph.json] [--serial <port>] [--protocol legacy|ivp] [--connect <host>] [--simulate]\n"
              << "  --serial <port>      inverter serial port for the local RTEServer\n"
              << "                       (default /dev/ttyACM0)\n"
              << "  --protocol <mode>    wire protocol: 'legacy' (current firmware, default)\n"
              << "                       or 'ivp' (new InverterProtocol stack)\n"
              << "  --connect <host>     connect to a remote RTEServer instead of spawning one\n"
              << "  --simulate           feed synthetic 100 Hz telemetry instead of the serial port\n";
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
    QString connectHost;
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
        } else if (arg == "--connect") {
            if (++i >= argc) {
                PrintUsage(argv[0]);
                return 1;
            }
            connectHost = QString::fromStdString(argv[i]);
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
    window.SetupRuntime(serialPort, simulate, protocol, connectHost);
    window.showNormal();

    if (!graphPath.empty()) {
        if (!window.OpenGraph(graphPath)) {
            std::cerr << "Could not open graph: " << graphPath << std::endl;
            return 1;
        }
    }

    return app.exec();
}
