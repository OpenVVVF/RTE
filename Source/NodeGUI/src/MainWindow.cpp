#include "MainWindow.h"

#include "FrameRateMonitor.h"
#include "InspectorPanel.h"
#include "NodePalette.h"

#include "runtime/ConsolePanel.h"
#include "runtime/RuntimeTab.h"
#include "runtime/SignalTablePanel.h"

#include <QCoreApplication>
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QCursor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMetaObject>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QScreen>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWidget>

#include <filesystem>
#include <set>

namespace NodeGUI {

namespace {

std::string DefaultTemplatesDir() {
#ifdef RTE_NODE_TEMPLATES_DIR
    return RTE_NODE_TEMPLATES_DIR;
#else
    return (std::filesystem::current_path() / "Assets" / "NodeTemplates").string();
#endif
}

std::string FindSpwmDemoGraph() {
    namespace fs = std::filesystem;
    const fs::path rel = fs::path("Images") / "HostSim" / "graphs" / "spwm_demo_graph.json";
    std::vector<fs::path> roots;
    roots.push_back(fs::current_path());
    if (auto exe = QCoreApplication::applicationDirPath(); !exe.isEmpty()) {
        roots.push_back(fs::path(exe.toStdString()));
        auto p = fs::path(exe.toStdString());
        for (int i = 0; i < 6; ++i) {
            p = p.parent_path();
            if (p.empty() || p == p.parent_path()) {
                break;
            }
            roots.push_back(p);
        }
    }
    for (const fs::path& root : roots) {
        const fs::path candidate = root / rel;
        if (fs::exists(candidate)) {
            return candidate.string();
        }
    }
    return {};
}

// Native dialogs (QMessageBox statics and, on this system, QMessageBox in
// general) crash in teardown under the KDE platform theme. A plain QDialog
// does not take the native path and is safe. Errors go to the canvas toast
// instead of a modal box.
bool AskYesNo(QWidget* parent, const QString& title, const QString& text) {
    QDialog dialog(parent);
    dialog.setWindowTitle(title);

    auto* layout = new QVBoxLayout(&dialog);
    auto* label = new QLabel(text, &dialog);
    label->setWordWrap(true);
    layout->addWidget(label);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Yes | QDialogButtonBox::No,
                                         &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    return dialog.exec() == QDialog::Accepted;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , graphScene_{std::make_unique<GraphScene>()} {
    setWindowTitle(QStringLiteral("NodeGUI"));
    resize(1280, 800);

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);

    view_ = new GraphView(graphScene_->Scene());
    view_->installEventFilter(this);

    // Note: measured on the target machine, a QOpenGLWidget viewport was ~3x
    // slower than the default raster viewport for this scene, so we keep the
    // raster path. Node-level DeviceCoordinateCache (set in GraphScene) is
    // what keeps pans and drags smooth.
    view_->setOptimizationFlags(QGraphicsView::DontAdjustForAntialiasing);

    // Node type dropped from the palette: instantiate it at the drop point.
    view_->onNodeTypeDropped = [this](const QString& typeId, const QPointF& scenePos) {
        const QString error = graphScene_->AddNodeAt(typeId.toStdString(), scenePos);
        if (error.isEmpty()) {
            UpdateStatus();
        } else {
            ShowToast(error);
        }
    };

    // Right-click on a node: offer the domain selection menu.
    view_->onNodeContextMenu = [this](const QPointF& globalPos, QtNodes::NodeId qtId) {
        ShowNodeDomainMenu(globalPos, qtId);
    };

    layout->addWidget(view_);

    // Tabs: the node editor (existing content) and the runtime view
    // (telemetry + flashing), added by SetupRuntime().
    tabs_ = new QTabWidget(this);
    tabs_->addTab(central, QStringLiteral("Node Editor"));
    setCentralWidget(tabs_);

    StripBrokenSceneActions();

    // Toast-style warning label, bottom-left of the canvas. Child of the
    // container (not the viewport, whose children get dragged by scroll
    // deltas), raised above the view, hidden until a refused action.
    toast_ = new QLabel(central);
    toast_->setStyleSheet(QStringLiteral(
        "background-color: rgba(30, 30, 30, 200);"
        "border-radius: 4px;"
        "padding: 4px 8px;"
        "color: #ffb74d;"));
    toast_->hide();
    toast_->raise();

    toastTimer_ = new QTimer(this);
    toastTimer_->setSingleShot(true);
    toastTimer_->setInterval(4000);
    connect(toastTimer_, &QTimer::timeout, toast_, &QLabel::hide);

    // The viewport's Resize event carries the final post-layout size (and
    // covers scrollbar appear/disappear).
    view_->viewport()->installEventFilter(this);

    // Toolbox dock listing the available node types; entries are dragged onto
    // the view to instantiate them. Populated at startup (below) and on
    // OpenGraph.
    palette_ = new NodePalette(this);
    toolboxDock_ = new QDockWidget(QStringLiteral("Node Toolbox"), this);
    toolboxDock_->setObjectName(QStringLiteral("nodeToolboxDock"));
    toolboxDock_->setWidget(palette_);
    addDockWidget(Qt::LeftDockWidgetArea, toolboxDock_);

    // Inspector dock reflecting the selected node (replaces the old
    // double-click properties popup).
    inspector_ = new InspectorPanel(this);
    inspector_->Attach(graphScene_.get());
    inspector_->onError = [this](const QString& message) { ShowToast(message); };
    inspectorDock_ = new QDockWidget(QStringLiteral("Inspector"), this);
    inspectorDock_->setObjectName(QStringLiteral("inspectorDock"));
    inspectorDock_->setWidget(inspector_);
    addDockWidget(Qt::RightDockWidgetArea, inspectorDock_);

    // Each tab screen has its own set of dockable panels; switching tabs
    // saves/restores the dock layout and rebuilds the View menu.
    connect(tabs_, &QTabWidget::currentChanged, this, &MainWindow::OnTabChanged);

    // Load the node-type templates right away so the toolbox and node
    // creation work before any graph file is opened.
    const QString templatesError = graphScene_->LoadTemplates(DefaultTemplatesDir());
    if (templatesError.isEmpty()) {
        if (view_) {
            view_->setScene(graphScene_->Scene());
            StripBrokenSceneActions();
        }
        palette_->SetNodeTypes(graphScene_->Graph().GetNodeTypes());
        ConnectModelSignals();
    } else {
        ShowToast(templatesError);
    }

#ifdef NODEGUI_ENABLE_FPS_OVERLAY
    // Top-right FPS / frametime overlay. The monitor parents the label to the
    // view itself and anchors it to the viewport geometry so it stays put when
    // the view is resized, moved, or reparented.
    (void)new FrameRateMonitor(view_, this);
#endif

    SetupMenu();
    UpdateStatus();

    if (QScreen* screen = QApplication::primaryScreen()) {
        move(screen->availableGeometry().center() - frameGeometry().center());
    }
}

void MainWindow::SetupRuntime(const QString& serialPort,
                              bool simulate,
                              runtime::Protocol protocol,
                              const QString& tcpHost,
                              int tcpPort) {
    runtimeController_ =
        std::make_unique<runtime::RuntimeController>(serialPort, simulate, protocol,
                                                     tcpHost, tcpPort);
    firmwareUpdater_ = std::make_unique<runtime::FirmwareUpdater>();
    httpApiServer_ = std::make_unique<runtime::HttpApiServer>(*firmwareUpdater_,
                                                              runtimeController_->Store());

    firmwareUpdater_->setSuspendCallback([this](bool suspend) {
        if (suspend) {
            runtimeController_->SuspendForFlash();
        } else {
            runtimeController_->ResumeAfterFlash();
        }
    });
    firmwareUpdater_->setCurrentPort(serialPort.toStdString());

    httpApiServer_->setDevicePort(serialPort.toStdString());
    httpApiServer_->setCommandHandler(
        [this](const std::string& cmd) { return runtimeController_->SendCommandRaw(cmd); });

    runtimeTab_ = new runtime::RuntimeTab(runtimeController_.get(),
                                          firmwareUpdater_.get(),
                                          httpApiServer_.get(),
                                          this);
    tabs_->addTab(runtimeTab_, QStringLiteral("Runtime"));

    // Runtime-screen docks:
    // while the Node Editor tab is active (see OnTabChanged).
    signalTableDock_ = new QDockWidget(QStringLiteral("Signal Table"), this);
    signalTableDock_->setObjectName(QStringLiteral("signalTableDock"));
    signalTableDock_->setWidget(runtimeTab_->GetSignalTable());
    addDockWidget(Qt::LeftDockWidgetArea, signalTableDock_);
    signalTableDock_->hide();

    telemetryConsoleDock_ = new QDockWidget(QStringLiteral("Console"), this);
    telemetryConsoleDock_->setObjectName(QStringLiteral("telemetryConsoleDock"));
    telemetryConsoleDock_->setWidget(runtimeTab_->GetConsole());
    addDockWidget(Qt::LeftDockWidgetArea, telemetryConsoleDock_);
    telemetryConsoleDock_->hide();

    // Node-screen console: same device console, docked at the bottom of the
    // Node Editor screen, off by default — enabled via the View menu.
    editorConsoleDock_ = new QDockWidget(QStringLiteral("Console"), this);
    editorConsoleDock_->setObjectName(QStringLiteral("editorConsoleDock"));
    editorConsoleDock_->setWidget(
        new runtime::ConsolePanel(runtimeController_.get(), editorConsoleDock_));
    addDockWidget(Qt::BottomDockWidgetArea, editorConsoleDock_);
    editorConsoleDock_->hide();

    runtimeController_->Start();
    httpApiServer_->start();

    // The View menu gains the new docks' toggle actions.
    RebuildViewMenu();
}

bool MainWindow::OpenGraph(const std::string& path) {
    // Detach the old scene before LoadGraph destroys it and recreates it.
    if (view_) {
        view_->setScene(nullptr);
    }

    const QString error = graphScene_->LoadGraph(path, DefaultTemplatesDir());
    if (!error.isEmpty()) {
        ShowToast(QStringLiteral("Failed to load graph: %1").arg(error));
        return false;
    }

    if (view_) {
        view_->setScene(graphScene_->Scene());
        StripBrokenSceneActions();
    }

    palette_->SetNodeTypes(graphScene_->Graph().GetNodeTypes());

    ConnectModelSignals();

    currentPath_ = path;
    setWindowTitle(QStringLiteral("NodeGUI - %1").arg(QString::fromStdString(path)));
    UpdateStatus();
    return true;
}

void MainWindow::OnNew() {
    if (!AskYesNo(this,
                  QStringLiteral("New Graph"),
                  QStringLiteral("Discard the current graph and start a new empty one?"))) {
        return;
    }

    if (view_) {
        view_->setScene(nullptr);
    }
    const QString error = graphScene_->NewGraph();
    if (!error.isEmpty()) {
        ShowToast(error);
    }
    if (view_) {
        view_->setScene(graphScene_->Scene());
        StripBrokenSceneActions();
    }

    ConnectModelSignals();
    currentPath_.clear();
    setWindowTitle(QStringLiteral("NodeGUI"));
    UpdateStatus();
}

void MainWindow::StripBrokenSceneActions() {
    if (!view_) {
        return;
    }
    const QList<QKeySequence> broken = {
        QKeySequence(QKeySequence::Undo),
        QKeySequence(QKeySequence::Redo),
        QKeySequence(QKeySequence::Paste),
        QKeySequence(Qt::CTRL | Qt::Key_D),
    };
    QList<QAction*> toRemove;
    for (QAction* action : view_->actions()) {
        for (const QKeySequence& shortcut : action->shortcuts()) {
            if (broken.contains(shortcut)) {
                toRemove.push_back(action);
                break;
            }
        }
    }
    for (QAction* action : toRemove) {
        // Only unregister: GraphicsView owns these and deletes them itself on
        // the next setScene. Deleting here would leave it with a dangling
        // pointer and double-free.
        view_->removeAction(action);
    }
}

void MainWindow::SetupMenu() {
    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));

    QAction* newAction = fileMenu->addAction(QStringLiteral("&New"));
    newAction->setShortcuts(QKeySequence::New);
    connect(newAction, &QAction::triggered, this, &MainWindow::OnNew);

    QAction* openAction = fileMenu->addAction(QStringLiteral("&Open..."));
    openAction->setShortcuts(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::OnOpen);

    QAction* saveAction = fileMenu->addAction(QStringLiteral("&Save"));
    saveAction->setShortcuts(QKeySequence::Save);
    connect(saveAction, &QAction::triggered, this, &MainWindow::OnSave);

    QAction* saveAsAction = fileMenu->addAction(QStringLiteral("Save &As..."));
    saveAsAction->setShortcuts(QKeySequence::SaveAs);
    connect(saveAsAction, &QAction::triggered, this, &MainWindow::OnSaveAs);

    fileMenu->addSeparator();

    QAction* spwmAction = fileMenu->addAction(QStringLiteral("Open SPWM &Demo Graph..."));
    spwmAction->setToolTip(QStringLiteral(
        "Load Images/HostSim/graphs/spwm_demo_graph.json. "
        "Run: powershell -File Images\\HostSim\\scripts\\run_spwm_live.ps1"));
    connect(spwmAction, &QAction::triggered, this, &MainWindow::OnOpenSpwmDemo);

    fileMenu->addSeparator();

    QAction* exitAction = fileMenu->addAction(QStringLiteral("E&xit"));
    exitAction->setShortcuts(QKeySequence::Quit);
    connect(exitAction, &QAction::triggered, this, &MainWindow::OnExit);

    QMenu* viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    viewMenu_ = viewMenu;

    arrangeAction_ = new QAction(QStringLiteral("&Auto Arrange"), this);
    arrangeAction_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+A")));
    connect(arrangeAction_, &QAction::triggered, this, &MainWindow::OnAutoArrange);

    RebuildViewMenu();
}

void MainWindow::RebuildViewMenu() {
    if (!viewMenu_) {
        return;
    }
    viewMenu_->clear();

    const bool nodeTab = !tabs_ || tabs_->currentIndex() == 0;
    if (nodeTab) {
        viewMenu_->addAction(arrangeAction_);
        viewMenu_->addSeparator();
        for (auto* dock : {toolboxDock_.data(), inspectorDock_.data(),
                           editorConsoleDock_.data()}) {
            if (dock) {
                viewMenu_->addAction(dock->toggleViewAction());
            }
        }
    } else {
        for (auto* dock : {signalTableDock_.data(), telemetryConsoleDock_.data()}) {
            if (dock) {
                viewMenu_->addAction(dock->toggleViewAction());
            }
        }
    }
}

void MainWindow::OnTabChanged(int index) {
    // Preserve each screen's dock layout (which panels, where) across
    // switches; the first visit of a screen gets its default arrangement.
    if (previousTab_ >= 0 && previousTab_ < 2) {
        screenStates_[previousTab_] = saveState();
    }
    previousTab_ = index;

    if (index >= 0 && index < 2 && !screenStates_[index].isEmpty()) {
        restoreState(screenStates_[index]);
    } else {
        // Defaults: node screen shows toolbox + inspector; runtime screen
        // shows the signal table + telemetry console. Panels from the other
        // screen and the (opt-in) node-screen console stay hidden.
        const bool nodeTab = (index == 0);
        if (toolboxDock_) toolboxDock_->setVisible(nodeTab);
        if (inspectorDock_) inspectorDock_->setVisible(nodeTab);
        if (editorConsoleDock_) editorConsoleDock_->setVisible(false);
        if (signalTableDock_) signalTableDock_->setVisible(!nodeTab);
        if (telemetryConsoleDock_) telemetryConsoleDock_->setVisible(!nodeTab);
    }

    RebuildViewMenu();
}

bool MainWindow::DoSave(const std::string& path) {
    graphScene_->SyncPositionsFromScene();

    const QString error = graphScene_->SaveGraph(path);
    if (!error.isEmpty()) {
        ShowToast(QStringLiteral("Failed to save graph: %1").arg(error));
        return false;
    }

    currentPath_ = path;
    setWindowTitle(QStringLiteral("NodeGUI - %1").arg(QString::fromStdString(path)));
    return true;
}

void MainWindow::ShowNodeDomainMenu(const QPointF& globalPos, QtNodes::NodeId qtId) {
    const std::string nodeId = graphScene_->NodeApiId(qtId);
    const auto node = nodeId.empty() ? std::nullopt : graphScene_->Graph().FindNode(nodeId);
    if (!node) {
        return;
    }

    QMenu menu(this);

    QAction* renameAction = menu.addAction(QStringLiteral("Rename node..."));
    connect(renameAction, &QAction::triggered, this, [this, qtId, nodeId] {
        bool ok = false;
        const QString name = QInputDialog::getText(this,
                                                   QStringLiteral("Rename Node"),
                                                   QStringLiteral("Node id:"),
                                                   QLineEdit::Normal,
                                                   QString::fromStdString(nodeId),
                                                   &ok);
        if (ok && !name.trimmed().isEmpty()) {
            const QString error = graphScene_->RenameNode(qtId, name.trimmed().toStdString());
            if (!error.isEmpty()) {
                ShowToast(error);
            }
        }
    });

    menu.addSeparator();

    // Types locked to a domain cannot be reassigned; say so in place.
    const auto nodeType = graphScene_->Graph().FindNodeType(node->type);
    if (nodeType && !nodeType->domain.empty()) {
        QAction* locked = menu.addAction(QStringLiteral("Locked to domain '%1' by its node type")
                                             .arg(QString::fromStdString(nodeType->domain)));
        locked->setEnabled(false);
        menu.exec(globalPos.toPoint());
        return;
    }

    // Existing domains in the graph, plus "no domain" and a custom entry.
    std::set<std::string> domains;
    for (const auto& n : graphScene_->Graph().GetNodes()) {
        if (!n.domain.empty()) {
            domains.insert(n.domain);
        }
    }
    auto addDomainAction = [&](const QString& label, const std::string& value) {
        QAction* action = menu.addAction(label);
        action->setCheckable(true);
        action->setChecked(node->domain == value);
        connect(action, &QAction::triggered, this, [this, qtId, value] {
            const QString error = graphScene_->SetNodeDomain(qtId, value);
            if (!error.isEmpty()) {
                ShowToast(error);
            }
        });
    };

    addDomainAction(QStringLiteral("(no domain)"), "");
    for (const auto& domain : domains) {
        addDomainAction(QString::fromStdString(domain), domain);
    }

    menu.addSeparator();
    QAction* customAction = menu.addAction(QStringLiteral("New domain..."));
    connect(customAction, &QAction::triggered, this, [this, qtId] {
        bool ok = false;
        const QString name = QInputDialog::getText(this,
                                                   QStringLiteral("Set Domain"),
                                                   QStringLiteral("Domain name:"),
                                                   QLineEdit::Normal,
                                                   {},
                                                   &ok);
        if (ok && !name.trimmed().isEmpty()) {
            const QString error = graphScene_->SetNodeDomain(qtId, name.trimmed().toStdString());
            if (!error.isEmpty()) {
                ShowToast(error);
            }
        }
    });

    menu.exec(globalPos.toPoint());
}

void MainWindow::ShowToast(const QString& message) {
    // Instant tooltip at the cursor at the moment of refusal, plus the
    // persistent (few seconds) toast in the canvas corner.
    QToolTip::showText(QCursor::pos(), message);

    toast_->setText(message);
    toast_->adjustSize();
    RepositionToast();
    toast_->show();
    toast_->raise();
    toastTimer_->start();  // restarts if already running
}

void MainWindow::RepositionToast() {
    if (!toast_ || !view_) {
        return;
    }
    constexpr int margin = 12;
    QWidget* container = toast_->parentWidget();
    const QPoint viewportBottomLeft =
        view_->viewport()->mapTo(container, QPoint(0, view_->viewport()->height()));
    toast_->move(viewportBottomLeft.x() + margin,
                 viewportBottomLeft.y() - toast_->height() - margin);
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    RepositionToast();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (runtimeTab_) {
        runtimeTab_->SaveAutosave();
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::UpdateStatus() {
    const auto& graph = graphScene_->Graph();
    const QString message =
        QStringLiteral("Nodes: %1 | Connections: %2 | Bridges: %3")
            .arg(graph.GetNodes().size())
            .arg(graph.GetConnections().size())
            .arg(graph.GetBridges().size());
    statusBar()->showMessage(message);
}

void MainWindow::OnOpenSpwmDemo() {
    const std::string path = FindSpwmDemoGraph();
    if (path.empty()) {
        ShowToast(QStringLiteral(
            "SPWM graph not found. Run from the repo root or use "
            "Images/HostSim/scripts/run_spwm_live.ps1"));
        OnOpen();
        return;
    }
    if (!OpenGraph(path)) {
        ShowToast(QStringLiteral("Failed to open SPWM demo graph"));
    } else {
        tabs_->setCurrentIndex(0);
        ShowToast(QStringLiteral(
            "SPWM graph loaded. Start live sim: "
            "powershell -File Images\\HostSim\\scripts\\run_spwm_live.ps1"));
    }
}

void MainWindow::OnOpen() {
    const QString fileName = QFileDialog::getOpenFileName(this,
                                                          QStringLiteral("Open Graph"),
                                                          QString{},
                                                          QStringLiteral("JSON (*.json)"));
    if (!fileName.isEmpty()) {
        OpenGraph(fileName.toStdString());
    }
}

void MainWindow::OnSave() {
    if (currentPath_.empty()) {
        OnSaveAs();
        return;
    }
    DoSave(currentPath_);
}

void MainWindow::OnSaveAs() {
    const QString fileName = QFileDialog::getSaveFileName(this,
                                                          QStringLiteral("Save Graph"),
                                                          QString{},
                                                          QStringLiteral("JSON (*.json)"));
    if (!fileName.isEmpty()) {
        DoSave(fileName.toStdString());
    }
}

void MainWindow::OnAutoArrange() {
    graphScene_->AutoArrange();
}

void MainWindow::OnExit() {
    close();
}

void MainWindow::ConnectModelSignals() {
    if (!graphScene_->Model()) {
        return;
    }

    connect(graphScene_->Model(),
            &QtNodes::AbstractGraphModel::connectionCreated,
            this,
            [this]() {
                connectionCreatedThisDrag_ = true;
                graphScene_->Model()->ClearRejectionState();
            });

    // The inspector follows the selection.
    inspector_->Clear();
    connect(graphScene_->Scene(),
            &QGraphicsScene::selectionChanged,
            this,
            &MainWindow::OnSceneSelectionChanged);
}

void MainWindow::OnSceneSelectionChanged() {
    if (!graphScene_->Scene()) {
        inspector_->Clear();
        return;
    }
    for (QGraphicsItem* item : graphScene_->Scene()->selectedItems()) {
        if (auto* ngo = qgraphicsitem_cast<QtNodes::NodeGraphicsObject*>(item)) {
            inspector_->ShowNode(ngo->nodeId());
            return;
        }
    }
    inspector_->Clear();
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    // Mouse and resize events are delivered to the viewport, not the view.
    if (view_ && watched == view_->viewport()) {
        if (event->type() == QEvent::Resize) {
            // Keep the toast anchored (covers post-layout sizing and
            // scrollbar appear/disappear).
            RepositionToast();
            return false;
        }

        if (event->type() == QEvent::MouseButtonPress) {
            connectionCreatedThisDrag_ = false;
            if (graphScene_->Model()) {
                graphScene_->Model()->ClearRejectionState();
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            if (!connectionCreatedThisDrag_ && graphScene_->Model()) {
                // Defer the check until QtNodes has finished processing the
                // release, so connectionCreated has already been emitted on
                // success.
                QMetaObject::invokeMethod(this,
                                          &MainWindow::CheckForRejectionReason,
                                          Qt::QueuedConnection);
            }
        }
        return false;
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::CheckForRejectionReason() {
    if (connectionCreatedThisDrag_) {
        return;
    }

    if (NodeGraphModel* model = graphScene_->Model()) {
        const QString reason = model->TakeLastRejectionReason();
        if (!reason.isEmpty()) {
            ShowToast(reason);
        }
    }
}

}  // namespace NodeGUI
