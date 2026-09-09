#include "MainWindow.h"

#include <RTEAutomation/CachePaths.h>

#include <QApplication>
#include <QLockFile>
#include <QSurfaceFormat>
#include <QTimer>

#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

void PrintUsage(const char* exe) {
    std::cerr << "usage: " << exe
              << " [graph.json] [--serial <port>] [--tcp <host:port>] "
                 "[--protocol legacy|ivp] [--simulate] [--sim-smoke]\n"
              << "  --serial <port>      override the saved telemetry serial port\n"
              << "  --tcp <host:port>    connect InverterProtocol over TCP "
                 "(implies --protocol ivp)\n"
              << "                       e.g. --tcp 127.0.0.1:14608 for HostSim --live\n"
              << "  --protocol <mode>    wire protocol: 'legacy' (current firmware, default)\n"
              << "                       or 'ivp' (new InverterProtocol stack)\n"
              << "  --simulate           feed synthetic 100 Hz telemetry instead of the serial port\n"
              << "  --sim-smoke          headless Build & Run Simulation self-test: run the\n"
              << "                       graph (default: the HostSim SPWM demo) in the\n"
              << "                       simulator, verify live TCP telemetry, print\n"
              << "                       SIM_SMOKE PASS/FAIL and exit\n";
}

bool ParseHostPort(const std::string& spec, QString* host, int* port) {
    const auto colon = spec.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= spec.size()) {
        return false;
    }
    try {
        *port = std::stoi(spec.substr(colon + 1));
    } catch (...) {
        return false;
    }
    *host = QString::fromStdString(spec.substr(0, colon));
    return *port > 0 && *port < 65536;
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("RTE Studio"));
    app.setApplicationDisplayName(QStringLiteral("RTE Studio"));
    app.setOrganizationName(QStringLiteral("RTE"));

    std::error_code cacheError;
    const std::filesystem::path cacheRoot = RTEAutomation::DefaultCacheRoot();
    std::filesystem::create_directories(cacheRoot, cacheError);
    if (cacheError) {
        std::cerr << "RTE Studio could not create its cache directory: "
                  << cacheError.message() << "\n";
        return 1;
    }
    QLockFile instanceLock(QString::fromStdString((cacheRoot / "rte-studio.lock").string()));
    if (!instanceLock.tryLock()) {
        std::cerr << "RTE Studio is already running. Use the existing window.\n";
        return 2;
    }

    // Vsync for the GPU telemetry plots. The node canvas keeps its raster
    // viewport regardless (see MainWindow).
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(format);

    // Empty means use the persistent Device port preference. --serial always
    // overrides it for this launch. --tcp selects the HostSim --live link
    // instead of any serial port.
    QString serialPort;
    QString tcpHost;
    int tcpPort = 0;
    bool simulate = false;
    bool simSmoke = false;
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
        } else if (arg == "--sim-smoke") {
            simSmoke = true;
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

    if (simSmoke) {
        // The smoke test runs against a graph on disk rather than the editor
        // buffer; default to the HostSim SPWM demo shipped with the tree.
        if (graphPath.empty()) {
#ifdef RTE_PROJECT_ROOT
            graphPath =
                std::string(RTE_PROJECT_ROOT) + "/Images/HostSim/graphs/spwm_demo_graph.json";
#endif
        }
        std::error_code existsError;
        if (graphPath.empty()
            || !std::filesystem::is_regular_file(graphPath, existsError)) {
            std::fprintf(stderr, "SIM_SMOKE FAIL: graph not found: %s\n",
                         graphPath.c_str());
            return 1;
        }
        // No serial scraping while attached to the simulator.
        serialPort.clear();
        simulate = true;
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

    if (simSmoke) {
        // Defer into the event loop: the QCoreApplication::exit() on the PASS
        // path is a no-op before exec() starts.
        QTimer::singleShot(0, &window, [&window, graphPath] {
            window.StartSimSmoke(QString::fromStdString(graphPath));
        });
    }

    return app.exec();
}
