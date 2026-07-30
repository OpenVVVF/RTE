#include "MainWindow.h"

#include "FrameRateMonitor.h"
#include "InspectorPanel.h"
#include "NodePalette.h"

#include "runtime/ConsolePanel.h"
#include "runtime/FlashPanel.h"
#include "runtime/RuntimeTab.h"
#include "runtime/SignalTablePanel.h"

#include <QCoreApplication>
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QCursor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollBar>
#include <QShortcut>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTextCursor>
#include <QTimer>
#include <QToolBar>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWidget>

#include <filesystem>
#include <set>
#include <vector>

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

std::filesystem::path ProjectRoot() {
#ifdef RTE_PROJECT_ROOT
    return RTE_PROJECT_ROOT;
#else
    return std::filesystem::current_path();
#endif
}

[[maybe_unused]] QString BuildKeyForGraph(const std::string& graphPath) {
    const QFileInfo graphInfo(QString::fromStdString(graphPath));
    QString stem = graphInfo.completeBaseName();
    for (qsizetype i = 0; i < stem.size(); ++i) {
        const QChar ch = stem.at(i);
        if (!ch.isLetterOrNumber() && ch != u'_' && ch != u'-') {
            stem[i] = u'_';
        }
    }
    if (stem.isEmpty()) {
        stem = QStringLiteral("graph");
    }

    const QByteArray digest =
        QCryptographicHash::hash(graphInfo.absoluteFilePath().toUtf8(),
                                 QCryptographicHash::Sha1)
            .toHex()
            .left(8);
    return stem + QStringLiteral("-") + QString::fromLatin1(digest);
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

    // The application switcher belongs to the main-window shell, above the
    // docks and central content. The stack below it holds only the central
    // content for each screen.
    auto* switcherBar = new QToolBar(QStringLiteral("Application Switcher"), this);
    switcherBar->setObjectName(QStringLiteral("applicationSwitcherToolBar"));
    switcherBar->setMovable(false);
    switcherBar->setFloatable(false);
    switcherBar->setAllowedAreas(Qt::TopToolBarArea);

    appSwitcher_ = new QTabBar(switcherBar);
    appSwitcher_->setDocumentMode(true);
    appSwitcher_->setDrawBase(true);
    appSwitcher_->setExpanding(false);
    appSwitcher_->setUsesScrollButtons(false);
    appSwitcher_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    appSwitcher_->addTab(QStringLiteral("Node Editor"));
    switcherBar->addWidget(appSwitcher_);
    addToolBar(Qt::TopToolBarArea, switcherBar);

    screens_ = new QStackedWidget(this);
    screens_->addWidget(central);
    setCentralWidget(screens_);

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

    // Each app screen has its own set of dockable panels; switching screens
    // saves/restores the dock layout and rebuilds the View menu.
    connect(appSwitcher_, &QTabBar::currentChanged,
            screens_, &QStackedWidget::setCurrentIndex);
    connect(appSwitcher_, &QTabBar::currentChanged,
            this, &MainWindow::OnTabChanged);

    auto addScreenShortcut = [this](const QKeySequence& key, int screenIndex) {
        auto* shortcut = new QShortcut(key, this);
        connect(shortcut, &QShortcut::activated, this, [this, screenIndex] {
            if (screenIndex < appSwitcher_->count()) {
                appSwitcher_->setCurrentIndex(screenIndex);
            }
        });
    };
    addScreenShortcut(QKeySequence(QStringLiteral("Ctrl+1")), 0);
    addScreenShortcut(QKeySequence(QStringLiteral("Ctrl+2")), 1);
    addScreenShortcut(QKeySequence(QStringLiteral("Ctrl+3")), 2);

    buildProcess_ = new QProcess(this);
    buildProcess_->setProcessChannelMode(QProcess::MergedChannels);
    connect(buildProcess_, &QProcess::readyReadStandardOutput, this, [this] {
        AppendBuildLog(QString::fromLocal8Bit(buildProcess_->readAllStandardOutput()));
    });
    connect(buildProcess_,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
                AppendBuildLog(
                    QString::fromLocal8Bit(buildProcess_->readAllStandardOutput()));
                const bool success =
                    exitStatus == QProcess::NormalExit && exitCode == 0;
                AppendBuildLog(
                    success
                        ? QStringLiteral("\n[finished] Operation completed successfully.\n")
                        : QStringLiteral("\n[finished] Operation failed (exit code %1).\n")
                              .arg(exitCode));
                statusBar()->showMessage(
                    success ? QStringLiteral("Firmware operation completed")
                            : QStringLiteral("Firmware operation failed"),
                    5000);
                SetBuildActionsEnabled(true);
            });
    connect(buildProcess_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            AppendBuildLog(QStringLiteral("\n[error] Could not start build workflow: %1\n")
                               .arg(buildProcess_->errorString()));
            statusBar()->showMessage(QStringLiteral("Could not start firmware operation"), 5000);
            SetBuildActionsEnabled(true);
        }
    });

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

    runtimeTab_ = new runtime::RuntimeTab(runtimeController_.get(), this);
    screens_->addWidget(runtimeTab_);
    appSwitcher_->addTab(QStringLiteral("Runtime"));
    runtimeTab_->LoadAutosave();

    firmwareUpdateTab_ = new runtime::FlashPanel(firmwareUpdater_.get(),
                                                  runtimeController_.get(),
                                                  httpApiServer_.get(),
                                                  this);
    screens_->addWidget(firmwareUpdateTab_);
    appSwitcher_->addTab(QStringLiteral("Firmware Update"));

    // Runtime-screen docks: the signal table and the device console. Hidden
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

    // Node-screen debug dock: device console and build logs share tabs inside
    // one detachable dock. It stays off by default and is enabled via View or
    // automatically when a firmware operation starts.
    editorConsoleDock_ = new QDockWidget(QStringLiteral("Console"), this);
    editorConsoleDock_->setObjectName(QStringLiteral("editorConsoleDock"));
    editorConsoleTabs_ = new QTabWidget(editorConsoleDock_);
    editorConsoleTabs_->addTab(
        new runtime::ConsolePanel(runtimeController_.get(), editorConsoleTabs_),
        QStringLiteral("Console"));

    auto* logsPage = new QWidget(editorConsoleTabs_);
    auto* logsLayout = new QVBoxLayout(logsPage);
    logsLayout->setContentsMargins(0, 0, 0, 0);
    auto* clearLogsButton = new QPushButton(QStringLiteral("Clear Logs"), logsPage);
    connect(clearLogsButton, &QPushButton::clicked, this, [this] {
        if (buildLogView_) {
            buildLogView_->clear();
        }
    });
    logsLayout->addWidget(clearLogsButton, 0, Qt::AlignLeft);
    buildLogView_ = new QPlainTextEdit(logsPage);
    buildLogView_->setReadOnly(true);
    buildLogView_->setMaximumBlockCount(5000);
    logsLayout->addWidget(buildLogView_, 1);
    editorConsoleTabs_->addTab(logsPage, QStringLiteral("Logs"));

    editorConsoleDock_->setWidget(editorConsoleTabs_);
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

    QMenu* buildMenu = menuBar()->addMenu(QStringLiteral("&Build"));

    generateAction_ = buildMenu->addAction(QStringLiteral("&Generate Code"));
    connect(generateAction_, &QAction::triggered, this, [this] {
        StartBuildCommand(BuildCommand::Generate);
    });

    flashAction_ = buildMenu->addAction(QStringLiteral("&Flash"));
    connect(flashAction_, &QAction::triggered, this, [this] {
        StartBuildCommand(BuildCommand::Flash);
    });

    buildMenu->addSeparator();

    generateFlashAction_ = buildMenu->addAction(QStringLiteral("Generate and &Flash"));
    generateFlashAction_->setShortcut(QKeySequence(Qt::Key_F5));
    connect(generateFlashAction_, &QAction::triggered, this, [this] {
        StartBuildCommand(BuildCommand::GenerateAndFlash);
    });

    buildMenu->addSeparator();

    buildSimAction_ = buildMenu->addAction(QStringLiteral("Build &Reflect in Simulator"));
    buildSimAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B));
    buildSimAction_->setToolTip(QStringLiteral("Re-emit graph into HostSim, compile simulator, and restart live telemetry feed"));
    connect(buildSimAction_, &QAction::triggered, this, [this] {
        StartBuildCommand(BuildCommand::BuildSimulation);
    });

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

    const int tabIndex = appSwitcher_ ? appSwitcher_->currentIndex() : 0;
    const bool nodeTab = tabIndex == 0;
    const bool runtimeTab = tabIndex == 1;
    if (nodeTab) {
        viewMenu_->addAction(arrangeAction_);
        viewMenu_->addSeparator();
        for (auto* dock : {toolboxDock_.data(), inspectorDock_.data(),
                           editorConsoleDock_.data()}) {
            if (dock) {
                viewMenu_->addAction(dock->toggleViewAction());
            }
        }
    } else if (runtimeTab) {
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
    if (previousTab_ >= 0 && previousTab_ < 3) {
        screenStates_[previousTab_] = saveState();
    }
    previousTab_ = index;

    if (index >= 0 && index < 3 && !screenStates_[index].isEmpty()) {
        restoreState(screenStates_[index]);
    } else {
        // Defaults: node screen shows toolbox + inspector, runtime shows the
        // signal table + telemetry console, and firmware update has no docks.
        // Panels from the other screens and the opt-in node console stay
        // hidden.
        const bool nodeTab = (index == 0);
        const bool runtimeTab = (index == 1);
        if (toolboxDock_) toolboxDock_->setVisible(nodeTab);
        if (inspectorDock_) inspectorDock_->setVisible(nodeTab);
        if (editorConsoleDock_) editorConsoleDock_->setVisible(false);
        if (signalTableDock_) signalTableDock_->setVisible(runtimeTab);
        if (telemetryConsoleDock_) telemetryConsoleDock_->setVisible(runtimeTab);
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

bool MainWindow::EnsureGraphSaved() {
    if (!currentPath_.empty()) {
        return DoSave(currentPath_);
    }

    const QString fileName = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Save Graph Before Building"),
        QString{},
        QStringLiteral("JSON (*.json)"));
    if (fileName.isEmpty()) {
        return false;
    }
    return DoSave(fileName.toStdString());
}

void MainWindow::ShowBuildLogs() {
    if (!editorConsoleDock_ || !editorConsoleTabs_ || !buildLogView_) {
        ShowToast(QStringLiteral("Build log panel is not available"));
        return;
    }

    // Firmware operations belong to the editor workflow. Select that screen,
    // reveal its detachable debug dock, and focus the Logs sub-tab.
    if (appSwitcher_->currentIndex() != 0) {
        appSwitcher_->setCurrentIndex(0);
    }
    editorConsoleDock_->show();
    editorConsoleDock_->raise();
    editorConsoleTabs_->setCurrentIndex(1);
}

void MainWindow::AppendBuildLog(const QString& text) {
    if (!buildLogView_ || text.isEmpty()) {
        return;
    }
    QTextCursor cursor = buildLogView_->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    buildLogView_->setTextCursor(cursor);
    buildLogView_->verticalScrollBar()->setValue(
        buildLogView_->verticalScrollBar()->maximum());
}

void MainWindow::SetBuildActionsEnabled(bool enabled) {
    if (generateAction_) generateAction_->setEnabled(enabled);
    if (flashAction_) flashAction_->setEnabled(enabled);
    if (generateFlashAction_) generateFlashAction_->setEnabled(enabled);
    if (buildSimAction_) buildSimAction_->setEnabled(enabled);
}

void MainWindow::StartBuildCommand(BuildCommand command) {
    ShowBuildLogs();

    if (!buildProcess_ || buildProcess_->state() != QProcess::NotRunning) {
        AppendBuildLog(QStringLiteral("\n[warning] A firmware operation is already running.\n"));
        return;
    }

    // Always serialize the live graph before invoking external tools. For a
    // new graph this prompts for its first filename.
    if (!EnsureGraphSaved()) {
        AppendBuildLog(QStringLiteral("\n[cancelled] Graph was not saved.\n"));
        return;
    }

    const std::filesystem::path projectRoot = ProjectRoot();
    const QString absoluteGraphPath =
        QFileInfo(QString::fromStdString(currentPath_)).absoluteFilePath();

    QString operationName;
    switch (command) {
        case BuildCommand::Generate:
            operationName = QStringLiteral("Generate Code");
            break;
        case BuildCommand::Flash:
            operationName = QStringLiteral("Flash");
            break;
        case BuildCommand::GenerateAndFlash:
            operationName = QStringLiteral("Generate and Flash");
            break;
        case BuildCommand::BuildSimulation:
            operationName = QStringLiteral("Build Simulation");
            break;
    }

    AppendBuildLog(
        QStringLiteral("\n============================================================\n"
                       "%1\n"
                       "Graph: %2\n"
                       "Project Root: %3\n"
                       "============================================================\n")
            .arg(operationName,
                 absoluteGraphPath,
                 QString::fromStdString(projectRoot.string())));

    QString program;
    QStringList arguments;

#ifdef _WIN32
    // Windows execution path: use PowerShell scripts for clean native execution
    program = QStringLiteral("powershell.exe");
    if (command == BuildCommand::BuildSimulation) {
        const std::filesystem::path psScript = projectRoot / "Images" / "HostSim" / "scripts" / "run_svpwm_live.ps1";
        arguments << QStringLiteral("-NoProfile")
                  << QStringLiteral("-ExecutionPolicy") << QStringLiteral("Bypass")
                  << QStringLiteral("-File") << QString::fromStdString(psScript.string())
                  << QStringLiteral("-ForceEmit")
                  << QStringLiteral("-KeepGui");
    } else {
        const std::filesystem::path psScript = projectRoot / "Images" / "NucleoL476FW" / "scripts" / "emit_and_build.ps1";
        arguments << QStringLiteral("-NoProfile")
                  << QStringLiteral("-ExecutionPolicy") << QStringLiteral("Bypass")
                  << QStringLiteral("-File") << QString::fromStdString(psScript.string())
                  << QStringLiteral("-Graph") << absoluteGraphPath;
        if (command == BuildCommand::Flash || command == BuildCommand::GenerateAndFlash) {
            arguments << QStringLiteral("-Flash");
        }
    }
#else
    // Linux / POSIX execution path
    if (command == BuildCommand::BuildSimulation) {
        program = QStringLiteral("bash");
        const std::filesystem::path psScript = projectRoot / "Images" / "HostSim" / "scripts" / "run_svpwm_live.ps1";
        arguments << QString::fromStdString(psScript.string()) << QStringLiteral("-ForceEmit");
    } else {
        const std::filesystem::path script = projectRoot / "Tools" / "build_flash_graph.sh";
        if (!std::filesystem::is_regular_file(script)) {
            AppendBuildLog(QStringLiteral("\n[error] Build workflow script not found: %1\n")
                               .arg(QString::fromStdString(script.string())));
            return;
        }
        program = QString::fromStdString(script.string());
        const QString buildKey = BuildKeyForGraph(currentPath_);
        const std::filesystem::path outputRoot =
            projectRoot / "build" / "nodegui" / buildKey.toStdString();
        const std::filesystem::path firmwareBuildDir = outputRoot / "firmware";
        const std::filesystem::path firmwareSourceDir = outputRoot / "firmware-src";

        arguments << QStringLiteral("--graph")
                  << absoluteGraphPath
                  << QStringLiteral("--fw-build-dir")
                  << QString::fromStdString(firmwareBuildDir.string())
                  << QStringLiteral("--fw-src-dir")
                  << QString::fromStdString(firmwareSourceDir.string())
                  << QStringLiteral("--build-type")
                  << QStringLiteral("Release");

        const bool shouldBuild = command != BuildCommand::Flash;
        const bool shouldFlash = command != BuildCommand::Generate;
        if (!shouldBuild) arguments << QStringLiteral("--flash-only");
        else if (!shouldFlash) arguments << QStringLiteral("--no-flash");
        if (shouldFlash && httpApiServer_) {
            arguments << QStringLiteral("--flash-url")
                      << QStringLiteral("http://127.0.0.1:%1").arg(httpApiServer_->actualPort());
        }
    }
#endif

    SetBuildActionsEnabled(false);
    statusBar()->showMessage(QStringLiteral("%1 in progress...").arg(operationName));
    buildProcess_->setWorkingDirectory(QString::fromStdString(projectRoot.string()));
    buildProcess_->start(program, arguments);
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
        if (appSwitcher_) {
            appSwitcher_->setCurrentIndex(0);
        }
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
