#include "MainWindow.h"

#include <RTEAutomation/CachePaths.h>

#include <QApplication>
#include <QLockFile>
#include <QPixmap>
#include <QSurfaceFormat>
#include <QTimer>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

// Headless visual smoke driver: RTE_SMOKE_DIR=<dir> makes the app step
// through its screens, grab each as <dir>/<screenIndex>.png with a
// manifest, then exit(0). Used for the offscreen dog-food loop; on a normal
// run nothing happens unless the env var is set.
struct SmokeState {
    std::ofstream manifest;
    int stage = 0;
    bool grabNextTick = false;
};

void RunSmokeSequence(NodeGUI::MainWindow* window, const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto* state = new SmokeState;
    state->manifest.open(dir / "manifest.txt", std::ios::out | std::ios::trunc);
    const int screens = window->SmokeScreenCount();
    auto* timer = new QTimer(window);
    timer->setInterval(500);
    QObject::connect(timer, &QTimer::timeout, window, [window, state, dir, screens]() {
        if (!state->grabNextTick) {
            window->SmokeSelectScreen(state->stage);
            state->grabNextTick = true;
            return;
        }
        const QPixmap shot = window->grab();
        const std::string name = dir.string() + "/" + std::to_string(state->stage) + ".png";
        shot.save(QString::fromStdString(name));
        state->manifest << state->stage << " " << (shot.isNull() ? "NULL" : "ok")
                        << " " << shot.width() << "x" << shot.height() << "\n";
        state->manifest.flush();
        ++state->stage;
        state->grabNextTick = false;
        if (state->stage >= screens) QApplication::exit(0);
    });
    timer->start();
}

}  // namespace

namespace {

void PrintUsage(const char* exe) {
    std::cerr << "usage: " << exe
              << " [graph.json] [--serial <port>] [--protocol legacy|ivp] [--simulate]\n"
              << "  --serial <port>      telemetry serial port (default /dev/ttyACM0)\n"
              << "  --protocol <mode>    wire protocol: 'legacy' (current firmware, default)\n"
              << "                       or 'ivp' (new InverterProtocol stack)\n"
              << "  --simulate           feed synthetic 100 Hz telemetry instead of the serial port\n";
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

#ifdef _WIN32
    QString serialPort = QStringLiteral("COM3");
#else
    QString serialPort = QStringLiteral("/dev/ttyACM0");
#endif
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
    window.SetupRuntime(serialPort, simulate, protocol);
    window.showNormal();

    if (!graphPath.empty()) {
        if (!window.OpenGraph(graphPath)) {
            std::cerr << "Could not open graph: " << graphPath << std::endl;
            return 1;
        }
    }

    if (const char* smokeDir = std::getenv("RTE_SMOKE_DIR"); smokeDir && *smokeDir) {
        RunSmokeSequence(&window, smokeDir);
    }

    return app.exec();
}
