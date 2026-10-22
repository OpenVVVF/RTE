// RTE Studio entry point: single-instance lock, runtime wiring, and the
// headless smoke driver (RTE_SMOKE_DIR / RTE_SMOKE_ACTIONS, see below).

// ---- GUI architecture notes (routing + node glyph layer) -----------------
//
// Manual wire routing ("waypoints"):
//   One wire behavior: every connection renders rectilinear - the rigid
//   orthogonal default (WireRoute::BuildRoute) for un-routed wires, the
//   waypoint route once the user places manual bends; the QtNodes smooth
//   cubic is gone from the painter, so untouched graphs still save
//   byte-identical. User bends live ONLY in the GUI layer
//   (NodeGraphModel::connectionWaypoints_,
//   keyed by QtNodes connection id, scene coordinates); NodeAPI never sees
//   them. GraphScene merges them into the document on save/snapshot as an
//   OPTIONAL per-connection key, and re-applies them on load/undo:
//       {"id":"c3","from":{...},"to":{...},
//        "waypoints":[{"x":120.0,"y":40.0}, ...]}
//   Unknown keys here are safe: NodeAPI's ConnectionFromJson/BridgeFromJson
//   read named fields only, and both code generators consume the parsed
//   Graph object. Wires with bends render rectilinear (WireRoute::BuildRoute:
//   horizontal port stubs, single elbows per leg); the auto-elbow only exists
//   at paint time and is never serialized. Wire-follow keeps bends riding
//   with their nodes (NodeGraphModel::FollowMovedNodes, fed at every
//   drag-settle by GraphScene): bends of every bend-carrying connection
//   attached to a moved node translate by that node's delta, labeled
//   connections skipped; a connection whose two anchors move by the same
//   delta (selection/area drag) translates exactly once — the routed shape
//   moves rigidly with the selection instead of spiking across freed rows.
//   Gestures (GraphView): drag a wire
//   to insert/move a bend (grid-snapped, dominant-axis lock), Ctrl+click or
//   right-click a bend handle to delete it. The Edit-menu route.addPoint /
//   route.removePoint verbs (also smoke-clickable) bend/remove on the single
//   selected wire. Foldback discipline lives in two places, neither in the
//   assets: the live bend drag clamps the dragged bend between its two
//   neighbor columns on the corridor-dominant axis (UpdateWireDrag: first
//   bend vs the out port, last vs the in port), so a gesture can never fold
//   the control chain back over its own track; and BuildRoute's
//   DropFoldbacks fixpoint cleanses produced routes of zero-area U-turn
//   micro segments (a short return leg landing inside the span just walked)
//   so loaded/legacy folds cannot paint overlapping back-tracking segments
//   either. BridgeConnectionPainter::routeBounds extends the item
//   bounding rect via a small QtNodes hook (AbstractConnectionPainter::
//   routeBounds) so routed wires repaint correctly.
//
// Display-only net labels ("netLabel"):
//   Optional per-connection text label — KiCad-style: a labeled wire renders
//   as a short exit stub per port (3 grid cells, toward the peer) terminated
//   by a tag chip with the name, instead of the long route; hit selection
//   covers stubs + chips. Display layer only: the connection stays pairwise
//   and NodeAPI/codegen/simulation never see labels (storage parallels
//   connectionWaypoints_ in NodeGraphModel, saved as an optional
//   "netLabel":"text" key beside "waypoints"). A non-empty label is UNIQUE
//   across all connections and bridges — a connection IS the net — enforced
//   at assignment (GraphScene::SetConnectionLabel refuses taken names;
//   inspector field reverts + toast). Edit seams: Inspector "Net label"
//   field (select a wire), Ctrl+double-click on a wire (instance-modal
//   QInputDialog), or the smoke verbs netlabel/dumpconn.
//   Chips are movable: a left press inside a chip (chip beats the wire
//   stroke on overlap — ChipItemAt in GraphView) starts the chip drag;
//   moves displace BOTH chips by one grid-snapped scene vector clamped to
//   25 grid cells (stubs stay anchored, no leader line), release commits
//   one undo entry. The displacement drips as an additive
//   "netLabelOffset":[dx,dy] sibling of "netLabel" (absent at the anchor,
//   dropped when the label clears); chips ride along when their endpoint
//   nodes move because the anchor is the live stub end. Reset paths:
//   Inspector "Reset label position", or `chipdrag <id> reset`. Smoke
//   verbs: chipdrag/dumpchip.
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
//
// Sim value tags ("graph values" mode, Simulink-style):
//   With a sim session on the Simulate screen (run or "Load session..."),
//   the editor canvases can draw each telemetry channel's value as a small
//   pill just off the publishing port's edge (wire side). Binding authority
//   is the Debug.TelemetryLog nodes: Key param == channel name, value ties
//   to the node's upstream OUTPUT socket when its input is wired, else to
//   its own INPUT port (GraphScene::TelemetryTagBindings). Rendering is a
//   dedicated scene layer (GraphValuesOverlay; one dirty-gated item per
//   binding, z above nodes/wires) - no per-node style surgery. Values
//   follow the sim cursor machinery (SimTelemetryView + ValueAt/LatestValue
//   on the retained store; same source as the Value column) on a 4 Hz
//   refresh. Guard policy keeps the tags honest: the Node Editor binds only
//   when its document is the scenario's controller graph (path equality;
//   the standalone-loaded-session fallback binds the controller surface
//   alone), the Plant Editor only under an exact plant-path match, and
//   everything hides when the mode is off or no session is loaded. Each
//   chip is an LTspice-style probe: channel-name row, value line, and a
//   sparkline strip of the channel's waveform over the sim plot view window
//   (decimated from the retained store, wire-hue polyline on a dark track,
//   light tick at the pinned cursor's window fraction; single click assigns
//   the channel onto the sim plot grid, first-free-graph order). Toggle:
//   View > Graph Signal Values (persisted via the showGraphValues app
//   preference, editable in Preferences); smoke verbs gvals
//   on|off|dump|sparkdump (dump prints "key=val;..." of the ACTIVE editor
//   scene; sparkdump one "key:npoints,wmin,wmax[,cursor=frac]" line per
//   tag, a trailing "sparkdump=end" marker included).
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
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLockFile>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QSlider>
#include <QSplitter>
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
        } else if (kind == "addnode") {
            // Instantiates a node type at a scene position through the real
            // creation seam (AddNodeAt), so the grid-snap pref applies.
            const QString result =
                window->SmokeAddNode(arg, parts.value(2).toDouble(), parts.value(3).toDouble());
            std::cout << "addnode " << arg.toStdString() << "=" << result.toStdString() << "\n";
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
        } else if (kind == "netlabel") {
            // netlabel <connGraphId> [text...] — set (empty text clears).
            // Prints "ok" or "refused <reason>" (labels are unique across
            // connections: a held name cannot be re-assigned elsewhere).
            const QString text = QStringList(parts.mid(2)).join(' ');
            std::cout << "netlabel " << arg.toStdString() << "="
                      << window->SmokeSetConnectionLabel(arg.toStdString(), text)
                             .toStdString()
                      << "\n";
        } else if (kind == "dumpconn") {
            // "label=<text|—>;layer=<z>;wp=[x,y;...]" single line for
            // checkers (wp = manual bends, "[]" when auto-routed).
            std::cout << window->SmokeDumpConnection(arg.toStdString()).toStdString();
        } else if (kind == "chipdrag") {
            // chipdrag <connGraphId> <dx> <dy> | chipdrag <connGraphId> reset —
            // REAL press/drag/release viewport events starting inside the
            // labeled connection's out chip (the full hit dispatch: chip
            // beats wire stroke; chip-drag state machine; snap+clamp;
            // undo-covered commit). The delta is scene-space. "reset" clears
            // the saved chip offset (inspector "Reset label position" seam).
            // Prints ok | no label (unlabeled connection) | failed.
            if (parts.value(2) == QLatin1String("reset")) {
                std::cout << "chipdrag " << arg.toStdString() << "="
                          << window->SmokeResetChipLabel(arg.toStdString())
                                 .toStdString()
                          << "\n";
            } else {
                std::cout << "chipdrag " << arg.toStdString() << "="
                          << window->SmokeDragChipLabel(arg.toStdString(),
                                                        parts.value(2).toDouble(),
                                                        parts.value(3).toDouble())
                                 .toStdString()
                          << "\n";
            }
        } else if (kind == "dumpchip") {
            // "label=<text|—>;offset=dx,dy|—;outchip=[x,y,w,h];
            // inchip=[x,y,w,h]" single line for checkers (offset/chip fields
            // print "—" on an unlabeled connection).
            std::cout << window->SmokeDumpChipInfo(arg.toStdString()).toStdString();
        } else if (kind == "dumproute") {
            // "route=[x,y;...]" rendered route polyline (scene coords) —
            // the foldback checker's read-out for bend-drag cases.
            std::cout << window->SmokeDumpConnectionRoute(arg.toStdString()).toStdString();
        } else if (kind == "wirebenddrag") {
            // Drag bend <index> of wire <id> to scene (x, y) through the
            // active view's real bend-drag machinery (guards, axis lock,
            // foldback clip, model commit) — no synthesized mouse events.
            const bool ok = window->SmokeDragWireBend(arg.toStdString(),
                                                      parts.value(2).toInt(),
                                                      parts.value(3).toDouble(),
                                                      parts.value(4).toDouble());
            std::cout << "wirebenddrag " << arg.toStdString() << "="
                      << (ok ? "ok" : "failed") << "\n";
        } else if (kind == "wireparams") {
            // Replace node <id>'s wired-parameter-input list (rest of the
            // step = comma-separated names) — the inspector "wire" commit
            // path whose port-set change re-sizes the node.
            const QString csv = QStringList(parts.mid(2)).join(' ');
            std::cout << "wireparams " << arg.toStdString() << "="
                      << window->SmokeSetNodeParameterInputs(arg.toStdString(), csv)
                             .toStdString()
                      << "\n";
        } else if (kind == "setparam") {
            // setparam <node> <name> [value...] — one parameter write through
            // the same scene seam the inspector's value commit reaches
            // (graph value; node re-sizes with the painted block).
            const QString value = QStringList(parts.mid(3)).join(' ');
            std::cout << "setparam " << arg.toStdString() << "="
                      << window->SmokeSetNodeParameter(arg.toStdString(),
                                                       parts.value(2).toStdString(),
                                                       value)
                             .toStdString()
                      << "\n";
        } else if (kind == "cornerdrag") {
            // REAL corner-grab drag (resize-handle pocket) as synthesized
            // viewport mouse events, through the full event dispatch.
            const bool ok = window->SmokeCornerDragNode(arg.toStdString(),
                                                        parts.value(2).toDouble(),
                                                        parts.value(3).toDouble());
            std::cout << "cornerdrag " << arg.toStdString() << "="
                      << (ok ? "ok" : "failed") << "\n";
        } else if (kind == "cornerpress") {
            const bool ok = window->SmokeCornerPressNode(arg.toStdString());
            std::cout << "cornerpress " << arg.toStdString() << "="
                      << (ok ? "ok" : "failed") << "\n";
        } else if (kind == "cornerrelease") {
            window->SmokeCornerRelease(parts.value(1).toInt(), parts.value(2).toInt());
            std::cout << "cornerrelease=ok\n";
        } else if (kind == "gvals") {
            // gvals on|off|dump|sparkdump — the "graph values" sim value-tag
            // mode. dump prints the ACTIVE editor scene's tags as
            // "key=val;..."; sparkdump prints one "key:npoints,wmin,wmax
            // [,cursor=frac]" line per tag (the LTspice-style chip's
            // sparkline backing; empty when the mode is off or nothing
            // binds).
            if (arg == "on" || arg == "off") {
                const bool ok = window->SmokeSetGraphValuesEnabled(arg == "on");
                std::cout << "gvals=" << arg.toStdString() << (ok ? "" : " failed") << "\n";
            } else if (arg == "dump") {
                std::cout << window->SmokeDumpGraphValues().toStdString() << "\n";
            } else if (arg == "sparkdump") {
                const QString text = window->SmokeDumpGraphSpark();
                if (!text.isEmpty()) {
                    std::cout << text.toStdString() << "\n";
                }
                std::cout << "sparkdump=end\n";
            } else {
                std::cerr << "smoke: gvals wants on|off|dump|sparkdump\n";
            }
        } else if (kind == "chipclick") {
            // chipclick <telemetryLogNodeId> — a real single click in the
            // name row of that log node's LTspice chip on the ACTIVE editor
            // tab (viewport event dispatch; the row dodges the socket-line
            // wire stroke the view's bend-drag interception claims).
            // Assigns the chip's channel onto the sim plot grid,
            // first-free-graph order.
            const bool ok = window->SmokeClickGraphTag(arg.toStdString());
            std::cout << "chipclick " << arg.toStdString() << "="
                      << (ok ? "ok" : "failed") << "\n";
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
        } else if (kind == "restablecol") {
            // Table-column resize poke: restablecol <tableObjectName> <col>
            // <px> drives the same QHeaderView::resizeSection a header-edge
            // drag issues (the column-resize freeze repro + its regression).
            auto* table = window->findChild<QTableWidget*>(arg);
            if (!table) {
                std::cerr << "smoke: no table named " << arg.toStdString() << "\n";
            } else {
                table->horizontalHeader()->resizeSection(parts.value(2).toInt(),
                                                         parts.value(3).toInt());
                std::cout << "restablecol " << arg.toStdString() << "="
                          << table->horizontalHeader()->sectionSize(
                                 parts.value(2).toInt())
                          << "\n";
            }
        } else if (kind == "resgraphcol") {
            // Plot-column resize poke: resgraphcol <sim|runtime> <px> drives
            // the same geometry write a separator drag lands on (sim: the
            // telemetry view's table|plots QSplitter; runtime: the signal
            // table dock vs the central plot stack). The freeze repro's
            // second, layout-driving edge.
            int applied = -1;
            if (arg == "sim") {
                auto* view = window->findChild<NodeGUI::sim::SimTelemetryView*>();
                if (auto* splitter = view ? view->findChild<QSplitter*>() : nullptr) {
                    QList<int> sizes = splitter->sizes();
                    const int total = sizes.value(0) + sizes.value(1);
                    splitter->setSizes({parts.value(2).toInt(),
                                        total - parts.value(2).toInt()});
                    applied = splitter->sizes().value(0);
                } else {
                    std::cerr << "smoke: no sim telemetry splitter\n";
                }
            } else if (arg == "runtime") {
                if (auto* dock = window->findChild<QDockWidget*>(
                        QStringLiteral("signalTableDock"))) {
                    window->resizeDocks({dock}, {parts.value(2).toInt()},
                                        Qt::Horizontal);
                    applied = dock->width();
                } else {
                    std::cerr << "smoke: no signalTableDock\n";
                }
            }
            if (applied >= 0) {
                std::cout << "resgraphcol " << arg.toStdString() << "="
                          << applied << "\n";
            }
        } else if (kind == "moveplot" || kind == "setplotsizes" || kind == "plotpanes") {
            // Plot-pane pokes (sim | runtime): the same surface entries the
            // title-strip reorder drag / splitter handles land on.
            // moveplot <surface> <from> <to> reorders a pane;
            // setplotsizes <surface> <px,px,...> sets per-slot heights;
            // plotpanes <surface> dumps "order=.. heights=.. titles=..".
            NodeGUI::sim::SimTelemetryView* simView = nullptr;
            NodeGUI::runtime::TelemetryPanel* runtimePanel = nullptr;
            if (arg == "sim") {
                simView = window->findChild<NodeGUI::sim::SimTelemetryView*>();
            } else if (arg == "runtime") {
                runtimePanel = window->findChild<NodeGUI::runtime::TelemetryPanel*>();
            }
            if (!simView && !runtimePanel) {
                std::cerr << "smoke: no telemetry surface for " << kind.toStdString()
                          << " " << arg.toStdString() << "\n";
            } else if (kind == "plotpanes") {
                std::cout << "plotpanes " << arg.toStdString() << "="
                          << (simView ? simView->DumpPlotPanes()
                                      : runtimePanel->DumpPlotPanes())
                                 .toStdString()
                          << "\n";
            } else if (kind == "moveplot") {
                const bool ok = simView
                    ? simView->MovePane(parts.value(2).toInt(), parts.value(3).toInt())
                    : runtimePanel->MovePane(parts.value(2).toInt(), parts.value(3).toInt());
                std::cout << "moveplot " << arg.toStdString() << "="
                          << (ok ? "ok" : "failed") << "\n";
            } else {
                QList<int> px;
                for (const QString& part : parts.value(2).split(u',')) {
                    px.push_back(part.toInt());
                }
                const bool ok = simView ? simView->SetPlotSizes(px)
                                        : runtimePanel->SetPlotSizes(px);
                std::cout << "setplotsizes " << arg.toStdString() << "="
                          << (ok ? "ok" : "failed") << "\n";
            }
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
        } else if (kind == "plotdragtitle") {
            // plotdragtitle <surface> <toIndex>: synthesized REAL reorder
            // drag — press on the first plot's title strip, drag to the
            // <toIndex>-th plot's vertical center, release. Exercises the
            // leaf strip hit-test + the surface drag hub the moveplot verb
            // skips. Plot children come out in QObject child order (not
            // splitter order after reorders); the smoke only asserts that
            // exactly one reorder commits and the cursor is untouched.
            QWidget* surface = nullptr;
            if (arg == "sim") {
                surface = window->findChild<NodeGUI::sim::SimTelemetryView*>();
            } else if (arg == "runtime") {
                surface = window->findChild<NodeGUI::runtime::TelemetryPanel*>();
            }
            const QList<NodeGUI::runtime::SignalPlotWidget*> plots = surface
                ? surface->findChildren<NodeGUI::runtime::SignalPlotWidget*>()
                : QList<NodeGUI::runtime::SignalPlotWidget*>{};
            auto* plot = plots.value(0);
            auto* target = plots.value(parts.value(2).toInt());
            if (!plot || !target || plot == target) {
                std::cerr << "smoke: bad panes for plotdragtitle " << arg.toStdString() << "\n";
            } else {
                const QPointF grab(plot->width() / 2.0, 12.0);  // inset in the title strip
                const QPointF g0 = plot->mapToGlobal(grab.toPoint());
                const QPoint targetGlobalF = target->mapToGlobal(target->rect().center());
                const QPointF localAtTarget =
                    plot->mapFromGlobal(targetGlobalF);
                QMouseEvent press(QEvent::MouseButtonPress, grab, grab, g0,
                                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(plot, &press);
                QMouseEvent move(QEvent::MouseMove, localAtTarget, localAtTarget,
                                 targetGlobalF,
                                 Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(plot, &move);
                QMouseEvent release(QEvent::MouseButtonRelease, localAtTarget,
                                    localAtTarget, targetGlobalF,
                                    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(plot, &release);
                std::cout << "plotdragtitle " << arg.toStdString() << "=ok\n";
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
        } else if (kind == "winresize") {
            // winresize <w> <h> — size the main window (layout-dependent
            // smoke assertions: persisted pane heights need real estate).
            window->resize(parts.value(1).toInt(), parts.value(2).toInt());
            std::cout << "winresize=" << window->width() << "x"
                      << window->height() << "\n";
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
