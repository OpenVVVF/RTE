#include "MainWindow.h"

#include <QApplication>
#include <QSurfaceFormat>

#include <iostream>
#include <string>

namespace {

void PrintUsage(const char* exe) {
    std::cerr << "usage: " << exe << " [graph.json] [--serial <port>] [--simulate]\n"
              << "  --serial <port>  telemetry serial port (default /dev/ttyACM0)\n"
              << "  --simulate       feed synthetic 100 Hz telemetry instead of the serial port\n";
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
    bool simulate = false;
    std::string graphPath;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--serial") {
            if (++i >= argc) {
                PrintUsage(argv[0]);
                return 1;
            }
            serialPort = QString::fromStdString(argv[i]);
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
    window.SetupRuntime(serialPort, simulate);
    window.showNormal();

    if (!graphPath.empty()) {
        if (!window.OpenGraph(graphPath)) {
            std::cerr << "Could not open graph: " << graphPath << std::endl;
            return 1;
        }
    }

    return app.exec();
}
