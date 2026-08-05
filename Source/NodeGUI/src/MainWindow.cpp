#include "MainWindow.h"

#include "FrameRateMonitor.h"
#include "InspectorPanel.h"
#include "NodePalette.h"

#include "runtime/ConsolePanel.h"
#include "runtime/FlashPanel.h"
#include "runtime/RuntimeTab.h"
#include "runtime/SignalTablePanel.h"

#include <RTEAutomation/CachePaths.h>
#include <RTEAutomation/Platform.h>

#include <QCoreApplication>
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QCursor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFrame>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
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
#include <QSettings>
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

namespace NodeGUI {

namespace {

std::filesystem::path InstalledResourceRoot() {
    std::filesystem::path directory =
        std::filesystem::path(QCoreApplication::applicationDirPath().toStdString());
    for (int level = 0; level < 6 && !directory.empty(); ++level) {
        const auto candidate = directory / "share" / "rte";
        if (std::filesystem::is_directory(candidate)) return candidate;
        directory = directory.parent_path();
    }
    return {};
}

std::string DefaultTemplatesDir() {
    const std::filesystem::path installed = InstalledResourceRoot()
        / "Assets" / "NodeTemplates";
    if (std::filesystem::is_directory(installed)) return installed.lexically_normal().string();
#ifdef RTE_NODE_TEMPLATES_DIR
    return RTE_NODE_TEMPLATES_DIR;
#else
    return (std::filesystem::current_path() / "Assets" / "NodeTemplates").string();
#endif
}

std::filesystem::path ProjectRoot() {
#ifdef RTE_PROJECT_ROOT
    return RTE_PROJECT_ROOT;
#else
    return std::filesystem::current_path();
#endif
}

std::filesystem::path DefaultFirmwareBaseDir() {
    const std::filesystem::path installed = InstalledResourceRoot()
        / "Images" / "Gen6FW";
    if (std::filesystem::is_directory(installed)) return installed.lexically_normal();
    return ProjectRoot() / "Images" / "Gen6FW";
}

QString RteCliPath() {
    const std::filesystem::path adjacent =
        std::filesystem::path(QCoreApplication::applicationDirPath().toStdString())
        / RTEAutomation::ExecutableName("rte");
    if (std::filesystem::is_regular_file(adjacent)) {
        return QString::fromStdString(adjacent.string());
    }
#ifdef RTE_CLI_DEVELOPMENT_PATH
    if (std::filesystem::is_regular_file(RTE_CLI_DEVELOPMENT_PATH)) {
        return QString::fromUtf8(RTE_CLI_DEVELOPMENT_PATH);
    }
#endif
    return QStringLiteral("rte");
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
    preferences_ = LoadAppPreferences();

    setWindowTitle(QStringLiteral("RTE Studio"));
    resize(1280, 800);

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // Persistent banner shown when the opened graph changes on disk (e.g. a
    // git pull). Stays up until the user picks Load / Save Copy / Ignore.
    reloadBanner_ = new QFrame(central);
    reloadBanner_->setStyleSheet(QStringLiteral(
        "QFrame { background: #4a3d22; border-bottom: 1px solid #8a6d3b; }"
        "QLabel { color: #ffd97a; background: transparent; }"
        "QPushButton { padding: 2px 10px; }"));
    auto* bannerLayout = new QHBoxLayout(reloadBanner_);
    bannerLayout->setContentsMargins(8, 4, 8, 4);
    reloadBannerText_ = new QLabel(reloadBanner_);
    bannerLayout->addWidget(reloadBannerText_, 1);
    auto* loadButton = new QPushButton(QStringLiteral("Load"), reloadBanner_);
    loadButton->setObjectName(QStringLiteral("reloadBannerLoad"));
    connect(loadButton, &QPushButton::clicked, this, [this] {
        HideReloadBanner();
        // Capture the current graph so a mis-clicked Load is one Ctrl+Z away.
        const std::string preLoad = graphScene_ ? graphScene_->Snapshot() : std::string{};
        if (currentPath_.empty() || !OpenGraph(currentPath_)) {
            // Keep offering until the load succeeds or the user ignores.
            ShowReloadBanner();
            return;
        }
        // OpenGraph resets the undo history; seed it with the pre-load state.
        if (!preLoad.empty() && preLoad != currentHistorySnapshot_) {
            undoHistory_.push_back(std::move(preLoad));
            UpdateHistoryActions();
        }
    });
    bannerLayout->addWidget(loadButton);
    auto* saveCopyButton = new QPushButton(QStringLiteral("Save Copy..."), reloadBanner_);
    saveCopyButton->setObjectName(QStringLiteral("reloadBannerSaveCopy"));
    connect(saveCopyButton, &QPushButton::clicked, this, [this] {
        const QString fileName = QFileDialog::getSaveFileName(
            this, QStringLiteral("Save Current Graph as Copy"), QString(),
            QStringLiteral("JSON (*.json)"));
        if (!fileName.isEmpty()) {
            HideReloadBanner();
            DoSave(fileName.toStdString());
        }
    });
    bannerLayout->addWidget(saveCopyButton);
    auto* ignoreButton = new QPushButton(QStringLiteral("Ignore"), reloadBanner_);
    ignoreButton->setObjectName(QStringLiteral("reloadBannerIgnore"));
    connect(ignoreButton, &QPushButton::clicked, this, &MainWindow::HideReloadBanner);
    bannerLayout->addWidget(ignoreButton);
    reloadBanner_->hide();
    layout->addWidget(reloadBanner_);

    // Watches the currently opened graph for on-disk changes.
    graphWatcher_ = new QFileSystemWatcher(this);
    connect(graphWatcher_, &QFileSystemWatcher::fileChanged,
            this, &MainWindow::OnGraphFileChanged);
    reloadDebounce_ = new QTimer(this);
    reloadDebounce_->setSingleShot(true);
    reloadDebounce_->setInterval(250);
    connect(reloadDebounce_, &QTimer::timeout, this, &MainWindow::ShowReloadBanner);

    view_ = new GraphView(graphScene_->Scene());
    view_->SetPanMouseButton(preferences_.panMouseButton);
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

    view_->onDomainDoubleClicked = [this](const QPointF& scenePos) {
        return graphScene_->SelectDomainAt(scenePos);
    };
    view_->onDomainDragStarted = [this](const QPointF& scenePos) {
        return graphScene_->BeginSelectedDomainDrag(scenePos);
    };
    view_->onDomainDragged = [this](const QPointF& delta) {
        graphScene_->MoveSelectedDomain(delta);
    };
    view_->onDomainDragFinished = [this] {
        graphScene_->EndSelectedDomainDrag();
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

    auto addScreenShortcut = [this](const QString& id,
                                    const QString& label,
                                    const QKeySequence& key,
                                    int screenIndex) {
        auto* action = new QAction(label, this);
        action->setShortcutContext(Qt::WindowShortcut);
        connect(action, &QAction::triggered, this, [this, screenIndex] {
            if (screenIndex < appSwitcher_->count()) {
                appSwitcher_->setCurrentIndex(screenIndex);
            }
        });
        addAction(action);
        RegisterShortcut(action,
                         id,
                         QStringLiteral("Navigation"),
                         label,
                         key);
    };
    addScreenShortcut(QStringLiteral("navigation.nodeEditor"),
                      QStringLiteral("Switch to Node Editor"),
                      QKeySequence(QStringLiteral("Ctrl+1")),
                      0);
    addScreenShortcut(QStringLiteral("navigation.runtime"),
                      QStringLiteral("Switch to Runtime"),
                      QKeySequence(QStringLiteral("Ctrl+2")),
                      1);
    addScreenShortcut(QStringLiteral("navigation.firmwareUpdate"),
                      QStringLiteral("Switch to Firmware Update"),
                      QKeySequence(QStringLiteral("Ctrl+3")),
                      2);

    buildProcess_ = new QProcess(this);
    buildProcess_->setProcessChannelMode(QProcess::SeparateChannels);
    connect(buildProcess_, &QProcess::readyReadStandardOutput, this, [this] {
        HandleCliOutput();
    });
    connect(buildProcess_, &QProcess::readyReadStandardError, this, [this] {
        AppendBuildLog(QString::fromLocal8Bit(buildProcess_->readAllStandardError()));
    });
    connect(buildProcess_,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
                HandleCliOutput();
                AppendBuildLog(QString::fromLocal8Bit(buildProcess_->readAllStandardError()));
                const bool success =
                    exitStatus == QProcess::NormalExit && exitCode == 0;
                FinishCliOperation(success, exitCode);
            });
    connect(buildProcess_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            AppendBuildLog(QStringLiteral("\n[error] Could not start build workflow: %1\n")
                               .arg(buildProcess_->errorString()));
            if (cliStage_ == CliStage::Flash && runtimeController_) {
                runtimeController_->ResumeAfterFlash();
            }
            cliStage_ = CliStage::None;
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
        if (!graphScene_->LoadWarning().isEmpty()) {
            ShowToast(graphScene_->LoadWarning());
        }
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
    graphScene_->SetChangeCallback([this] { RecordHistorySnapshot(); });
    ResetHistory();
    UpdateStatus();

    QSettings settings(QStringLiteral("RTE"), QStringLiteral("RTEStudio"));
    const QByteArray savedGeometry = settings.value(QStringLiteral("window/geometry")).toByteArray();
    if (preferences_.rememberWindowGeometry && !savedGeometry.isEmpty()) {
        restoreGeometry(savedGeometry);
    } else if (QScreen* screen = QApplication::primaryScreen()) {
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
    localSessionServer_ = std::make_unique<runtime::LocalSessionServer>(
        runtimeController_->Store(), this);

    localSessionServer_->SetDevicePort(serialPort.toStdString());
    localSessionServer_->SetCommandHandler(
        [this](const std::string& cmd) { return runtimeController_->SendCommandRaw(cmd); });
    localSessionServer_->SetFlashLeaseHandler([this](bool acquire) {
        if (acquire) runtimeController_->SuspendForFlash();
        else runtimeController_->ResumeAfterFlash();
    });
    localSessionServer_->SetExternalDeviceWritesEnabled(
        preferences_.allowExternalDeviceWrites);
    std::string sessionError;
    if (!localSessionServer_->Start(&sessionError)) {
        ShowToast(QStringLiteral("Local automation session could not start: %1")
                      .arg(QString::fromStdString(sessionError)));
    }

    runtimeTab_ = new runtime::RuntimeTab(runtimeController_.get(), this);
    screens_->addWidget(runtimeTab_);
    appSwitcher_->addTab(QStringLiteral("Runtime"));
    runtimeTab_->LoadAutosave();

    if (saveFramKeysAction_) {
        saveFramKeysAction_->setEnabled(true);
    }
    if (loadFramKeysAction_) {
        loadFramKeysAction_->setEnabled(true);
    }

    firmwareUpdateTab_ = new runtime::FlashPanel(runtimeController_.get(),
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
    buildLogView_->setMaximumBlockCount(preferences_.buildLogLineLimit);
    logsLayout->addWidget(buildLogView_, 1);
    editorConsoleTabs_->addTab(logsPage, QStringLiteral("Logs"));

    editorConsoleDock_->setWidget(editorConsoleTabs_);
    addDockWidget(Qt::BottomDockWidgetArea, editorConsoleDock_);
    editorConsoleDock_->hide();

    runtimeController_->Start();

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
    setWindowTitle(QStringLiteral("RTE Studio - %1").arg(QString::fromStdString(path)));
    ResetHistory();
    UpdateStatus();
    HideReloadBanner();
    UpdateGraphWatcher();
    if (!graphScene_->LoadWarning().isEmpty()) {
        ShowToast(graphScene_->LoadWarning());
    }
    return true;
}

void MainWindow::OnNew() {
    if (preferences_.confirmNewGraph
        && !AskYesNo(this,
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
    setWindowTitle(QStringLiteral("RTE Studio"));
    ResetHistory();
    UpdateStatus();
    HideReloadBanner();
    UpdateGraphWatcher();
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
    const QStringList replacedEditorActions = {
        QStringLiteral("Clear Selection"),
        QStringLiteral("Delete Selection"),
        QStringLiteral("Cut Selection"),
        QStringLiteral("Copy Selection"),
    };
    QList<QAction*> toRemove;
    for (QAction* action : view_->actions()) {
        if (action == clearSelectionAction_
            || action == deleteAction_
            || action == cutAction_
            || action == copyAction_) {
            continue;
        }
        if (replacedEditorActions.contains(action->text())) {
            toRemove.push_back(action);
            continue;
        }
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
    RegisterShortcut(newAction,
                     QStringLiteral("file.new"),
                     QStringLiteral("File"),
                     QStringLiteral("New Graph"),
                     QKeySequence(QKeySequence::New));
    connect(newAction, &QAction::triggered, this, &MainWindow::OnNew);

    QAction* openAction = fileMenu->addAction(QStringLiteral("&Open..."));
    RegisterShortcut(openAction,
                     QStringLiteral("file.open"),
                     QStringLiteral("File"),
                     QStringLiteral("Open Graph"),
                     QKeySequence(QKeySequence::Open));
    connect(openAction, &QAction::triggered, this, &MainWindow::OnOpen);

    QAction* saveAction = fileMenu->addAction(QStringLiteral("&Save"));
    RegisterShortcut(saveAction,
                     QStringLiteral("file.save"),
                     QStringLiteral("File"),
                     QStringLiteral("Save Graph"),
                     QKeySequence(QKeySequence::Save));
    connect(saveAction, &QAction::triggered, this, &MainWindow::OnSave);

    QAction* saveAsAction = fileMenu->addAction(QStringLiteral("Save &As..."));
    RegisterShortcut(saveAsAction,
                     QStringLiteral("file.saveAs"),
                     QStringLiteral("File"),
                     QStringLiteral("Save Graph As"),
                     QKeySequence(QKeySequence::SaveAs));
    connect(saveAsAction, &QAction::triggered, this, &MainWindow::OnSaveAs);

    fileMenu->addSeparator();

    QAction* spwmAction = fileMenu->addAction(QStringLiteral("Open SPWM &Demo Graph..."));
    spwmAction->setToolTip(QStringLiteral(
        "Load Images/HostSim/graphs/spwm_demo_graph.json. "
#ifdef _WIN32
        "Run: powershell -File Images\\HostSim\\scripts\\run_spwm_live.ps1"
#else
        "Run: Images/HostSim/scripts/run_spwm_live.sh"
#endif
        ));
    connect(spwmAction, &QAction::triggered, this, &MainWindow::OnOpenSpwmDemo);

    fileMenu->addSeparator();

    QAction* exitAction = fileMenu->addAction(QStringLiteral("E&xit"));
    RegisterShortcut(exitAction,
                     QStringLiteral("file.exit"),
                     QStringLiteral("File"),
                     QStringLiteral("Exit"),
                     QKeySequence(QKeySequence::Quit));
    connect(exitAction, &QAction::triggered, this, &MainWindow::OnExit);

    QMenu* editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));

    undoAction_ = editMenu->addAction(QStringLiteral("&Undo"));
    RegisterShortcut(undoAction_,
                     QStringLiteral("edit.undo"),
                     QStringLiteral("Edit"),
                     QStringLiteral("Undo"),
                     QKeySequence(QKeySequence::Undo));
    connect(undoAction_, &QAction::triggered, this, &MainWindow::OnUndo);

    redoAction_ = editMenu->addAction(QStringLiteral("&Redo"));
    RegisterShortcut(redoAction_,
                     QStringLiteral("edit.redo"),
                     QStringLiteral("Edit"),
                     QStringLiteral("Redo"),
                     QKeySequence(QKeySequence::Redo));
    connect(redoAction_, &QAction::triggered, this, &MainWindow::OnRedo);

    editMenu->addSeparator();

    cutAction_ = editMenu->addAction(QStringLiteral("Cu&t"));
    cutAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    RegisterShortcut(cutAction_,
                     QStringLiteral("edit.cut"),
                     QStringLiteral("Edit"),
                     QStringLiteral("Cut Selection"),
                     QKeySequence(QKeySequence::Cut));
    connect(cutAction_, &QAction::triggered, this, [this] {
        if (view_) {
            view_->onCopySelectedObjects();
            view_->onDeleteSelectedObjects();
        }
    });
    view_->addAction(cutAction_);

    copyAction_ = editMenu->addAction(QStringLiteral("&Copy"));
    copyAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    RegisterShortcut(copyAction_,
                     QStringLiteral("edit.copy"),
                     QStringLiteral("Edit"),
                     QStringLiteral("Copy Selection"),
                     QKeySequence(QKeySequence::Copy));
    connect(copyAction_, &QAction::triggered, this, [this] {
        if (view_) {
            view_->onCopySelectedObjects();
        }
    });
    view_->addAction(copyAction_);

    deleteAction_ = editMenu->addAction(QStringLiteral("&Delete Selection"));
    deleteAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    RegisterShortcut(deleteAction_,
                     QStringLiteral("edit.deleteSelection"),
                     QStringLiteral("Edit"),
                     QStringLiteral("Delete Selection"),
                     QKeySequence(QKeySequence::Delete));
    connect(deleteAction_, &QAction::triggered, this, [this] {
        if (view_) {
            view_->onDeleteSelectedObjects();
        }
    });
    view_->addAction(deleteAction_);

    clearSelectionAction_ =
        editMenu->addAction(QStringLiteral("Clear Selection"));
    clearSelectionAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    RegisterShortcut(clearSelectionAction_,
                     QStringLiteral("edit.clearSelection"),
                     QStringLiteral("Edit"),
                     QStringLiteral("Clear Selection"),
                     QKeySequence(Qt::Key_Escape));
    connect(clearSelectionAction_, &QAction::triggered, this, [this] {
        if (graphScene_->Scene()) {
            graphScene_->Scene()->clearSelection();
        }
    });
    view_->addAction(clearSelectionAction_);

    editMenu->addSeparator();
    QAction* preferencesAction =
        editMenu->addAction(QStringLiteral("&Preferences..."));
    RegisterShortcut(preferencesAction,
                     QStringLiteral("edit.preferences"),
                     QStringLiteral("Edit"),
                     QStringLiteral("Preferences"),
                     QKeySequence(QStringLiteral("Ctrl+,")));
    connect(preferencesAction, &QAction::triggered,
            this, &MainWindow::OnPreferences);

    QMenu* buildMenu = menuBar()->addMenu(QStringLiteral("&Build"));

    generateAction_ = buildMenu->addAction(QStringLiteral("&Generate Code"));
    RegisterShortcut(generateAction_,
                     QStringLiteral("build.generate"),
                     QStringLiteral("Build"),
                     QStringLiteral("Generate Code"),
                     {});
    connect(generateAction_, &QAction::triggered, this, [this] {
        StartBuildCommand(BuildCommand::Generate);
    });

    flashAction_ = buildMenu->addAction(QStringLiteral("&Flash"));
    RegisterShortcut(flashAction_,
                     QStringLiteral("build.flash"),
                     QStringLiteral("Build"),
                     QStringLiteral("Flash"),
                     {});
    connect(flashAction_, &QAction::triggered, this, [this] {
        StartBuildCommand(BuildCommand::Flash);
    });

    buildMenu->addSeparator();

    generateFlashAction_ = buildMenu->addAction(QStringLiteral("Generate and &Flash"));
    RegisterShortcut(generateFlashAction_,
                     QStringLiteral("build.generateAndFlash"),
                     QStringLiteral("Build"),
                     QStringLiteral("Generate and Flash"),
                     QKeySequence(Qt::Key_F5));
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
    RegisterShortcut(arrangeAction_,
                     QStringLiteral("view.autoArrange"),
                     QStringLiteral("View"),
                     QStringLiteral("Auto Arrange"),
                     QKeySequence(QStringLiteral("Ctrl+Shift+A")));
    connect(arrangeAction_, &QAction::triggered, this, &MainWindow::OnAutoArrange);

    RebuildViewMenu();

    // Runtime menu is only meaningful once SetupRuntime has created runtimeTab_.
    runtimeMenu_ = menuBar()->addMenu(QStringLiteral("&Runtime"));
    saveFramKeysAction_ =
        runtimeMenu_->addAction(QStringLiteral("&Save FRAM Keys\u2026"));
    saveFramKeysAction_->setEnabled(false);
    connect(saveFramKeysAction_, &QAction::triggered, this, [this] {
        if (runtimeTab_) {
            runtimeTab_->OnSaveFramKeys();
        }
    });
    loadFramKeysAction_ =
        runtimeMenu_->addAction(QStringLiteral("&Load FRAM Keys\u2026"));
    loadFramKeysAction_->setEnabled(false);
    connect(loadFramKeysAction_, &QAction::triggered, this, [this] {
        if (runtimeTab_) {
            runtimeTab_->OnLoadFramKeys();
        }
    });
}

void MainWindow::RegisterShortcut(QAction* action,
                                  const QString& id,
                                  const QString& category,
                                  const QString& label,
                                  const QKeySequence& defaultSequence) {
    if (!action) {
        return;
    }
    action->setShortcut(LoadShortcutPreference(id, defaultSequence));
    shortcutBindings_.push_back({id, category, label, defaultSequence, action});
}

void MainWindow::ApplyPreferences(const AppPreferences& preferences) {
    preferences_ = preferences;
    if (view_) {
        view_->SetPanMouseButton(preferences_.panMouseButton);
    }
    if (buildLogView_) {
        buildLogView_->setMaximumBlockCount(preferences_.buildLogLineLimit);
    }
    if (localSessionServer_) {
        localSessionServer_->SetExternalDeviceWritesEnabled(
            preferences_.allowExternalDeviceWrites);
    }

    const std::size_t limit = static_cast<std::size_t>(preferences_.undoHistoryLimit);
    if (undoHistory_.size() > limit) {
        undoHistory_.erase(undoHistory_.begin(),
                           undoHistory_.begin()
                               + static_cast<std::ptrdiff_t>(undoHistory_.size() - limit));
    }
    UpdateHistoryActions();
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
    UpdateHistoryActions();
}

bool MainWindow::DoSave(const std::string& path) {
    graphScene_->SyncPositionsFromScene();

    // Our own write trips the file watcher; suppress the reload banner for
    // it and re-arm the watch on the (possibly new) path.
    suppressWatch_ = true;
    const QString error = graphScene_->SaveGraph(path);
    if (!error.isEmpty()) {
        suppressWatch_ = false;
        ShowToast(QStringLiteral("Failed to save graph: %1").arg(error));
        return false;
    }

    currentPath_ = path;
    setWindowTitle(QStringLiteral("RTE Studio - %1").arg(QString::fromStdString(path)));
    UpdateGraphWatcher();
    QTimer::singleShot(500, this, [this] { suppressWatch_ = false; });
    return true;
}

void MainWindow::UpdateGraphWatcher() {
    if (!graphWatcher_) {
        return;
    }
    const QStringList watched = graphWatcher_->files();
    if (!watched.isEmpty()) {
        graphWatcher_->removePaths(watched);
    }
    if (currentPath_.empty()) {
        return;
    }
    const QString path = QString::fromStdString(currentPath_);
    if (QFileInfo::exists(path)) {
        graphWatcher_->addPath(path);
    }
}

void MainWindow::OnGraphFileChanged(const QString& /*path*/) {
    // QFileSystemWatcher drops paths that get replaced (atomic saves, git
    // checkout), so re-arm on every notification.
    UpdateGraphWatcher();
    if (suppressWatch_ || reloadBanner_->isVisible()) {
        return;
    }
    reloadDebounce_->start();
}

void MainWindow::ShowReloadBanner() {
    if (currentPath_.empty() || suppressWatch_) {
        return;
    }
    const QString name = QFileInfo(QString::fromStdString(currentPath_)).fileName();
    reloadBannerText_->setText(
        QStringLiteral("'%1' changed on disk. Load the changes?").arg(name));
    reloadBanner_->show();
}

void MainWindow::HideReloadBanner() {
    reloadBanner_->hide();
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
    if (preferences_.automaticallyShowBuildLogs) {
        ShowBuildLogs();
    }

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
    const auto workspace = RTEAutomation::WorkspaceForGraph(
        absoluteGraphPath.toStdString(), preferences_.firmwareBuildType.toStdString());
    const std::filesystem::path firmwareBuildDir = workspace.build;
    const std::filesystem::path firmwareSourceDir = workspace.generated;
    const std::filesystem::path firmwareBinary =
        workspace.artifacts / "STM32CubeMX.bin";

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

    activeBuildCommand_ = command;
    pendingFirmwarePath_ = QString::fromStdString(firmwareBinary.string());
    SetBuildActionsEnabled(false);
    statusBar()->showMessage(QStringLiteral("%1 in progress...").arg(operationName));
    if (command == BuildCommand::Flash) {
        if (!std::filesystem::is_regular_file(firmwareBinary)) {
            AppendBuildLog(QStringLiteral("\n[error] Firmware binary does not exist: %1\n")
                               .arg(pendingFirmwarePath_));
            SetBuildActionsEnabled(true);
            return;
        }
        StartCliFlash(pendingFirmwarePath_);
        return;
    }

    if (command == BuildCommand::BuildSimulation) {
        // Re-emit the graph into HostSim, rebuild the simulator, and restart
        // the live telemetry feed (see Images/HostSim/scripts).
#ifdef _WIN32
        const std::filesystem::path script =
            projectRoot / "Images" / "HostSim" / "scripts" / "run_spwm_live.ps1";
        const QStringList arguments{
            QStringLiteral("-NoProfile"),
            QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
            QStringLiteral("-File"), QString::fromStdString(script.string()),
            QStringLiteral("-ForceEmit"),
            QStringLiteral("-NoGui"),
            QStringLiteral("-Graph"),
            absoluteGraphPath,
        };
        buildProcess_->start(QStringLiteral("powershell.exe"), arguments);
#else
        const std::filesystem::path script =
            projectRoot / "Images" / "HostSim" / "scripts" / "run_spwm_live.sh";
        const QStringList arguments{
            QString::fromStdString(script.string()),
            QStringLiteral("--force-emit"),
            QStringLiteral("--no-gui"),
            QStringLiteral("--graph"),
            absoluteGraphPath,
        };
        buildProcess_->start(QStringLiteral("bash"), arguments);
#endif
        buildProcess_->setWorkingDirectory(QString::fromStdString(projectRoot.string()));
        return;
    }

    QStringList arguments{
        QStringLiteral("--format"), QStringLiteral("jsonl"),
        command == BuildCommand::Generate ? QStringLiteral("generate")
                                           : QStringLiteral("build"),
        QStringLiteral("--graph"), absoluteGraphPath,
        QStringLiteral("--base-source"),
        QString::fromStdString(DefaultFirmwareBaseDir().string()),
        QStringLiteral("--templates"), QString::fromStdString(DefaultTemplatesDir()),
    };
    if (command == BuildCommand::Generate) {
        arguments << QStringLiteral("--output")
                  << QString::fromStdString(firmwareSourceDir.string());
        cliStage_ = CliStage::Generate;
    } else {
        arguments << QStringLiteral("--source-output")
                  << QString::fromStdString(firmwareSourceDir.string())
                  << QStringLiteral("--build-dir")
                  << QString::fromStdString(firmwareBuildDir.string())
                  << QStringLiteral("--build-type")
                  << preferences_.firmwareBuildType;
        cliStage_ = CliStage::Build;
    }
    cliOutputBuffer_.clear();
    buildProcess_->setWorkingDirectory(
        QFileInfo(absoluteGraphPath).absolutePath());
    buildProcess_->start(RteCliPath(), arguments);
}

void MainWindow::StartCliFlash(const QString& firmwarePath) {
    if (!runtimeController_) {
        AppendBuildLog(QStringLiteral("\n[error] Runtime controller is unavailable.\n"));
        SetBuildActionsEnabled(true);
        return;
    }
    runtimeController_->SuspendForFlash();
    cliStage_ = CliStage::Flash;
    cliOutputBuffer_.clear();
    const QStringList arguments{
        QStringLiteral("--format"), QStringLiteral("jsonl"),
        QStringLiteral("flash"), QStringLiteral("--firmware"), firmwarePath,
        QStringLiteral("--serial"), runtimeController_->Port(),
        QStringLiteral("--auto-gpio"),
    };
    statusBar()->showMessage(QStringLiteral("Flashing firmware..."));
    buildProcess_->start(RteCliPath(), arguments);
}

void MainWindow::HandleCliOutput() {
    if (!buildProcess_) return;
    cliOutputBuffer_ += buildProcess_->readAllStandardOutput();
    for (;;) {
        const qsizetype newline = cliOutputBuffer_.indexOf('\n');
        if (newline < 0) break;
        QByteArray line = cliOutputBuffer_.left(newline).trimmed();
        cliOutputBuffer_.remove(0, newline + 1);
        if (line.isEmpty()) continue;
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            AppendBuildLog(QString::fromUtf8(line) + u'\n');
            continue;
        }
        const QJsonObject object = document.object();
        const QString event = object.value(QStringLiteral("event")).toString();
        const QString message = object.value(QStringLiteral("message")).toString();
        if (!message.isEmpty()) {
            AppendBuildLog(QStringLiteral("[%1] %2\n").arg(
                object.value(QStringLiteral("phase")).toString(event), message));
        }
        if (event == QStringLiteral("artifact")
            && object.value(QStringLiteral("kind")).toString()
                == QStringLiteral("firmware-bin")) {
            pendingFirmwarePath_ = object.value(QStringLiteral("path")).toString();
        }
    }
}

void MainWindow::FinishCliOperation(bool success, int exitCode) {
    const CliStage finishedStage = cliStage_;
    if (finishedStage == CliStage::Flash && runtimeController_) {
        runtimeController_->ResumeAfterFlash();
    }
    if (success && finishedStage == CliStage::Build
        && activeBuildCommand_ == BuildCommand::GenerateAndFlash) {
        StartCliFlash(pendingFirmwarePath_);
        return;
    }
    cliStage_ = CliStage::None;
    AppendBuildLog(success
        ? QStringLiteral("\n[finished] Operation completed successfully.\n")
        : QStringLiteral("\n[finished] Operation failed (exit code %1).\n").arg(exitCode));
    statusBar()->showMessage(success ? QStringLiteral("Firmware operation completed")
                                     : QStringLiteral("Firmware operation failed"), 5000);
    SetBuildActionsEnabled(true);
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
    if (preferences_.rememberWindowGeometry) {
        QSettings settings(QStringLiteral("RTE"), QStringLiteral("RTEStudio"));
        settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
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
#ifdef _WIN32
            "Images\\HostSim\\scripts\\run_spwm_live.ps1"
#else
            "Images/HostSim/scripts/run_spwm_live.sh"
#endif
            ));
        OnOpen();
        return;
    }
    if (!OpenGraph(path)) {
        ShowToast(QStringLiteral("Failed to open SPWM demo graph"));
    } else {
        appSwitcher_->setCurrentIndex(0);
        ShowToast(QStringLiteral(
            "SPWM graph loaded. Start live sim: "
#ifdef _WIN32
            "powershell -File Images\\HostSim\\scripts\\run_spwm_live.ps1"
#else
            "Images/HostSim/scripts/run_spwm_live.sh"
#endif
            ));
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

void MainWindow::OnUndo() {
    if (undoHistory_.empty()) {
        return;
    }

    const std::string target = undoHistory_.back();
    if (!RestoreHistorySnapshot(target)) {
        return;
    }

    undoHistory_.pop_back();
    redoHistory_.push_back(std::move(currentHistorySnapshot_));
    currentHistorySnapshot_ = target;
    UpdateHistoryActions();
}

void MainWindow::OnRedo() {
    if (redoHistory_.empty()) {
        return;
    }

    const std::string target = redoHistory_.back();
    if (!RestoreHistorySnapshot(target)) {
        return;
    }

    redoHistory_.pop_back();
    undoHistory_.push_back(std::move(currentHistorySnapshot_));
    currentHistorySnapshot_ = target;
    UpdateHistoryActions();
}

void MainWindow::OnPreferences() {
    PreferencesDialog dialog(preferences_, shortcutBindings_, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const AppPreferences updatedPreferences = dialog.Preferences();
    const QMap<QString, QKeySequence> shortcuts = dialog.Shortcuts();
    for (ShortcutBinding& binding : shortcutBindings_) {
        const auto it = shortcuts.constFind(binding.id);
        if (it != shortcuts.cend() && binding.action) {
            binding.action->setShortcut(it.value());
        }
    }

    SaveAppPreferences(updatedPreferences);
    SaveShortcutPreferences(shortcuts);
    ApplyPreferences(updatedPreferences);
    statusBar()->showMessage(QStringLiteral("Preferences saved"), 3000);
}

void MainWindow::OnAutoArrange() {
    graphScene_->AutoArrange();
}

void MainWindow::OnExit() {
    close();
}

void MainWindow::ResetHistory() {
    undoHistory_.clear();
    redoHistory_.clear();
    currentHistorySnapshot_ = graphScene_ ? graphScene_->Snapshot() : std::string{};
    UpdateHistoryActions();
}

void MainWindow::RecordHistorySnapshot() {
    if (restoringHistory_ || !graphScene_) {
        return;
    }

    std::string snapshot = graphScene_->Snapshot();
    if (snapshot == currentHistorySnapshot_) {
        return;
    }

    if (!currentHistorySnapshot_.empty()) {
        undoHistory_.push_back(std::move(currentHistorySnapshot_));
    }
    currentHistorySnapshot_ = std::move(snapshot);
    redoHistory_.clear();

    const std::size_t limit = static_cast<std::size_t>(preferences_.undoHistoryLimit);
    if (undoHistory_.size() > limit) {
        undoHistory_.erase(undoHistory_.begin(),
                           undoHistory_.begin()
                               + static_cast<std::ptrdiff_t>(undoHistory_.size() - limit));
    }
    UpdateHistoryActions();
}

bool MainWindow::RestoreHistorySnapshot(const std::string& snapshot) {
    if (!graphScene_ || !view_) {
        return false;
    }

    restoringHistory_ = true;
    view_->setScene(nullptr);
    const QString error = graphScene_->RestoreSnapshot(snapshot);
    view_->setScene(graphScene_->Scene());
    StripBrokenSceneActions();
    palette_->SetNodeTypes(graphScene_->Graph().GetNodeTypes());
    ConnectModelSignals();
    restoringHistory_ = false;

    if (!error.isEmpty()) {
        ShowToast(error);
        return false;
    }

    UpdateStatus();
    return true;
}

void MainWindow::UpdateHistoryActions() {
    const bool nodeEditorActive = !appSwitcher_ || appSwitcher_->currentIndex() == 0;
    if (undoAction_) {
        undoAction_->setEnabled(nodeEditorActive && !undoHistory_.empty());
    }
    if (redoAction_) {
        redoAction_->setEnabled(nodeEditorActive && !redoHistory_.empty());
    }
    for (QAction* action : {cutAction_, copyAction_, deleteAction_, clearSelectionAction_}) {
        if (action) {
            action->setEnabled(nodeEditorActive);
        }
    }
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
