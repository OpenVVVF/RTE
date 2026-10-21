// RTE Studio entry point: single-instance lock, runtime wiring, and the
// headless smoke driver (RTE_SMOKE_DIR / RTE_SMOKE_ACTIONS, see below).

// ---- GUI architecture notes (routing + node glyph layer) -----------------
//
// Manual wire routing ("waypoints"):
//   Connections render with the QtNodes cubic default UNTIL the user places
//   manual bends, so untouched graphs save and render byte-identical. User
//   bends live ONLY in the GUI layer (NodeGraphModel::connectionWaypoints_,
//   keyed by QtNodes connection id, scene coordinates); NodeAPI never sees
//   them. GraphScene merges them into the document on save/snapshot as an
//   OPTIONAL per-connection key, and re-applies them on load/undo:
//       {"id":"c3","from":{...},"to":{...},
//        "waypoints":[{"x":120.0,"y":40.0}, ...]}
//   Unknown keys here are safe: NodeAPI's ConnectionFromJson/BridgeFromJson
//   read named fields only, and both code generators consume the parsed
//   Graph object. Wires with bends render rectilinear (WireRoute::BuildRoute:
//   horizontal port stubs, single elbows per leg); the auto-elbow only exists
//   at paint time and is never serialized. Gestures (GraphView): drag a wire
//   to insert/move a bend (grid-snapped, dominant-axis lock), Ctrl+click or
//   right-click a bend handle to delete it. The Edit-menu route.addPoint /
//   route.removePoint verbs (also smoke-clickable) bend/remove on the single
//   selected wire. BridgeConnectionPainter::routeBounds extends the item
//   bounding rect via a small QtNodes hook (AbstractConnectionPainter::
//   routeBounds) so routed wires repaint correctly.
//
// Domain node glyphs (painter layer only, no model/geometry changes):
//   TypedNodePainter::DrawTypeGlyph paints a type-driven icon into the node
//   center (between the port text columns, above the parameter block).
//   NodeGlyphs keeps the registry (exact type id wins over prefix match):
//   Plant.Machine.* -> motor, Plant.Inverter.* -> bridge, Plant.DcBus ->
//   dc rail, Plant.Thermal.* -> heatsink, Plant.Sensor.* -> hall ring /
//   encoder disc / voltmeter, Plant.Shaft.* -> shaft block, Plant.DcDc.* ->
//   transformer, Control.Pi -> PI block, Transforms.Svpwm -> PWM carrier.
//   Glyph colors come from the node's style (FontColor family), i.e. they
//   follow MainWindow::ApplyThemeMode instead of hardcoding a light look.
// ---------------------------------------------------------------------------

#include "MainWindow.h"

#include "runtime/SignalPlotWidget.h"
#include "runtime/TelemetryPanel.h"
#include "sim/SimTelemetryView.h"

#include <RTEAutomation/CachePaths.h>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QLockFile>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QSlider>
#include <QStatusBar>
#include <QTableWidget>
#include <QTextEdit>
#include <QSurfaceFormat>
#include <QTimer>
#include <QThread>
#include <QWheelEvent>

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

// Fine-grained smoke actions: RTE_SMOKE_ACTIONS='screen 2; click simRun;
// wait 12000; grab sim_after; wait 50; exit 0' plays them in order on a
// fixed cadence and supplements the default per-screen sweep when set.
void RunSmokeActions(NodeGUI::MainWindow* window,
                     const std::filesystem::path& dir,
                     const std::string& script) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto* actions = new QStringList;
    for (const auto& step : QString::fromStdString(script).split(';', Qt::SkipEmptyParts)) {
        actions->push_back(step.trimmed());
    }
    auto* idx = new int; *idx = 0;
    auto* timer = new QTimer(window);
    timer->setInterval(300);
    QObject::connect(timer, &QTimer::timeout, window,
                     [window, dir, actions, idx]() {
        if (*idx >= actions->size()) { QApplication::exit(0); return; }
        const QString step = actions->at(*idx); ++*idx;
        const QStringList parts = step.split(' ', Qt::SkipEmptyParts);
        const QString kind = parts.value(0);
        const QString arg = parts.value(1);
        if (kind == "screen") {
            window->SmokeSelectScreen(arg.toInt());
        } else if (kind == "pick") {
            window->SmokePickNode(arg.toStdString());
        } else if (kind == "pickconn") {
            window->SmokePickConnection(arg.toStdString());
        } else if (kind == "movenode") {
            const bool ok = window->SmokeMoveNode(arg.toStdString(),
                                                  parts.value(2).toDouble(),
                                                  parts.value(3).toDouble());
            std::cout << "movenode " << arg.toStdString() << "=" << (ok ? "ok" : "failed") << "\n";
        } else if (kind == "dragnode") {
            const bool ok = window->SmokeDragNode(arg.toStdString(),
                                                  parts.value(2).toDouble(),
                                                  parts.value(3).toDouble());
            std::cout << "dragnode " << arg.toStdString() << "=" << (ok ? "ok" : "failed") << "\n";
        } else if (kind == "plainclick") {
            const bool ok = window->SmokePlainClick(arg.toStdString());
            std::cout << "plainclick " << arg.toStdString() << "=" << (ok ? "ok" : "failed") << "\n";
        } else if (kind == "mousedrag") {
            const bool ok = window->SmokeMouseDragNode(arg.toStdString(),
                                                       parts.value(2).toDouble(),
                                                       parts.value(3).toDouble());
            std::cout << "mousedrag " << arg.toStdString() << "=" << (ok ? "ok" : "failed") << "\n";
        } else if (kind == "dumppos") {
            std::cout << window->SmokeDumpNodeGeometry(arg.toStdString()).toStdString();
        } else if (kind == "click") {
            if (QAbstractButton* button = window->findChild<QAbstractButton*>(arg)) {
                button->click();
            } else if (QAction* action = window->findChild<QAction*>(arg)) {
                action->trigger();
            } else {
                std::cerr << "smoke: no button named " << arg.toStdString() << "\n";
            }
        } else if (kind == "set") {
            QString text = step.mid(step.indexOf(arg) + arg .size()). trimmed();
            if (QWidget* w = window->findChild<QWidget*>(arg)) {
                if (auto* edit = qobject_cast<QLineEdit*>(w)) edit->setText(text);
                else if (auto* combo = qobject_cast<QComboBox*>(w)) combo->setCurrentText(text);
                else if (auto* spin = qobject_cast<QDoubleSpinBox*>(w)) spin->setValue(text.toDouble());
                else if (auto* slider = qobject_cast<QSlider*>(w)) slider->setValue(text.toInt());
                else if (auto* plain = qobject_cast<QPlainTextEdit*>(w)) {
                    // action scripts carry newlines as "\\n" or "\n"; both mean newline
                    plain->setPlainText(text.replace("\\\\n", "\n").replace("\\n", "\n"));
                }
                else if (auto* area = qobject_cast<QTextEdit*>(w)) {
                    area->setPlainText(text.replace("\\\\n", "\n").replace("\\n", "\n"));
                }
                else std::cerr << "smoke: unsupported widget for set " << arg.toStdString() << "\n";
            } else {
                std::cerr << "smoke: no widget named " << arg.toStdString() << "\n";
            }
        } else if (kind == "dump") {
            QString value;
            if (auto* edit = window->findChild<QLineEdit*>(arg)) value = edit->text();
            else if (auto* label = window->findChild<QLabel*>(arg)) value = label->text();
            else if (auto* combo = window->findChild<QComboBox*>(arg)) value = combo->currentText();
            else if (auto* slider = window->findChild<QSlider*>(arg)) value = QString::number(slider->value());
            else if (auto* plain = window->findChild<QPlainTextEdit*>(arg)) value = plain->toPlainText().replace("\n", "|");
            else if (auto* area = window->findChild<QTextEdit*>(arg)) value = area->toPlainText().replace("\n", "|");
            else if (auto* table = window->findChild<QTableWidget*>(arg)) {
                // row=col|col rows ';'-joined — cellWidget combos dump as their
                // current text (the signal tables' Graph column).
                QStringList rows;
                for (int r = 0; r < table->rowCount(); ++r) {
                    QStringList cells;
                    for (int c = 0; c < table->columnCount(); ++c) {
                        if (auto* cellCombo = qobject_cast<QComboBox*>(table->cellWidget(r, c))) {
                            cells.push_back(cellCombo->currentText());
                        } else if (auto* item = table->item(r, c)) {
                            cells.push_back(item->text());
                        } else {
                            cells.push_back(QString());
                        }
                    }
                    rows.push_back(cells.join(QStringLiteral("|")));
                }
                value = rows.join(QStringLiteral(";"));
            }
            else {
                std::cerr << "smoke: no dumpable widget named " << arg.toStdString() << "\n";
            }
            std::cout << arg.toStdString() << "=" << value.toStdString() << "\n";
        } else if (kind == "plotdrag" || kind == "plotdblclick" || kind == "plotwheel") {
            // Synthesized REAL gestures on the surface's first plot, routed
            // through the leaf mouse/wheel handlers (the dragcursor family
            // pokes the hub instead). plotdrag takes a 0..1 plot-area
            // fraction; plotwheel takes signed notches.
            QWidget* surface = nullptr;
            if (arg == "sim") {
                surface = window->findChild<NodeGUI::sim::SimTelemetryView*>();
            } else if (arg == "runtime") {
                surface = window->findChild<NodeGUI::runtime::TelemetryPanel*>();
            }
            auto* plot = surface
                ? surface->findChild<NodeGUI::runtime::SignalPlotWidget*>()
                : nullptr;
            if (!plot) {
                std::cerr << "smoke: no plot for " << kind.toStdString() << " " << arg.toStdString() << "\n";
            } else {
                const QRect r = plot->PlotAreaRect();
                const double fx = parts.value(2).toDouble();
                const QPointF pos(r.left() + fx * r.width(), r.center().y());
                const QPointF global = plot->mapToGlobal(pos.toPoint());
                if (kind == "plotdrag") {
                    QMouseEvent press(QEvent::MouseButtonPress, pos, pos, global,
                                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                    QApplication::sendEvent(plot, &press);
                    QMouseEvent move(QEvent::MouseMove, pos, pos, global,
                                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                    QApplication::sendEvent(plot, &move);
                    QMouseEvent release(QEvent::MouseButtonRelease, pos, pos, global,
                                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                    QApplication::sendEvent(plot, &release);
                } else if (kind == "plotdblclick") {
                    QMouseEvent dbl(QEvent::MouseButtonDblClick, pos, pos, global,
                                    Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                    QApplication::sendEvent(plot, &dbl);
                    QMouseEvent release(QEvent::MouseButtonRelease, pos, pos, global,
                                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                    QApplication::sendEvent(plot, &release);
                } else {
                    const int notches = qRound(fx);
                    QWheelEvent wheel(pos, plot->mapToGlobal(pos.toPoint()),
                                      QPoint(0, 0), QPoint(0, 120 * notches),
                                      Qt::NoButton, Qt::NoModifier,
                                      Qt::NoScrollPhase, false);
                    QApplication::sendEvent(plot, &wheel);
                }
                std::cout << kind.toStdString() << " " << arg.toStdString() << "=ok\n";
            }
        } else if (kind == "dragcursor" || kind == "clearcursor" || kind == "panwindow") {
            // Playback-cursor pokes (sim | runtime): drive the same surface
            // hub methods a mouse drag / double-click / wheel pan reaches;
            // read back through the simCursorInfo / rtCursorInfo dumps.
            NodeGUI::sim::SimTelemetryView* simView = nullptr;
            NodeGUI::runtime::TelemetryPanel* runtimePanel = nullptr;
            if (arg == "sim") {
                simView = window->findChild<NodeGUI::sim::SimTelemetryView*>();
            } else if (arg == "runtime") {
                runtimePanel = window->findChild<NodeGUI::runtime::TelemetryPanel*>();
            }
            bool ok = simView || runtimePanel;
            if (ok && kind == "dragcursor") {
                const double tsec = parts.value(2).toDouble();
                if (simView) simView->DragCursorTo(tsec); else runtimePanel->DragCursorTo(tsec);
            } else if (ok && kind == "clearcursor") {
                if (simView) simView->ClearCursor(); else runtimePanel->ClearCursor();
            } else if (ok) {
                const double delta = parts.value(2).toDouble();
                if (simView) simView->PanWindowBy(delta); else runtimePanel->PanWindowBy(delta);
            }
            if (!ok) std::cerr << "smoke: no telemetry surface for " << kind.toStdString() << " " << arg.toStdString() << "\n";
            else std::cout << kind.toStdString() << " " << arg.toStdString() << "=ok\n";
        } else if (kind == "overlayload" || kind == "overlayoffset" || kind == "overlayclear") {
            // Recorded-session overlay pokes (sim | runtime): drive the same
            // surface hub methods the "Load…"/offset row reaches; read back
            // through the simOverlayInfo / rtOverlayInfo dumps.
            NodeGUI::sim::SimTelemetryView* simView = nullptr;
            NodeGUI::runtime::TelemetryPanel* runtimePanel = nullptr;
            if (arg == "sim") {
                simView = window->findChild<NodeGUI::sim::SimTelemetryView*>();
            } else if (arg == "runtime") {
                runtimePanel = window->findChild<NodeGUI::runtime::TelemetryPanel*>();
            }
            bool ok = simView || runtimePanel;
            if (ok && kind == "overlayload") {
                const QString path = QString::fromStdString(parts.value(2).toStdString());
                ok = simView ? simView->LoadOverlay(path)
                             : runtimePanel->LoadOverlay(path);
            } else if (ok && kind == "overlayoffset") {
                const double tsec = parts.value(2).toDouble();
                if (simView) simView->SetOverlayOffset(tsec); else runtimePanel->SetOverlayOffset(tsec);
            } else if (ok) {
                if (simView) simView->ClearOverlay(); else runtimePanel->ClearOverlay();
            }
            if (!ok) std::cerr << "smoke: overlay op failed " << kind.toStdString() << " " << arg.toStdString() << "\n";
            else std::cout << kind.toStdString() << " " << arg.toStdString() << "=ok\n";
        } else if (kind == "dumpstatus") {
            // The main-window status bar's current message (build refusals,
            // post-flash hints, ...).
            std::cout << "status=" << window->statusBar()->currentMessage().toStdString() << "\n";
        } else if (kind == "dumpmenu") {
            // "text [shortcut] | ..." for every action of the named menu —
            // menu-membership assertions without opening the popup.
            if (QMenu* menu = window->findChild<QMenu*>(arg)) {
                QStringList entries;
                for (QAction* action : menu->actions()) {
                    if (action->isSeparator()) {
                        entries.push_back(QStringLiteral("--"));
                        continue;
                    }
                    QString entry = action->text();
                    if (!action->shortcut().isEmpty()) {
                        entry += QStringLiteral(" [") + action->shortcut().toString()
                                 + QStringLiteral("]");
                    }
                    entries.push_back(entry);
                }
                std::cout << arg.toStdString() << "="
                          << entries.join(QStringLiteral(" | ")).toStdString() << "\n";
            } else {
                std::cerr << "smoke: no menu named " << arg.toStdString() << "\n";
            }
        } else if (kind == "open") {
            if (!window->OpenGraph(arg.toStdString())) {
                std::cerr << "smoke: open failed " << arg.toStdString() << "\n";
            }
        } else if (kind == "loadsession") {
            const bool ok = window->SmokeLoadSimSession(
                QString::fromStdString(arg.toStdString()));
            if (!ok) std::cerr << "smoke: loadsession failed " << arg.toStdString() << "\n";
        } else if (kind == "saveas") {
            const QString err = window->SmokeSaveActiveAs(arg);
            if (!err.isEmpty()) std::cerr << "smoke: saveas failed " << err.toStdString() << "\n";
        } else if (kind == "wait") {
            QThread::msleep(static_cast<unsigned long>(arg.toInt()));
        } else if (kind == "grab") {
            window->grab().save(QString::fromStdString(dir.string() + "/" + arg.toStdString() + ".png"));
        } else if (kind == "exit") {
            QApplication::exit(arg.toInt());
        }
    });
    timer->start();
}

void RunSmokeSequence(NodeGUI::MainWindow* window, const std::filesystem::path& dir) {
    if (const char* actions = std::getenv("RTE_SMOKE_ACTIONS"); actions && *actions) {
        RunSmokeActions(window, dir, actions);
        return;
    }
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
        // A desktop double-click gives the user nothing while the CLI
        // answer is perfect for it: show a small alert on graphical
        // platforms only (headless/offscreen runs keep the plain exit so
        // the smoke harness isn't blocked by a modal).
        const QString platform = qgetenv("QT_QPA_PLATFORM");
        if (platform.isEmpty()) {
            QMessageBox alert(QMessageBox::Information,
                              QStringLiteral("RTE Studio"),
                              QStringLiteral(
                                  "RTE Studio is already running.\n\n"
                                  "Use the existing window."),
                              QMessageBox::Ok);
            alert.exec();
        }
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
