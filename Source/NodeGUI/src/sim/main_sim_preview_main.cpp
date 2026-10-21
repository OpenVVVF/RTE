// Dev-only standalone harness for the Simulate screen — NOT wired into the
// NodeGUI CMake target (the parent owns that file); compiled by
// src/sim/build_preview.sh with raw g++ so the screen can be dogfooded
// offscreen before it is wired into MainWindow.
//
//   sim_preview [session.jsonl]     open the Simulate screen standalone;
//                                   a session file argument pre-loads the
//                                   telemetry region
//   sim_preview --selfcheck FILE    run SimSession::SelfCheck and exit 0/1
//
// env:
//   RTE_SIM_PREVIEW_GRAB=<png>      quit after ~1.2 s, saving a window grab
//   RTE_SIM_SCENARIO_DEMO=1         prefill the gen7 bench card paths
//                                   (foc_demo + gen7_bench_pmsm, 500 rpm)

#include "sim/SimulateScreen.h"
#include "sim/SimSession.h"

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {

void PrefillDemoScenario(NodeGUI::sim::SimulateScreen& screen) {
#ifdef RTE_PROJECT_ROOT
    const QString root = QString::fromUtf8(RTE_PROJECT_ROOT);
    QString sw = root + QStringLiteral("/Assets/Examples/foc_demo.json");
    QString plant =
        root + QStringLiteral("/Assets/PlantExamples/gen7_bench_pmsm.json");
    if (std::getenv("RTE_SIM_SWAP_GRAPHS")) {
        // Invalid scenario on purpose (plant graph as the controller): makes
        // rte itself fail mid-run so the missing-artifact path is exercised.
        std::swap(sw, plant);
    }
    screen.SetSwGraphPath(sw);
    screen.SetPlantGraphPath(plant);
    if (auto* rpm = screen.findChild<QLineEdit*>(QStringLiteral("simDynoRpm"))) {
        rpm->setText(QStringLiteral("500"));
    }
#endif
}

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("RTE Studio Simulate preview"));

    QStringList args = app.arguments().mid(1);
    const qsizetype selfcheckIndex = args.indexOf(QStringLiteral("--selfcheck"));
    if (selfcheckIndex >= 0 && selfcheckIndex + 1 < args.size()) {
        QString report;
        const bool ok = NodeGUI::sim::SimSession::SelfCheck(
            args.at(selfcheckIndex + 1), report);
        std::cout << report.toStdString();
        return ok ? 0 : 1;
    }

    NodeGUI::sim::SimulateScreen screen;
    screen.setWindowTitle(QStringLiteral("RTE Studio — Simulate (preview)"));
    screen.resize(1100, 860);
    if (std::getenv("RTE_SIM_SCENARIO_DEMO")) {
        PrefillDemoScenario(screen);
    }
    if (!args.isEmpty() && !args.first().startsWith(u'-')) {
        screen.LoadSessionFile(args.first());
    }
    if (const char* filter = std::getenv("RTE_SIM_FILTER")) {
        if (auto* edit = screen.findChild<QLineEdit*>(
                QStringLiteral("simSignalFilter"))) {
            edit->setText(QString::fromUtf8(filter));
        }
    }
    screen.show();

    const char* grabEnv = std::getenv("RTE_SIM_PREVIEW_GRAB");
    const auto grabAndExit = [&screen, &app, grabEnv](int delayMs) {
        if (!grabEnv || !*grabEnv) {
            QTimer::singleShot(delayMs, &app, [] { QApplication::exit(0); });
            return;
        }
        const QString path = QString::fromUtf8(grabEnv);
        QTimer::singleShot(delayMs, &app, [&screen, path]() {
            const QPixmap shot = screen.grab();
            std::cout << "grab: " << (shot.save(path) ? "ok " : "FAILED ")
                      << shot.width() << "x" << shot.height() << " -> "
                      << path.toStdString() << "\n";
            QApplication::exit(shot.isNull() ? 1 : 0);
        });
    };

    if (std::getenv("RTE_SIM_RUN")) {
        // Dogfood driver: press Run for real, wait for the status line to
        // leave the idle state (robust even for sub-second failures), then
        // grab/exit.
        auto* statusLabel =
            screen.findChild<QLabel*>(QStringLiteral("simStatus"));
        const QString idleText =
            statusLabel ? statusLabel->text() : QString();
        QTimer::singleShot(300, &screen, [&screen] {
            if (auto* run =
                    screen.findChild<QPushButton*>(QStringLiteral("simRun"))) {
                run->click();
            }
        });
        auto* watcher = new QTimer(&screen);
        watcher->setInterval(250);
        auto elapsed = std::make_shared<int>(0);
        QObject::connect(
            watcher, &QTimer::timeout, &screen,
            [&screen, watcher, statusLabel, idleText, elapsed, grabAndExit] {
                *elapsed += 250;
                const QString text =
                    statusLabel ? statusLabel->text() : QString();
                const bool settled =
                    text != idleText && !screen.IsRunning()
                    && !text.startsWith(QStringLiteral("Running "));
                if (*elapsed < 240000 && !settled) {
                    return;
                }
                watcher->stop();
                std::cout << "run finished; status=<" << text.toStdString()
                          << ">\n";
                grabAndExit(600);
            });
        watcher->start();
    } else {
        grabAndExit(1200);
    }
    return app.exec();
}
