#include "MainWindow.h"

#include "FrameRateMonitor.h"
#include "GraphKind.h"
#include "InspectorPanel.h"
#include "NodePalette.h"
#include "PlantEditorScreen.h"  // [plant]
#include "ResourcePaths.h"
#include "sim/SimulateScreen.h"     // [sim]
#include "sim/SimTelemetryView.h"  // [gvals]

#include "runtime/ConsolePanel.h"
#include "runtime/FlashPanel.h"
#include "runtime/RuntimeTab.h"
#include "runtime/SignalTablePanel.h"
#include "runtime/TelemetryStore.h"  // [gvals] ValueAt/LatestValue

#include <RTEAutomation/CachePaths.h>
#include <RTEAutomation/Platform.h>

#include <QtNodes/internal/ConnectionGraphicsObject.hpp>
#include <QtNodes/internal/BasicGraphicsScene.hpp>
#include <QtNodes/internal/NodeGraphicsObject.hpp>

#include <QAction>
#include <QApplication>
#include <QAbstractButton>
#include <QCloseEvent>
#include <QCursor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPalette>
#include <QStyleFactory>
#include <QDockWidget>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFrame>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
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

// [plant] Sibling of DefaultTemplatesDir(): the plant-side node library used
// by plant nodegraphs (HW side of the loop; see Assets/PlantTemplates).
std::string DefaultPlantTemplatesDir() {
    const std::filesystem::path installed = InstalledResourceRoot()
        / "Assets" / "PlantTemplates";
    if (std::filesystem::is_directory(installed)) return installed.lexically_normal().string();
    return (ProjectRoot() / "Assets" / "PlantTemplates").lexically_normal().string();
}

std::filesystem::path DefaultFirmwareBaseDir() {
    const std::filesystem::path installed = InstalledResourceRoot()
        / "Images" / "Gen6FW";
    if (std::filesystem::is_directory(installed)) return installed.lexically_normal();
    return ProjectRoot() / "Images" / "Gen6FW";
}

/**
 * @brief Find the firmware base source directory that belongs to a graph file.
 *
 * Graphs live under <project>/Assets/Examples, while the matching firmware tree
 * is at <project>/Images/Gen6FW.  When RTE Studio is launched from a directory
 * other than the project root (or packaged without RTE_PROJECT_ROOT), the
 * generic DefaultFirmwareBaseDir() resolves relative to cwd and points at the
 * wrong place.  This helper walks up from the graph file until it finds the
 * Images/Gen6FW tree and returns an absolute path.
 */
std::filesystem::path FirmwareBaseDirForGraph(const std::filesystem::path& graphPath) {
    std::error_code ec;
    std::filesystem::path dir = graphPath.parent_path();
    for (int level = 0; level < 6 && !dir.empty(); ++level) {
        std::filesystem::path candidate = dir / "Images" / "Gen6FW";
        if (std::filesystem::is_regular_file(candidate / "CMakeLists.txt", ec)) {
            return candidate.lexically_normal();
        }
        dir = dir.parent_path();
    }
    return DefaultFirmwareBaseDir();
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
    // Apply the saved theme at startup: ApplyThemeMode used to run only from
    // the Preferences-accept path, so a saved dark/light choice silently
    // never came into effect on relaunch (access audit 2026-10-21, seeded
    // themeMode images were bit-identical).
    ApplyThemeMode(preferences_.themeMode);

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
        // The banner itself is the discard confirmation (it keeps the
        // pre-load state one Ctrl+Z away), so skip the unsaved prompt here.
        if (currentPath_.empty() || !OpenGraph(currentPath_, /*respectDirty=*/false)) {
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

    // Ctrl+double-click on a wire: quick-edit its net label through an
    // instance-modal dialog. The label pipeline itself lives in GraphScene.
    view_->onNetLabelRequested = [this](QtNodes::ConnectionId connId) {
        GraphScene* scene = ActiveGraphScene();
        if (!scene) {
            return;
        }
        QInputDialog dialog(this);
        dialog.setInputMode(QInputDialog::TextInput);
        dialog.setWindowTitle(QStringLiteral("Net label"));
        dialog.setLabelText(QStringLiteral(
            "Net label (empty clears; unique across all connections):"));
        dialog.setTextValue(scene->ConnectionLabel(connId));
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        const QString error = scene->SetConnectionLabel(connId, dialog.textValue());
        if (!error.isEmpty()) {
            ShowToast(error);
        } else if (inspector_ && scene == graphScene_.get()) {
            // The node editor's inspector only — the plant screen keeps its
            // own panel and never shows connection rows.
            inspector_->RefreshNetLabel(connId, scene->ConnectionLabel(connId));
        }
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
    view_->onNodeDragProven = [this](QtNodes::NodeId qtId) {
        graphScene_->ReportDragProven(qtId);
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

    // [plant] Plant Editor screen: first-class editing for plant nodegraphs,
    // tabbed between the Node Editor and Runtime. Owns its own document,
    // history, scene, palette and inspector (the Node Editor singletons
    // stay untouched); its panels live in per-screen docks like the other
    // tabs' panels.
    plantScreen_ = new PlantEditorScreen(DefaultPlantTemplatesDir(), this);
    screens_->addWidget(plantScreen_);
    appSwitcher_->addTab(QStringLiteral("Plant Editor"));

    plantToolboxDock_ = new QDockWidget(QStringLiteral("Plant Toolbox"), this);
    plantToolboxDock_->setObjectName(QStringLiteral("plantToolboxDock"));
    plantToolboxDock_->setWidget(
        new NodePaletteHost(plantScreen_->PalettePanel(), this));
    // Same usable-read floor as the node palette (2-char truncation at the
    // default dock breadth; preferred size still wins when resized).
    plantScreen_->PalettePanel()->setMinimumWidth(190);
    addDockWidget(Qt::LeftDockWidgetArea, plantToolboxDock_);
    plantToolboxDock_->hide();

    plantInspectorDock_ = new QDockWidget(QStringLiteral("Plant Inspector"), this);
    plantInspectorDock_->setObjectName(QStringLiteral("plantInspectorDock"));
    plantInspectorDock_->setWidget(plantScreen_->InspectorPanelWidget());
    addDockWidget(Qt::RightDockWidgetArea, plantInspectorDock_);
    plantInspectorDock_->hide();

    plantScreen_->SetUndoHistoryLimit(preferences_.undoHistoryLimit);
    plantScreen_->SetPanMouseButton(preferences_.panMouseButton);
    plantScreen_->onDocumentChanged = [this] {
        UpdateWindowTitle();
        UpdateHistoryActions();
    };
    plantScreen_->onStatusMessage = [this](const QString& message) {
        if (PlantTabActive()) {
            statusBar()->showMessage(message);
        }
    };

    // [sim] Simulate screen (QWidget, dock-less): runs `rte sim` against the
    // two graph documents and visualizes the produced v3 session. It lives
    // between the Plant Editor and Runtime tabs.
    simulateScreen_ = new sim::SimulateScreen(this);
#ifdef RTE_CLI_DEVELOPMENT_PATH
    simulateScreen_->SetRteBinary(QStringLiteral(RTE_CLI_DEVELOPMENT_PATH));
#endif
    // [defsim] Default run source = the OPEN documents: each graph row binds
    // to its editor's live document. The info read feeds the row caption
    // ("current document (Node Editor): foc_demo.json * (unsaved edits)" /
    // placeholder naming when no document is open); the export read
    // serializes the in-memory document FRESH at every Run (dirty content
    // included, no save dialog, never dirtying the file) into the session
    // run dir the run compiles from.
    simulateScreen_->SetCurrentDocumentInfoProvider(
        [this]() -> sim::SimulateScreen::CurrentDocumentInfo {
            sim::SimulateScreen::CurrentDocumentInfo info;
            info.hasDocument = ControllerDocumentPresent();
            info.dirty = ControllerDocumentDirty();
            info.documentPath = ControllerDocumentPath();
            info.displayName = ControllerDocumentDisplayName();
            return info;
        });
    simulateScreen_->SetCurrentPlantDocumentInfoProvider(
        [this]() -> sim::SimulateScreen::CurrentDocumentInfo {
            sim::SimulateScreen::CurrentDocumentInfo info;
            info.hasDocument = PlantDocumentPresent();
            info.dirty = PlantDocumentDirty();
            info.documentPath = PlantDocumentPath();
            info.displayName = PlantDocumentDisplayName();
            return info;
        });
    simulateScreen_->SetCurrentDocumentExportProvider(
        [this]() -> std::string { return ExportControllerDocumentJson(); });
    simulateScreen_->SetCurrentPlantDocumentExportProvider(
        [this]() -> std::string { return ExportPlantDocumentJson(); });
    // Assigning the plant document a (new) path re-binds the sim Plant row
    // to that document: the row compiles the live document by default
    // instead of clobbering its field with a path that would go stale
    // against the next unsaved edit (replaces the pre-defsim SetPlantGraphPath
    // push, keeping its per-assignment-only cadence — never per canvas edit,
    // clunk audit item 3).
    plantScreen_->onPathAssigned = [this](const std::string& path) {
        if (simulateScreen_ && !path.empty()) {
            simulateScreen_->BindPlantRowToCurrentDocument();
        }
    };
    // [unsaved] Every plant-open entry (incl. the screen's own "Open an
    // example" link) flows through OpenPlantGraph so the dirty-document
    // prompt runs before the discard.
    plantScreen_->onOpenRequest = [this](const std::string& path) {
        OpenPlantGraph(path);
    };
    screens_->addWidget(simulateScreen_);
    appSwitcher_->addTab(QStringLiteral("Simulate"));

    // [gvals] Sim value tags: one overlay layer per editor canvas. The tag
    // set refills at 4 Hz (graphValuesTimer_ -> UpdateGraphValueTags) from
    // the Sim screen's retained store; the layers paint nothing until the
    // mode toggle AND the per-surface guard both pass.
    graphValuesNode_ = std::make_unique<GraphValuesOverlay>(graphScene_.get());
    graphValuesPlant_ = std::make_unique<GraphValuesOverlay>(plantScreen_->Scene());

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

    // [smoke] Hidden dump seams for the unsaved-changes model: the composed
    // window title (dirty '*' markers included) and the most recent
    // Save/Discard/Cancel offer + choice. findChild reaches hidden widgets,
    // so the offscreen smoke driver's `dump` reads both.
    windowTitleDump_ = new QLabel(this);
    windowTitleDump_->setObjectName(QStringLiteral("windowTitleDump"));
    windowTitleDump_->hide();
    unsavedPromptDump_ = new QLabel(this);
    unsavedPromptDump_->setObjectName(QStringLiteral("unsavedPromptDump"));
    unsavedPromptDump_->setText(QStringLiteral("(no unsaved-changes offer yet)"));
    unsavedPromptDump_->hide();

    // The viewport's Resize event carries the final post-layout size (and
    // covers scrollbar appear/disappear).
    view_->viewport()->installEventFilter(this);

    // Toolbox dock listing the available node types; entries are dragged onto
    // the view to instantiate them. Populated at startup (below) and on
    // OpenGraph.
    palette_ = new NodePalette(this);
    // Double-click / Return inserts the type at the view center through the
    // same onNodeTypeDropped seam as drag-drops (wave-3b keyboard batch).
    connect(palette_, &NodePalette::nodeTypeActivated, this,
            [this](const QString& typeId) {
                if (view_ && view_->onNodeTypeDropped) {
                    view_->onNodeTypeDropped(
                        typeId,
                        view_->mapToScene(view_->viewport()->rect().center()));
                }
            });
    toolboxDock_ = new QDockWidget(QStringLiteral("Node Toolbox"), this);
    toolboxDock_->setObjectName(QStringLiteral("nodeToolboxDock"));
    toolboxDock_->setWidget(new NodePaletteHost(palette_, this));
    // default dock breadth left names truncated to 2 chars; give the tree a
    // usable-read floor (preferred size still wins when the user resizes)
    palette_->setMinimumWidth(190);
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
        action->setObjectName(id);
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
        // Listed permanently in the View menu (see RebuildViewMenu).
        screenActions_.push_back(action);
    };
    addScreenShortcut(QStringLiteral("navigation.nodeEditor"),
                      QStringLiteral("Switch to Node Editor"),
                      QKeySequence(QStringLiteral("Ctrl+1")),
                      0);
    // [plant] The Plant Editor sits between Node Editor and Runtime, so the
    // runtime/firmware shortcuts shift one slot (Ctrl+2 -> Ctrl+3/Ctrl+4).
    addScreenShortcut(QStringLiteral("navigation.plantEditor"),
                      QStringLiteral("Switch to Plant Editor"),
                      QKeySequence(QStringLiteral("Ctrl+2")),
                      1);
    addScreenShortcut(QStringLiteral("navigation.simulate"),
                      QStringLiteral("Switch to Simulate"),
                      QKeySequence(QStringLiteral("Ctrl+3")),
                      2);
    // [sim] runtime/firmware shortcuts shift one slot again (Ctrl+4/Ctrl+5)
    addScreenShortcut(QStringLiteral("navigation.runtime"),
                      QStringLiteral("Switch to Runtime"),
                      QKeySequence(QStringLiteral("Ctrl+4")),
                      3);
    // Resolve the firmware screen's position at trigger time: SetupRuntime
    // appends it screens-side AFTER the runtime - the old literal '3' pinned
    // Ctrl+5 onto Runtime and the shortcut was dead on arrival
    // (2026-10-21 clunk audit).
    auto* firmwareShortcut = new QAction(
        QStringLiteral("Switch to Firmware Update"), this);
    firmwareShortcut->setObjectName(QStringLiteral("navigation.firmwareUpdate"));
    firmwareShortcut->setShortcutContext(Qt::WindowShortcut);
    connect(firmwareShortcut, &QAction::triggered, this, [this] {
        if (firmwareUpdateTab_) {
            const int idx = screens_->indexOf(firmwareUpdateTab_);
            if (idx >= 0) {
                appSwitcher_->setCurrentIndex(idx);
            }
        }
    });
    addAction(firmwareShortcut);
    RegisterShortcut(firmwareShortcut,
                     QStringLiteral("navigation.firmwareUpdate"),
                     QStringLiteral("Navigation"),
                     QStringLiteral("Switch to Firmware Update"),
                     QKeySequence(QStringLiteral("Ctrl+5")));
    screenActions_.push_back(firmwareShortcut);

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
    // [unsaved] The just-built empty document starts clean; anchor the saved
    // snapshot for the controller document (the plant screen anchors its own).
    savedSnapshotNode_ = currentHistorySnapshot_;
    UpdateWindowTitle();
    UpdateStatus();

    QSettings settings(QStringLiteral("RTE"), QStringLiteral("RTEStudio"));
    const QByteArray savedGeometry = settings.value(QStringLiteral("window/geometry")).toByteArray();
    if (preferences_.rememberWindowGeometry && !savedGeometry.isEmpty()) {
        restoreGeometry(savedGeometry);
    } else if (QScreen* screen = QApplication::primaryScreen()) {
        move(screen->availableGeometry().center() - frameGeometry().center());
    }

    // Per-screen dock layouts persist across restarts too (the in-memory
    // screenStates_ array used to die with the process - dossier P2-9):
    // window/screenState.<i> mirrors what OnTabChanged saves per switch; the
    // initial screen's state applies eagerly below, each later switch applies
    // through the existing OnTabChanged restore hook.
    for (int i = 0; i < 5; ++i) {
        const QByteArray state = settings.value(
            QStringLiteral("window/screenState.%1").arg(i)).toByteArray();
        if (!state.isEmpty()) {
            screenStates_[i] = state;
        }
    }
    if (!screenStates_[0].isEmpty()) {
        restoreState(screenStates_[0]);
    }

    // [gvals] 4 Hz value-tag refresh (the mode spec caps refresh at 4-8 Hz;
    // tag paints are dirty-gated, so the idle tick is free).
    graphValuesTimer_ = new QTimer(this);
    graphValuesTimer_->setInterval(250);
    connect(graphValuesTimer_, &QTimer::timeout, this,
            &MainWindow::UpdateGraphValueTags);
    graphValuesTimer_->start();
}

void MainWindow::SaveScreenStates() {
    QSettings settings(QStringLiteral("RTE"), QStringLiteral("RTEStudio"));
    // The active screen's layout lives in screenStates_[previousTab_] - write
    // that fresh first so a never-switched-before exit isn't stale.
    if (previousTab_ >= 0 && previousTab_ < 5) {
        screenStates_[previousTab_] = saveState();
    }
    for (int i = 0; i < 5; ++i) {
        if (!screenStates_[i].isEmpty()) {
            settings.setValue(QStringLiteral("window/screenState.%1").arg(i),
                              screenStates_[i]);
        }
    }
}

void MainWindow::SetupRuntime(const QString& serialPort,
                              bool simulate,
                              runtime::Protocol protocol) {
    runtimeController_ =
        std::make_unique<runtime::RuntimeController>(serialPort, simulate, protocol);
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

    telemetryConsoleDock_ = new QDockWidget(QStringLiteral("Runtime Console"), this);
    telemetryConsoleDock_->setObjectName(QStringLiteral("telemetryConsoleDock"));
    telemetryConsoleDock_->setWidget(runtimeTab_->GetConsole());
    addDockWidget(Qt::LeftDockWidgetArea, telemetryConsoleDock_);
    telemetryConsoleDock_->hide();

    // Node-screen debug dock: device console and build logs share tabs inside
    // one detachable dock. It stays off by default and is enabled via View or
    // automatically when a firmware operation starts.
    editorConsoleDock_ = new QDockWidget(QStringLiteral("Editor Console"), this);
    editorConsoleDock_->setObjectName(QStringLiteral("editorConsoleDock"));
    editorConsoleTabs_ = new QTabWidget(editorConsoleDock_);
    editorConsoleTabs_->addTab(
        new runtime::ConsolePanel(runtimeController_.get(), editorConsoleTabs_),
        QStringLiteral("Console"));

    auto* logsPage = new QWidget(editorConsoleTabs_);
    auto* logsLayout = new QVBoxLayout(logsPage);
    logsLayout->setContentsMargins(0, 0, 0, 0);
    auto* logsHeader = new QHBoxLayout;
    auto* clearLogsButton = new QPushButton(QStringLiteral("Clear Logs"), logsPage);
    connect(clearLogsButton, &QPushButton::clicked, this, [this] {
        if (buildLogView_) {
            buildLogView_->clear();
        }
    });
    logsHeader->addWidget(clearLogsButton);
    buildCancelButton_ = new QPushButton(QStringLiteral("Cancel"), logsPage);
    buildCancelButton_->setObjectName(QStringLiteral("buildCancel"));
    buildCancelButton_->setEnabled(false);
    buildCancelButton_->setToolTip(QStringLiteral(
        "Abort the running firmware operation."));
    connect(buildCancelButton_, &QPushButton::clicked, this, &MainWindow::OnBuildCancel);
    logsHeader->addWidget(buildCancelButton_);
    logsHeader->addStretch(1);
    logsLayout->addLayout(logsHeader);
    buildLogView_ = new QPlainTextEdit(logsPage);
    // Tab leaves the field; the all-text-edit Tab trap ends keyboard nav.
    buildLogView_->setTabChangesFocus(true);
    buildLogView_->setObjectName(QStringLiteral("buildLogView"));
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
    // Every open entry (File>Open, Recent Files, the CLI argument, the
    // plant screen's example link) lands here with the unsaved-changes gate
    // on; the reload banner passes respectDirty=false below because its
    // Load/Save Copy/Ignore choice already IS the confirmation (and it keeps
    // the pre-load state one Ctrl+Z away).
    return OpenGraph(path, /*respectDirty=*/true);
}

bool MainWindow::OpenGraph(const std::string& path, bool respectDirty) {
    // [plant] Route by document content: a graph holding any Plant.* node
    // opens in the Plant Editor (its own document + history); every other
    // graph keeps the Node Editor path below, unchanged. Unknown-kind files
    // (unreadable / not a JSON object) also stay here — LoadGraph below
    // surfaces the real parse error.
    if (plantScreen_ && GraphKindOf(path) == GraphKind::Plant) {
        return OpenPlantGraph(path, respectDirty);
    }

    // [unsaved] The load below replaces the controller document; offer
    // Save/Discard/Cancel first when it has unsaved changes.
    if (respectDirty && !ConfirmControllerDiscard(QStringLiteral("Open Graph"))) {
        return false;
    }

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
    ResetHistory();
    // [unsaved] A freshly loaded controller document is clean; only now can
    // the title read the settled dirty state (it compares history head vs
    // saved anchor, so refresh after both are set).
    savedSnapshotNode_ = currentHistorySnapshot_;
    UpdateWindowTitle();  // [plant] (was an inline setWindowTitle call)
    UpdateStatus();
    HideReloadBanner();
    UpdateGraphWatcher();
    if (!graphScene_->LoadWarning().isEmpty()) {
        ShowToast(graphScene_->LoadWarning());
    }
    FrameCanvasToContent();
    NoteRecentFile(QString::fromStdString(path));
    return true;
}

void MainWindow::NoteRecentFile(const QString& path) {
    if (path.isEmpty()) {
        return;
    }
    const QString absolute = QFileInfo(path).absoluteFilePath();
    QSettings settings(QStringLiteral("RTE"), QStringLiteral("RTEStudio"));
    QStringList recent = settings.value(QStringLiteral("file/recentFiles")).toStringList();
    recent.removeAll(absolute);
    recent.prepend(absolute);
    constexpr int kMaxRecent = 10;
    while (recent.size() > kMaxRecent) {
        recent.removeLast();
    }
    settings.setValue(QStringLiteral("file/recentFiles"), recent);
}

void MainWindow::RebuildRecentFilesMenu() {
    recentFilesMenu_->clear();
    QSettings settings(QStringLiteral("RTE"), QStringLiteral("RTEStudio"));
    const QStringList recent =
        settings.value(QStringLiteral("file/recentFiles")).toStringList();
    if (recent.isEmpty()) {
        recentFilesMenu_->addAction(QStringLiteral("(no recent files)"))->setEnabled(false);
        return;
    }
    for (const QString& path : recent) {
        const QFileInfo info(path);
        // "name (parentdir)" keeps the list skimmable; tooltip = full path.
        const QString label = QStringLiteral("%1\t%2")
                                  .arg(info.fileName(), info.dir().absolutePath());
        QAction* entry = recentFilesMenu_->addAction(label);
        entry->setToolTip(path);
        const bool exists = info.exists();
        entry->setEnabled(exists);
        connect(entry, &QAction::triggered, this, [this, path] {
            OpenGraph(path.toStdString());
        });
    }
}

void MainWindow::FrameCanvasToContent() {
    // [plant] Frame to Content follows the active tab's canvas.
    if (PlantTabActive()) {
        plantScreen_->FrameToContent();
        return;
    }
    if (!view_ || !graphScene_) return;
    const QRectF bounds = graphScene_->Scene()->itemsBoundingRect();
    if (bounds.isEmpty()) return;
    view_->fitInView(bounds.adjusted(-60, -60, 60, 60), Qt::KeepAspectRatio);
}

// [plant] Open a plant nodegraph in the Plant Editor screen and switch to it.
bool MainWindow::OpenPlantGraph(const std::string& path) {
    return OpenPlantGraph(path, /*respectDirty=*/true);
}

bool MainWindow::OpenPlantGraph(const std::string& path, bool respectDirty) {
    if (!plantScreen_) {
        return false;
    }
    // [unsaved] The load below replaces the PLANT document; prompt first
    // when it has unsaved changes (before the tab switch, so a cancelled
    // open changes nothing at all).
    if (respectDirty && !ConfirmPlantDiscard(QStringLiteral("Open Graph"))) {
        return false;
    }
    // Switch first so the load results (graph or error toast) show where the
    // user is looking.
    appSwitcher_->setCurrentIndex(screens_->indexOf(plantScreen_.data()));
    const QString error = plantScreen_->OpenGraph(path);
    if (!error.isEmpty()) {
        plantScreen_->ReportError(error);
        return false;
    }
    UpdateWindowTitle();
    UpdateHistoryActions();
    NoteRecentFile(QString::fromStdString(path));
    return true;
}

// [plant] Save the active plant document through the plant screen.
bool MainWindow::DoSavePlant(const std::string& path) {
    if (!plantScreen_) {
        return false;
    }
    const QString error = plantScreen_->SaveGraph(path);
    if (!error.isEmpty()) {
        plantScreen_->ReportError(error);
        return false;
    }
    return true;
}

// [plant] True while the Plant Editor is the active screen.
bool MainWindow::PlantTabActive() const {
    return appSwitcher_ && screens_ && plantScreen_
        && appSwitcher_->currentIndex() == screens_->indexOf(plantScreen_.data());
}

// [plant] Title tracks both documents once the Plant Editor has a named one
// ('RTE Studio - sw:<a> | plant:<b>'); with no plant document the title is
// exactly the Node Editor's original format. [unsaved] Each dirty document
// carries a '*' right after its component; the hidden windowTitleDump_ label
// mirrors the result for headless assertion.
void MainWindow::UpdateWindowTitle() {
    const QString controllerDirty = ControllerDocumentDirty() ? QStringLiteral("*")
                                                              : QString();
    const QString plantDirty = (plantScreen_ && plantScreen_->DocumentDirty())
                                   ? QStringLiteral("*")
                                   : QString();
    const bool hasPlant = plantScreen_ && !plantScreen_->CurrentPath().empty();
    if (!hasPlant) {
        // Untitled controller documents keep the plain 'RTE Studio' contract;
        // the marker then sits right after the app title itself.
        const QString path = QString::fromStdString(currentPath_);
        setWindowTitle(path.isEmpty()
                           ? QStringLiteral("RTE Studio%1").arg(controllerDirty)
                           : QStringLiteral("RTE Studio - %1%2").arg(path, controllerDirty));
    } else {
        QStringList documents;
        if (!currentPath_.empty()) {
            documents << QStringLiteral("sw:%1%2")
                             .arg(QFileInfo(QString::fromStdString(currentPath_)).fileName(),
                                  controllerDirty);
        } else if (ControllerDocumentDirty()) {
            // Unsaved untitled controller doc beside a named plant doc: give
            // its marker a component to hang on (normally omitted by design).
            documents << QStringLiteral("sw:(untitled)%1").arg(controllerDirty);
        }
        documents << QStringLiteral("plant:%1%2")
                         .arg(QFileInfo(QString::fromStdString(plantScreen_->CurrentPath())).fileName(),
                              plantDirty);
        setWindowTitle(QStringLiteral("RTE Studio - %1").arg(documents.join(QStringLiteral(" | "))));
    }
    if (windowTitleDump_) {
        windowTitleDump_->setText(windowTitle());
    }
    // [defsim] Every document-affecting change funnels here (open/new/save,
    // history record, dirty flip); the sim rows' "current document" captions
    // track the same truth.
    if (simulateScreen_) {
        simulateScreen_->RefreshSourceCaptions();
    }
}

bool MainWindow::ControllerDocumentDirty() const {
    return currentHistorySnapshot_ != savedSnapshotNode_;
}

// ---- [defsim] Simulate-screen document getters ----------------------------
bool MainWindow::ControllerDocumentPresent() const {
    return graphScene_
        && (!currentPath_.empty() || !graphScene_->Graph().GetNodes().empty());
}

QString MainWindow::ControllerDocumentPath() const {
    return QString::fromStdString(currentPath_);
}

QString MainWindow::ControllerDocumentDisplayName() const {
    if (!ControllerDocumentPresent()) {
        return QString();
    }
    return currentPath_.empty()
               ? QStringLiteral("(untitled)")
               : QFileInfo(QString::fromStdString(currentPath_)).fileName();
}

std::string MainWindow::ExportControllerDocumentJson() {
    if (!ControllerDocumentPresent()) {
        return {};
    }
    return graphScene_->Snapshot();   // in-memory serialization, read-only
}

bool MainWindow::PlantDocumentPresent() const {
    return plantScreen_ && plantScreen_->Scene()
        && (!plantScreen_->CurrentPath().empty()
            || !plantScreen_->Scene()->Graph().GetNodes().empty());
}

QString MainWindow::PlantDocumentPath() const {
    return plantScreen_ ? QString::fromStdString(plantScreen_->CurrentPath())
                        : QString();
}

QString MainWindow::PlantDocumentDisplayName() const {
    if (!PlantDocumentPresent()) {
        return QString();
    }
    return plantScreen_->CurrentPath().empty()
               ? QStringLiteral("(untitled)")
               : QFileInfo(PlantDocumentPath()).fileName();
}

bool MainWindow::PlantDocumentDirty() const {
    return plantScreen_ && plantScreen_->DocumentDirty();
}

std::string MainWindow::ExportPlantDocumentJson() {
    if (!PlantDocumentPresent()) {
        return {};
    }
    return plantScreen_->Scene()->Snapshot();   // in-memory, read-only
}

void MainWindow::MarkControllerSaved() {
    // Re-anchor BOTH ends on one Snapshot(): a mid-drag SyncPositionsFromScene
    // inside DoSave can push the persisted content one tick past the recorded
    // history head, and the two must read identical content identical bytes.
    savedSnapshotNode_ = graphScene_ ? graphScene_->Snapshot() : std::string{};
    currentHistorySnapshot_ = savedSnapshotNode_;
}

MainWindow::UnsavedChoice MainWindow::AskSaveDiscardCancel(const QString& context,
                                                           const QString& docLabel,
                                                           const QString& graphName) {
    const QString offer = QStringLiteral("context=%1 doc=%2 graph=%3")
                              .arg(context, docLabel, graphName);

    // [smoke] Documented headless override: RTE_SMOKE_UNSAVED_CHOICE=
    // save|discard|cancel pre-answers the prompt (offscreen modals cannot be
    // clicked). Real users always take the QMessageBox::exec() path below.
    const QByteArray forced = qgetenv("RTE_SMOKE_UNSAVED_CHOICE").toLower();
    UnsavedChoice choice;
    bool haveChoice = true;
    if (forced == "save") {
        choice = UnsavedChoice::Save;
    } else if (forced == "discard") {
        choice = UnsavedChoice::Discard;
    } else if (forced == "cancel") {
        choice = UnsavedChoice::Cancel;
    } else {
        haveChoice = false;
    }

    if (!haveChoice) {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(QStringLiteral("Unsaved Changes - %1").arg(context));
        box.setText(QStringLiteral("'%1' (%2) has unsaved changes.\n"
                                   "Save them first?")
                        .arg(graphName, docLabel));
        auto* saveButton = box.addButton(QStringLiteral("Save"),
                                         QMessageBox::AcceptRole);
        box.addButton(QStringLiteral("Discard"), QMessageBox::DestructiveRole);
        auto* cancelButton = box.addButton(QStringLiteral("Cancel"),
                                           QMessageBox::RejectRole);
        box.setDefaultButton(saveButton);
        box.setEscapeButton(cancelButton);
        box.exec();  // modal; headless runs use the env override above.
        QAbstractButton* clicked = box.clickedButton();
        if (clicked == saveButton) {
            choice = UnsavedChoice::Save;
        } else if (clicked == cancelButton || clicked == nullptr) {
            // Escape / window-close reads as Cancel: never discard silently.
            choice = UnsavedChoice::Cancel;
        } else {
            choice = UnsavedChoice::Discard;
        }
    }

    if (unsavedPromptDump_) {
        const char* choiceText = choice == UnsavedChoice::Save ? "save"
                                 : choice == UnsavedChoice::Discard ? "discard"
                                                                    : "cancel";
        unsavedPromptDump_->setText(
            QStringLiteral("%1 choice=%2").arg(offer, QString::fromLatin1(choiceText)));
    }
    return choice;
}

bool MainWindow::ConfirmControllerDiscard(const QString& context) {
    if (!ControllerDocumentDirty()) {
        return true;
    }
    const QString name = currentPath_.empty()
                             ? QStringLiteral("(untitled)")
                             : QString::fromStdString(currentPath_);
    switch (AskSaveDiscardCancel(context, QStringLiteral("Node Editor"), name)) {
        case UnsavedChoice::Discard:
            return true;
        case UnsavedChoice::Cancel:
            return false;
        case UnsavedChoice::Save:
            if (!currentPath_.empty()) {
                return DoSave(currentPath_);
            }
            // Untitled document needs a filename first; backing out of that
            // dialog aborts the pending action like Cancel does.
            const QString fileName = QFileDialog::getSaveFileName(
                this, QStringLiteral("Save Graph"), QString(),
                QStringLiteral("JSON (*.json)"));
            return !fileName.isEmpty() && DoSave(fileName.toStdString());
    }
    return false;
}

bool MainWindow::ConfirmPlantDiscard(const QString& context) {
    if (!plantScreen_ || !plantScreen_->DocumentDirty()) {
        return true;
    }
    const QString name = plantScreen_->CurrentPath().empty()
                             ? QStringLiteral("(untitled)")
                             : QString::fromStdString(plantScreen_->CurrentPath());
    switch (AskSaveDiscardCancel(context, QStringLiteral("Plant Editor"), name)) {
        case UnsavedChoice::Discard:
            return true;
        case UnsavedChoice::Cancel:
            return false;
        case UnsavedChoice::Save:
            if (!plantScreen_->CurrentPath().empty()) {
                return DoSavePlant(plantScreen_->CurrentPath());
            }
            const QString fileName = QFileDialog::getSaveFileName(
                this, QStringLiteral("Save Plant Graph"), QString(),
                QStringLiteral("JSON (*.json)"));
            return !fileName.isEmpty() && DoSavePlant(fileName.toStdString());
    }
    return false;
}

void MainWindow::OnNew() {
    // [plant] File>New applies to the ACTIVE tab's document.
    if (PlantTabActive()) {
        // [unsaved] A dirty plant document earns the Save/Discard/Cancel
        // prompt (it subsumes the confirm-new preference); the clean path
        // keeps the old preference-gated question.
        if (plantScreen_->DocumentDirty()) {
            if (!ConfirmPlantDiscard(QStringLiteral("File>New"))) {
                return;
            }
        } else if (preferences_.confirmNewGraph
                   && !AskYesNo(this,
                                QStringLiteral("New Plant Graph"),
                                QStringLiteral("Discard the current plant graph and start a new empty one?"))) {
            return;
        }
        const QString error = plantScreen_->NewGraph();
        if (!error.isEmpty()) {
            plantScreen_->ReportError(error);
        }
        return;
    }

    if (ControllerDocumentDirty()) {
        if (!ConfirmControllerDiscard(QStringLiteral("File>New"))) {
            return;
        }
    } else if (preferences_.confirmNewGraph
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
    ResetHistory();
    // [unsaved] The new empty controller document starts clean; the title
    // refresh follows the anchor so it reads the settled dirty state.
    savedSnapshotNode_ = currentHistorySnapshot_;
    UpdateWindowTitle();  // [plant] (was an inline setWindowTitle call)
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
    newAction->setObjectName(QStringLiteral("file.new"));  // smoke click seam
    RegisterShortcut(newAction,
                     QStringLiteral("file.new"),
                     QStringLiteral("File"),
                     QStringLiteral("New Graph"),
                     QKeySequence(QKeySequence::New));
    connect(newAction, &QAction::triggered, this, &MainWindow::OnNew);

    QAction* openAction = fileMenu->addAction(QStringLiteral("&Open..."));
    openAction->setObjectName(QStringLiteral("file.open"));  // smoke click seam
    RegisterShortcut(openAction,
                     QStringLiteral("file.open"),
                     QStringLiteral("File"),
                     QStringLiteral("Open Graph"),
                     QKeySequence(QKeySequence::Open));
    connect(openAction, &QAction::triggered, this, &MainWindow::OnOpen);

    recentFilesMenu_ = fileMenu->addMenu(QStringLiteral("Recent Files"));
    recentFilesMenu_->setObjectName(QStringLiteral("fileRecentMenu"));
    connect(recentFilesMenu_, &QMenu::aboutToShow, this,
            &MainWindow::RebuildRecentFilesMenu);

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

    QAction* exitAction = fileMenu->addAction(QStringLiteral("E&xit"));
    exitAction->setObjectName(QStringLiteral("file.exit"));  // smoke click seam
    RegisterShortcut(exitAction,
                     QStringLiteral("file.exit"),
                     QStringLiteral("File"),
                     QStringLiteral("Exit"),
                     QKeySequence(QKeySequence::Quit));
    connect(exitAction, &QAction::triggered, this, &MainWindow::OnExit);

    QMenu* editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));

    undoAction_ = editMenu->addAction(QStringLiteral("&Undo"));
    undoAction_->setObjectName(QStringLiteral("edit.undo"));  // smoke click seam
    RegisterShortcut(undoAction_,
                     QStringLiteral("edit.undo"),
                     QStringLiteral("Edit"),
                     QStringLiteral("Undo"),
                     QKeySequence(QKeySequence::Undo));
    connect(undoAction_, &QAction::triggered, this, &MainWindow::OnUndo);

    redoAction_ = editMenu->addAction(QStringLiteral("&Redo"));
    redoAction_->setObjectName(QStringLiteral("edit.redo"));  // smoke click seam
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
    QAction* addBendAction = editMenu->addAction(QStringLiteral("Add Wire &Bend"));
    addBendAction->setObjectName(QStringLiteral("route.addPoint"));
    addBendAction->setStatusTip(QStringLiteral(
        "Insert a manual routing bend on the selected wire"));
    connect(addBendAction, &QAction::triggered, this, [this] {
        OnWireBendAction(true);
    });

    QAction* removeBendAction =
        editMenu->addAction(QStringLiteral("Remove Last Wire &Bend"));
    removeBendAction->setObjectName(QStringLiteral("route.removePoint"));
    removeBendAction->setStatusTip(QStringLiteral(
        "Drop the selected wire's last manual routing bend"));
    connect(removeBendAction, &QAction::triggered, this, [this] {
        OnWireBendAction(false);
    });

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
    generateAction_->setObjectName(QStringLiteral("buildGenerate"));
    RegisterShortcut(generateAction_,
                     QStringLiteral("build.generate"),
                     QStringLiteral("Build"),
                     QStringLiteral("Generate Code"),
                     {});
    connect(generateAction_, &QAction::triggered, this, [this] {
        StartBuildCommand(BuildCommand::Generate);
    });

    flashAction_ = buildMenu->addAction(QStringLiteral("&Flash"));
    flashAction_->setObjectName(QStringLiteral("buildFlash"));
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
    generateFlashAction_->setObjectName(QStringLiteral("buildGenerateFlash"));
    RegisterShortcut(generateFlashAction_,
                     QStringLiteral("build.generateAndFlash"),
                     QStringLiteral("Build"),
                     QStringLiteral("Generate and Flash"),
                     QKeySequence(Qt::Key_F5));
    connect(generateFlashAction_, &QAction::triggered, this, [this] {
        StartBuildCommand(BuildCommand::GenerateAndFlash);
    });

    QMenu* viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    viewMenu->setObjectName(QStringLiteral("viewMenu"));
    viewMenu_ = viewMenu;

    frameContentAction_ = new QAction(QStringLiteral("Frame to &Content"), this);
    RegisterShortcut(frameContentAction_,
                     QStringLiteral("view.frameContent"),
                     QStringLiteral("View"),
                     QStringLiteral("Frame to Content"),
                     QKeySequence(QStringLiteral("Ctrl+0")));
    connect(frameContentAction_, &QAction::triggered,
            this, &MainWindow::FrameCanvasToContent);

    arrangeAction_ = new QAction(QStringLiteral("&Auto Arrange"), this);
    RegisterShortcut(arrangeAction_,
                     QStringLiteral("view.autoArrange"),
                     QStringLiteral("View"),
                     QStringLiteral("Auto Arrange"),
                     QKeySequence(QStringLiteral("Ctrl+Shift+A")));
    connect(arrangeAction_, &QAction::triggered, this, &MainWindow::OnAutoArrange);

    // [gvals] Sim value tags toggle — persisted with the app preferences
    // (the same setting as the Preferences dialog's "Show sim signal values
    // on the editor graphs" checkbox); toolbar-less, menu-only.
    graphValuesAction_ =
        new QAction(QStringLiteral("Graph Signal &Values"), this);
    graphValuesAction_->setObjectName(QStringLiteral("view.graphValues"));  // smoke click seam
    graphValuesAction_->setCheckable(true);
    graphValuesAction_->setChecked(preferences_.showGraphValues);
    graphValuesAction_->setStatusTip(QStringLiteral(
        "With a sim session loaded, draw each telemetry channel's value at "
        "the graph ports that publish it (value-at-cursor while the "
        "playback cursor is pinned)."));
    connect(graphValuesAction_, &QAction::toggled, this, [this](bool on) {
        if (preferences_.showGraphValues != on) {
            preferences_.showGraphValues = on;
            SaveAppPreferences(preferences_);
        }
        UpdateGraphValueTags();
    });

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

namespace {

// Startup-captured platform defaults: after a Dark cycle, style()->
// standardPalette() stays mutated, so System restore needs these originals.
QVariantMap StartupLook() {
    static const QVariantMap captured = {
        {QStringLiteral("palette"), QApplication::palette()},
        {QStringLiteral("style"), QApplication::style()->objectName()},
    };
    return captured;
}

}  // namespace

namespace {

// freedesktop xdg-desktop-portal appearance.color-scheme: the only source a
// desktop session reliably agrees on for "system dark mode" (KDE/GNOME etc.,
// under both Wayland-native and xcb/Xwayland: portal answers the same).
// 0 = no preference, 1 = dark, 2 = light. On any failure: treat as light.
bool SystemPrefersDarkMode() {
    QDBusInterface portal(QStringLiteral("org.freedesktop.portal.Desktop"),
                          QStringLiteral("/org/freedesktop/portal/desktop"),
                          QStringLiteral("org.freedesktop.portal.Settings"),
                          QDBusConnection::sessionBus());
    if (!portal.isValid()) return false;
    const QDBusMessage reply = portal.call(
        QStringLiteral("Read"), QStringLiteral("org.freedesktop.appearance"),
        QStringLiteral("color-scheme"));
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) {
        return false;
    }
    QVariant v = reply.arguments().front();
    for (int unwrap = 0; unwrap < 2; ++unwrap) {
        if (v.userType() == qMetaTypeId<QDBusVariant>()) {
            v = v.value<QDBusVariant>().variant();
        } else {
            break;
        }
    }
    return v.toInt() == 1;
}

}  // namespace

void MainWindow::ApplyThemeMode(int mode) {
    const QVariantMap startup = StartupLook();
    const bool dark = (mode == 2) || (mode == 0 && SystemPrefersDarkMode());
    if (dark) {
        QApplication::setStyle(
            QStyleFactory::create(QStringLiteral("Fusion")));
        // Fusion-based dark palette (Qt docs "Dark Theme" recipe)
        QPalette dark;
        dark.setColor(QPalette::Window, QColor(53, 53, 53));
        dark.setColor(QPalette::WindowText, Qt::white);
        dark.setColor(QPalette::Base, QColor(35, 35, 35));
        dark.setColor(QPalette::AlternateBase, QColor(53, 53, 53));
        dark.setColor(QPalette::ToolTipBase, QColor(25, 25, 25));
        dark.setColor(QPalette::ToolTipText, Qt::white);
        dark.setColor(QPalette::Text, Qt::white);
        dark.setColor(QPalette::Button, QColor(53, 53, 53));
        dark.setColor(QPalette::ButtonText, Qt::white);
        dark.setColor(QPalette::BrightText, Qt::red);
        dark.setColor(QPalette::Link, QColor(42, 130, 218));
        dark.setColor(QPalette::Highlight, QColor(42, 130, 218));
        dark.setColor(QPalette::HighlightedText, QColor(35, 35, 35));
        dark.setColor(QPalette::Disabled, QPalette::Text,
                      QColor(127, 127, 127));
        dark.setColor(QPalette::Disabled, QPalette::WindowText,
                      QColor(127, 127, 127));
        dark.setColor(QPalette::Disabled, QPalette::ButtonText,
                      QColor(127, 127, 127));
        QApplication::setPalette(dark);
        qApp->setStyleSheet(QStringLiteral(
            "QToolTip { color: #ffffff; background-color: #232323; "
            "border: 1px solid #767676; }"));
        return;
    }
    QApplication::setStyle(
        QStyleFactory::create(startup.value(QStringLiteral("style")).toString()));
    QApplication::setPalette(
        startup.value(QStringLiteral("palette")).value<QPalette>());
    qApp->setStyleSheet(QString());
    if (mode == 1) {
        // Forced light on a dark host: Fusion without the dark palette is
        // the dependable light look.
        QApplication::setStyle(
            QStyleFactory::create(QStringLiteral("Fusion")));
        QApplication::setPalette(QApplication::style()->standardPalette());
    }
}

void MainWindow::ApplyPreferences(const AppPreferences& preferences) {
    preferences_ = preferences;
    ApplyThemeMode(preferences_.themeMode);
    if (view_) {
        view_->SetPanMouseButton(preferences_.panMouseButton);
        view_->SetGridSnap(preferences_.gridSnap);
    }
    if (graphScene_) {
        graphScene_->SetGridSnap(preferences_.gridSnap);
    }
    // [plant] Keep the plant screen in step with app preferences.
    if (plantScreen_) {
        plantScreen_->SetPanMouseButton(preferences_.panMouseButton);
        plantScreen_->SetGridSnap(preferences_.gridSnap);
        plantScreen_->SetUndoHistoryLimit(preferences_.undoHistoryLimit);
    }
    if (buildLogView_) {
        buildLogView_->setMaximumBlockCount(preferences_.buildLogLineLimit);
    }
    if (localSessionServer_) {
        localSessionServer_->SetExternalDeviceWritesEnabled(
            preferences_.allowExternalDeviceWrites);
    }
    // [gvals] The View-menu toggle is the mode's live surface; the
    // preferences dialog edits the same setting. (setChecked re-fires the
    // persisted-save only on a real change.)
    if (graphValuesAction_
        && graphValuesAction_->isChecked() != preferences_.showGraphValues) {
        graphValuesAction_->setChecked(preferences_.showGraphValues);
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
    // [plant] resolve screens by widget, not fixed index.
    const bool plantTab = plantScreen_ && screens_
        && tabIndex == screens_->indexOf(plantScreen_.data());
    const bool runtimeTab = runtimeTab_ && screens_
        && tabIndex == screens_->indexOf(runtimeTab_.data());
    if (nodeTab) {
        viewMenu_->addAction(frameContentAction_);
        viewMenu_->addAction(arrangeAction_);
        viewMenu_->addSeparator();
        for (auto* dock : {toolboxDock_.data(), inspectorDock_.data(),
                           editorConsoleDock_.data()}) {
            if (dock) {
                viewMenu_->addAction(dock->toggleViewAction());
            }
        }
        if (graphValuesAction_) {
            viewMenu_->addSeparator();
            viewMenu_->addAction(graphValuesAction_);
        }
    } else if (plantTab) {
        // [plant] The plant canvas frames content too; Auto Arrange stays a
        // Node-Editor action (plant positions are hand-authored).
        viewMenu_->addAction(frameContentAction_);
        viewMenu_->addSeparator();
        for (auto* dock : {plantToolboxDock_.data(), plantInspectorDock_.data()}) {
            if (dock) {
                viewMenu_->addAction(dock->toggleViewAction());
            }
        }
        if (graphValuesAction_) {
            viewMenu_->addSeparator();
            viewMenu_->addAction(graphValuesAction_);
        }
    } else if (runtimeTab) {
        for (auto* dock : {signalTableDock_.data(), telemetryConsoleDock_.data()}) {
            if (dock) {
                viewMenu_->addAction(dock->toggleViewAction());
            }
        }
    }

    // The five screen-switching actions stay menu-discoverable on every
    // screen (Ctrl+1..5; clunk audit item 11). The clear() at the top makes
    // repeated rebuilds refresh rather than duplicate them.
    if (!viewMenu_->isEmpty()) {
        viewMenu_->addSeparator();
    }
    for (QAction* action : screenActions_) {
        viewMenu_->addAction(action);
    }
}

void MainWindow::OnTabChanged(int index) {
    // Preserve each screen's dock layout (which panels, where) across
    // switches; the first visit of a screen gets its default arrangement.
    if (previousTab_ >= 0 && previousTab_ < 5) {  // [plant][sim] (array grew)
        screenStates_[previousTab_] = saveState();
    }
    previousTab_ = index;

    // [plant] resolve the plant screen by widget, not fixed index.
    const bool plantTab = plantScreen_ && screens_
        && index == screens_->indexOf(plantScreen_.data());

    if (index >= 0 && index < 5 && !screenStates_[index].isEmpty()) {  // [plant][sim]
        restoreState(screenStates_[index]);
    } else {
        // Defaults: node screen shows toolbox + inspector, plant screen its
        // own plant toolbox + inspector ( [plant] ), runtime shows the
        // signal table + telemetry console, and firmware update has no docks.
        // Panels from the other screens and the opt-in node console stay
        // hidden.
        const bool nodeTab = (index == 0);
        // [sim] resolve by widget: the Simulate screen sits between Plant and
        // Runtime, so a numeric position is never stable
        const bool runtimeTab = runtimeTab_ && screens_
            && index == screens_->indexOf(runtimeTab_.data());
        if (toolboxDock_) toolboxDock_->setVisible(nodeTab);
        if (inspectorDock_) inspectorDock_->setVisible(nodeTab);
        if (plantToolboxDock_) plantToolboxDock_->setVisible(plantTab);
        if (plantInspectorDock_) plantInspectorDock_->setVisible(plantTab);
        if (editorConsoleDock_) editorConsoleDock_->setVisible(false);
        if (signalTableDock_) signalTableDock_->setVisible(runtimeTab);
        if (telemetryConsoleDock_) telemetryConsoleDock_->setVisible(runtimeTab);
    }

    // [plant] The status line reflects the active tab's document.
    if (index == 0) {
        UpdateStatus();
    } else if (plantTab) {
        statusBar()->showMessage(plantScreen_->StatusText());
    }

    RebuildViewMenu();
    UpdateHistoryActions();
}

namespace {

// [gvals] Loose path identity for the value-tag binding guards: canonical
// when the file resolves, cleaned absolute otherwise (a stale scenario row
// must never crash the guard).
QString GraphValuesCanonPath(const QString& raw) {
    const QFileInfo info(raw.trimmed());
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath())
                               : canonical;
}

}  // namespace

// ---- Graph values mode ----------------------------------------------------
// Guard policy (bind cautiously — a wrong-document surface would lie):
//   * Node Editor binds when its open document IS the sim controller graph
//     (path equality against the scenario row). With no scenario row set —
//     a standalone "Load session..." — the controller surface alone binds,
//     since Debug.TelemetryLog channels are controller-side output.
//   * Plant Editor binds only on exact path equality with the sim plant
//     row; it is never guess-bound. (Plant graphs today carry no
//     Debug.TelemetryLog nodes, so a plant surface shows no tags yet — the
//     binding authority is the log nodes, per the mode contract.)
bool MainWindow::NodeGraphValuesBindingOk() const {
    if (!simulateScreen_ || currentPath_.empty()) {
        return false;
    }
    const QString simSw = simulateScreen_->SwGraphPath();
    if (simSw.isEmpty()) {
        return true;
    }
    return GraphValuesCanonPath(simSw)
        == GraphValuesCanonPath(QString::fromStdString(currentPath_));
}

bool MainWindow::PlantGraphValuesBindingOk() const {
    if (!simulateScreen_ || !plantScreen_ || plantScreen_->CurrentPath().empty()) {
        return false;
    }
    const QString simPlant = simulateScreen_->PlantGraphPath();
    if (simPlant.isEmpty()) {
        return false;
    }
    return GraphValuesCanonPath(simPlant)
        == GraphValuesCanonPath(
            QString::fromStdString(plantScreen_->CurrentPath()));
}

void MainWindow::UpdateGraphValueTags() {
    auto refill = [this](GraphScene* scene,
                         bool bindOk,
                         GraphValuesOverlay* overlay) {
        if (!scene || !overlay) {
            return;
        }
        const bool enabled = graphValuesAction_ && graphValuesAction_->isChecked();
        sim::SimTelemetryView* view =
            simulateScreen_ ? simulateScreen_->TelemetryView() : nullptr;
        if (!enabled || !bindOk || !view || !view->Session()) {
            overlay->Clear();
            return;
        }
        runtime::TelemetryStore& store = view->Store();
        std::vector<GraphValuesOverlay::Tag> tags;
        for (const GraphScene::TelemetryTagBinding& binding :
             scene->TelemetryTagBindings()) {
            float value = 0.0f;
            // Same source as the sim Value column: the pinned cursor's
            // ValueAt while one is held, the session tail otherwise.
            const bool ok =
                view->HasCursor()
                    ? store.ValueAt(binding.key,
                                    static_cast<float>(view->CursorTsec()),
                                    value)
                    : store.LatestValue(binding.key, value);
            if (!ok) {
                continue;  // channel absent from this session — no tag
            }
            tags.push_back({binding.nodeId, binding.key, binding.scenePos,
                            binding.anchorIsOutput,
                            QString::number(value, 'g', 6)});
        }
        overlay->ApplyTags(tags);
    };
    refill(graphScene_.get(), NodeGraphValuesBindingOk(), graphValuesNode_.get());
    refill(plantScreen_ ? plantScreen_->Scene() : nullptr,
           PlantGraphValuesBindingOk(), graphValuesPlant_.get());
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
    MarkControllerSaved();  // [unsaved] re-anchor the dirty baseline
    UpdateWindowTitle();  // [plant] (was an inline setWindowTitle call)
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

    // Reveal the debug dock and focus the Logs sub-tab WITHOUT switching
    // screens: teleporting to the Node Editor mid-flow breaks the
    // build-preview -> sim-preview loop (clunk audit item 4).
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
    if (buildCancelButton_) buildCancelButton_->setEnabled(!enabled);
}

void MainWindow::StartBuildCommand(BuildCommand command) {
    if (preferences_.automaticallyShowBuildLogs) {
        ShowBuildLogs();
    }

    if (!buildProcess_ || buildProcess_->state() != QProcess::NotRunning) {
        AppendBuildLog(QStringLiteral("\n[warning] A firmware operation is already running.\n"));
        return;
    }

    // Plant graphs carry no firmware: they belong to the Plant Editor /
    // rte sim path. Refuse before EnsureGraphSaved can overwrite the on-disk
    // file and without touching buildProcess_. The Node Editor's path only
    // points at a plant document after an on-disk kind change routed its
    // reload to the Plant Editor (routing keeps plant graphs out of
    // currentPath_ at open time).
    if (!currentPath_.empty() && GraphKindOf(currentPath_) != GraphKind::Controller) {
        ShowToast(QStringLiteral(
            "Plant graphs build no firmware — use the Plant Editor / rte sim path"));
        AppendBuildLog(QStringLiteral(
            "\n[refused] '%1' is a plant graph. Firmware builds start from a "
            "controller graph; plant graphs belong to the Plant Editor / rte sim path.\n")
                .arg(QString::fromStdString(currentPath_)));
        statusBar()->showMessage(
            QStringLiteral("Build refused: plant graphs belong to the Plant Editor / rte sim path"),
            6000);
        return;
    }

    // Always serialize the live graph before invoking external tools. For a
    // new graph this prompts for its first filename.
    if (!EnsureGraphSaved()) {
        AppendBuildLog(QStringLiteral("\n[cancelled] Graph was not saved.\n"));
        return;
    }

    // Any stage that reaches hardware asks FIRST: un-guarded F5 flashes from
    // whichever tab happens to be active with no visible target anywhere
    // (top scored violation across the 2026-10-21 dossier set: F5 silently
    // writes a device). Generate-only stages keep their old silent flow.
    if (command != BuildCommand::Generate) {
        const QString portLabel = runtimeController_
            ? runtimeController_->Port()
            : QStringLiteral("(no runtime port - the build's default)");
        const QString stageLabel =
            command == BuildCommand::Flash
                ? QStringLiteral("Flash")
                : QStringLiteral("Generate and Flash");
        QMessageBox confirm(this);
        confirm.setIcon(QMessageBox::Warning);
        confirm.setWindowTitle(QStringLiteral("%1 - confirm").arg(stageLabel));
        confirm.setText(QStringLiteral(
            "This will write the firmware binary to the device on %1.\n\n"
            "Graph: %2\nOperation: %3\n\nProceed?")
                .arg(portLabel)
                .arg(QString::fromStdString(currentPath_))
                .arg(stageLabel));
        auto* proceed = confirm.addButton(QStringLiteral("Flash"),
                                          QMessageBox::AcceptRole);
        confirm.addButton(QMessageBox::Cancel);
        confirm.setDefaultButton(QMessageBox::Cancel);
        confirm.exec();
        if (confirm.clickedButton() != proceed) {
            AppendBuildLog(QStringLiteral("\n[cancelled] flash refused by user.\n"));
            return;
        }
    }

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
    }

    AppendBuildLog(
        QStringLiteral("\n============================================================\n"
                       "%1\n"
                       "Graph: %2\n"
                       "Build: %3\n"
                       "============================================================\n")
            .arg(operationName,
                 absoluteGraphPath,
                 QString::fromStdString(firmwareBuildDir.string())));

    activeBuildCommand_ = command;
    pendingFirmwarePath_ = QString::fromStdString(firmwareBinary.string());
    cancelRequested_ = false;
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

    const std::filesystem::path firmwareBaseDir =
        FirmwareBaseDirForGraph(absoluteGraphPath.toStdString());

    QStringList arguments{
        QStringLiteral("--format"), QStringLiteral("jsonl"),
        command == BuildCommand::Generate ? QStringLiteral("generate")
                                           : QStringLiteral("build"),
        QStringLiteral("--graph"), absoluteGraphPath,
        QStringLiteral("--base-source"),
        QString::fromStdString(firmwareBaseDir.string()),
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

void MainWindow::OnBuildCancel() {
    if (!buildProcess_ || buildProcess_->state() == QProcess::NotRunning) {
        return;
    }
    // The kill runs through the normal finished wiring; the flag makes
    // FinishCliOperation report a cancellation instead of a failure.
    cancelRequested_ = true;
    AppendBuildLog(QStringLiteral("\n[cancelled] operation cancelled by user\n"));
    if (buildCancelButton_) {
        buildCancelButton_->setEnabled(false);
    }
    buildProcess_->kill();
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
    const bool cancelled = cancelRequested_;
    cancelRequested_ = false;
    if (finishedStage == CliStage::Flash && runtimeController_) {
        runtimeController_->ResumeAfterFlash();
    }
    if (success && finishedStage == CliStage::Build
        && activeBuildCommand_ == BuildCommand::GenerateAndFlash) {
        StartCliFlash(pendingFirmwarePath_);
        return;
    }
    cliStage_ = CliStage::None;
    if (cancelled) {
        AppendBuildLog(QStringLiteral("\n[finished] Operation cancelled by user.\n"));
        statusBar()->showMessage(QStringLiteral("Firmware operation cancelled"), 5000);
        SetBuildActionsEnabled(true);
        return;
    }
    AppendBuildLog(success
        ? QStringLiteral("\n[finished] Operation completed successfully.\n")
        : QStringLiteral("\n[finished] Operation failed (exit code %1).\n").arg(exitCode));
    if (success && finishedStage == CliStage::Flash) {
        // Post-flash crumb: the next thing the user wants is live telemetry.
        AppendBuildLog(QStringLiteral(
            "[hint] Firmware flashed — switch to Runtime (Ctrl+4) to watch telemetry.\n"));
        statusBar()->showMessage(
            QStringLiteral("Firmware flashed — switch to Runtime (Ctrl+4) to watch telemetry"),
            8000);
    } else {
        statusBar()->showMessage(success ? QStringLiteral("Firmware operation completed")
                                         : QStringLiteral("Firmware operation failed"), 5000);
    }
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
    // [unsaved] Gate on BOTH documents before anything else runs: a Cancel
    // leaves autosave, geometry and teardown untouched. Two dirty documents
    // produce two named prompts in sequence (controller first).
    if (!ConfirmControllerDiscard(QStringLiteral("close"))
        || !ConfirmPlantDiscard(QStringLiteral("close"))) {
        event->ignore();
        return;
    }
    if (runtimeTab_) {
        runtimeTab_->SaveAutosave();
    }
    SaveScreenStates();
    if (preferences_.rememberWindowGeometry) {
        QSettings settings(QStringLiteral("RTE"), QStringLiteral("RTEStudio"));
        settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    }
    QMainWindow::closeEvent(event);
}

int MainWindow::SmokeScreenCount() const { return screens_->count(); }

void MainWindow::SmokeSelectScreen(int index) {
    if (index >= 0 && index < screens_->count()) appSwitcher_->setCurrentIndex(index);
}

QString MainWindow::SmokeSaveActiveAs(const QString& path) {
    // Route through the menu save paths: the raw scene save used to sidestep
    // DoSave/DoSavePlant, leaving the document's current path, disk watcher,
    // window title and [unsaved] dirty anchor untouched - a menu-free save
    // must be semantics-equal, not just file-equal.
    if (plantScreen_ && screens_ && screens_->currentWidget() == plantScreen_.data()) {
        return DoSavePlant(path.toStdString()) ? QString{}
                                               : QStringLiteral("plant save failed");
    }
    return DoSave(path.toStdString()) ? QString{}
                                      : QStringLiteral("save failed");
}

bool MainWindow::SmokeLoadSimSession(const QString& path) {
    return simulateScreen_ && simulateScreen_->LoadSessionFile(path);
}

bool MainWindow::SmokeSetGraphValuesEnabled(bool on) {
    if (!graphValuesAction_) {
        return false;
    }
    graphValuesAction_->setChecked(on);
    return graphValuesAction_->isChecked() == on;
}

QString MainWindow::SmokeDumpGraphValues() {
    // Synchronous refill first: a scripted read right after a cursor poke
    // must not hinge on the 4 Hz timer having ticked.
    UpdateGraphValueTags();
    GraphScene* active = ActiveGraphScene();
    GraphValuesOverlay* overlay =
        (active && plantScreen_ && active == plantScreen_->Scene())
            ? graphValuesPlant_.get()
            : graphValuesNode_.get();
    if (!overlay) {
        return {};
    }
    QStringList pairs;
    for (const auto& [key, text] : overlay->DumpPairs()) {
        pairs << QString::fromStdString(key) + QStringLiteral("=") + text;
    }
    return pairs.join(QStringLiteral(";"));
}

void MainWindow::SmokePickNode(const std::string& nodeId) {
    GraphScene* active = nullptr;
    if (plantScreen_ && screens_ && screens_->currentWidget() == plantScreen_.data()) {
        active = plantScreen_->Scene();
    } else {
        active = graphScene_.get();
    }
    if (!active || !active->Scene()) return;
    const auto it = active->nodeIdMap_.find(nodeId);
    if (it == active->nodeIdMap_.end()) return;
    active->Scene()->clearSelection();
    if (auto* ngo = active->Scene()->nodeGraphicsObject(it->second)) {
        ngo->setSelected(true);   // selectionChanged fires -> inspector panels feed
    }
}

bool MainWindow::SmokePickConnection(const std::string& graphId) {
    GraphScene* active = ActiveGraphScene();
    return active && active->SelectConnection(graphId);
}

bool MainWindow::SmokeMoveNode(const std::string& nodeId, double dx, double dy) {
    GraphScene* active = ActiveGraphScene();
    return active && active->TranslateNode(nodeId, QPointF(dx, dy));
}

bool MainWindow::SmokeDragNode(const std::string& nodeId, double dx, double dy) {
    GraphScene* active = ActiveGraphScene();
    return active && active->DragNodeAsUser(nodeId, QPointF(dx, dy));
}

bool MainWindow::SmokePlainClick(const std::string& nodeId) {
    GraphScene* active = ActiveGraphScene();
    return active && active->PlainClickNode(nodeId);
}

bool MainWindow::SmokeMouseDragNode(const std::string& nodeId, double dx, double dy) {
    // Event-level mouse drag emulation: post press/move/release QMouseEvents
    // at the active view's viewport, aimed at the node's body - the full
    // widget chain a physical drag produces (2026-10-21 OCD drag-snap
    // diagnosis tool).
    GraphScene* active = ActiveGraphScene();
    if (!active) {
        return false;
    }
    GraphView* view = (plantScreen_ && PlantTabActive())
                          ? plantScreen_->View()
                          : view_.get();
    if (!view) {
        return false;
    }
    const QPointF center = active->NodeCenterForSmoke(nodeId);
    if (center.isNull()) {
        return false;
    }
    view->centerOn(center);
    QCoreApplication::processEvents();
    const QPoint start = view->mapFromScene(center);
    const QPoint target = start + QPoint(std::lround(dx), std::lround(dy));
    auto postToViewport = [&](QEvent::Type type, const QPoint& pos) {
        QMouseEvent ev(type, QPointF(pos), view->mapToGlobal(pos),
                       Qt::LeftButton,
                       type == QEvent::MouseButtonRelease ? Qt::NoButton
                                                          : Qt::LeftButton,
                       Qt::NoModifier);
        QCoreApplication::sendEvent(view->viewport(), &ev);
        QCoreApplication::processEvents();
    };
    postToViewport(QEvent::MouseButtonPress, start);
    for (int i = 1; i <= 4; ++i) {
        const QPoint cur = start + (target - start) * (i / 4.0);
        postToViewport(QEvent::MouseMove, cur);
    }
    postToViewport(QEvent::MouseButtonRelease, target);
    return true;
}

QString MainWindow::SmokeDumpNodeGeometry(const std::string& nodeId) {
    GraphScene* active = ActiveGraphScene();
    if (!active) {
        return QStringLiteral("no scene");
    }
    return active->DumpNodeGeometry(nodeId);
}

QString MainWindow::SmokeSetConnectionLabel(const std::string& graphId,
                                            const QString& label) {
    GraphScene* active = ActiveGraphScene();
    if (!active) {
        return QStringLiteral("refused no scene");
    }
    const QtNodes::ConnectionId connId = active->FindQtConnectionId(graphId);
    const QString error = active->SetConnectionLabel(connId, label);
    if (!error.isEmpty()) {
        return QStringLiteral("refused ") + error;
    }
    if (active == graphScene_.get() && inspector_) {
        inspector_->RefreshNetLabel(connId, active->ConnectionLabel(connId));
    }
    return QStringLiteral("ok");
}

QString MainWindow::SmokeDumpConnection(const std::string& graphId) {
    GraphScene* active = ActiveGraphScene();
    if (!active) {
        return QStringLiteral("no scene");
    }
    return active->DumpConnectionInfo(graphId);
}

QString MainWindow::SmokeDumpConnectionRoute(const std::string& graphId) {
    GraphScene* active = ActiveGraphScene();
    if (!active) {
        return QStringLiteral("no scene");
    }
    return active->DumpConnectionRoute(graphId);
}

bool MainWindow::SmokeDragWireBend(const std::string& graphId,
                                   int index,
                                   double x,
                                   double y) {
    GraphScene* active = ActiveGraphScene();
    GraphView* view = view_;
    if (plantScreen_ && PlantTabActive()) {
        view = plantScreen_->View();
    }
    if (!active || !view) {
        return false;
    }
    const QtNodes::ConnectionId connId = active->FindQtConnectionId(graphId);
    if (connId.outNodeId == QtNodes::InvalidNodeId) {
        return false;
    }
    return view->SmokeDragWireBend(connId, index, QPointF(x, y));
}


QString MainWindow::SmokeSetNodeParameterInputs(const std::string& nodeId,
                                                const QString& csv) {
    GraphScene* active = ActiveGraphScene();
    if (!active) {
        return QStringLiteral("failed no scene");
    }
    // Same seam the inspector's per-parameter "wire" checkboxes commit
    // through: GraphScene applies the NodeAPI write + the port/geometry
    // reset that re-sizes the node. The full replacement list is passed
    // verbatim as comma-separated parameter names.
    std::vector<std::string> inputs;
    const QStringList names = csv.split(QLatin1Char(','), Qt::SkipEmptyParts);
    inputs.reserve(names.size());
    for (const QString& name : names) {
        inputs.push_back(name.trimmed().toStdString());
    }
    const QtNodes::NodeId qtId = active->FindQtNodeId(nodeId);
    if (qtId == QtNodes::InvalidNodeId) {
        return QStringLiteral("failed unknown node");
    }
    const QString error = active->SetNodeParameterInputs(qtId, inputs);
    return error.isEmpty() ? QStringLiteral("ok")
                           : QStringLiteral("failed ") + error;
}

QString MainWindow::SmokeSetNodeParameter(const std::string& nodeId,
                                          const std::string& name,
                                          const QString& value) {
    GraphScene* active = ActiveGraphScene();
    if (!active) {
        return QStringLiteral("failed no scene");
    }
    // Same scene write the inspector's value commit reaches (GraphScene::
    // SetNodeParameters re-lays the painted block and resets geometry).
    const auto node = active->Graph().FindNode(nodeId);
    if (!node) {
        return QStringLiteral("failed unknown node");
    }
    std::map<std::string, std::string> parameters = node->parameters;
    parameters[name] = value.toStdString();
    const QtNodes::NodeId qtId = active->FindQtNodeId(nodeId);
    if (qtId == QtNodes::InvalidNodeId) {
        return QStringLiteral("failed unknown node");
    }
    const QString error = active->SetNodeParameters(qtId, parameters);
    return error.isEmpty() ? QStringLiteral("ok")
                           : QStringLiteral("failed ") + error;
}

bool MainWindow::SmokeCornerPressNode(const std::string& nodeId) {
    GraphScene* active = ActiveGraphScene();
    GraphView* view = view_;
    if (plantScreen_ && PlantTabActive()) {
        view = plantScreen_->View();
    }
    if (!active || !view || !active->Scene()) {
        return false;
    }
    const QtNodes::NodeId qtId = active->FindQtNodeId(nodeId);
    auto* ngo = qtId == QtNodes::InvalidNodeId
                    ? nullptr
                    : active->Scene()->nodeGraphicsObject(qtId);
    if (!ngo) {
        return false;
    }
    const QSize size = active->Scene()->nodeGeometry().size(qtId);
    const QPointF sceneStart = ngo->pos() + QPointF(size.width(), size.height())
                               - QPointF(2.0, 2.0);
    const QPoint start = view->mapFromScene(sceneStart);
    smokeCornerView_ = view;
    smokeCornerStart_ = start;

    QMouseEvent hover(QEvent::MouseMove, QPointF(start),
                      QPointF(view->viewport()->mapToGlobal(start)),
                      Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(view->viewport(), &hover);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(start),
                      QPointF(view->viewport()->mapToGlobal(start)),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view->viewport(), &press);
    return true;
}

void MainWindow::SmokeCornerRelease(int x, int y) {
    if (!smokeCornerView_) {
        return;
    }
    GraphView* view = smokeCornerView_;
    const QPoint start = smokeCornerStart_;
    const QPoint target(x, y);
    for (int step = 1; step <= 2; ++step) {
        QMouseEvent move(QEvent::MouseMove,
                         QPointF(start + (target - start) * step / 2),
                         QPointF(view->viewport()->mapToGlobal(
                             start + (target - start) * step / 2)),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(view->viewport(), &move);
    }
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(target),
                        QPointF(view->viewport()->mapToGlobal(target)),
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view->viewport(), &release);
    smokeCornerView_ = nullptr;
    QApplication::processEvents();
}

bool MainWindow::SmokeCornerDragNode(const std::string& nodeId,
                                     double dx,
                                     double dy) {
    GraphScene* active = ActiveGraphScene();
    GraphView* view = view_;
    if (plantScreen_ && PlantTabActive()) {
        view = plantScreen_->View();
    }
    if (!active || !view || !active->Scene()) {
        return false;
    }
    const QtNodes::NodeId qtId = active->FindQtNodeId(nodeId);
    auto* ngo = qtId == QtNodes::InvalidNodeId
                    ? nullptr
                    : active->Scene()->nodeGraphicsObject(qtId);
    if (!ngo) {
        return false;
    }
    const QSize size = active->Scene()->nodeGeometry().size(qtId);
    const QPointF sceneStart = ngo->pos() + QPointF(size.width(), size.height())
                               - QPointF(2.0, 2.0);
    if (!SmokeCornerPressNode(nodeId)) {
        return false;
    }
    const QPoint start = smokeCornerStart_;
    SmokeCornerRelease(start.x() + qRound(dx), start.y() + qRound(dy));
    return true;
}

QString MainWindow::SmokeAddNode(const QString& typeId, double x, double y) {
    GraphScene* active = ActiveGraphScene();
    if (!active) {
        return QStringLiteral("failed no scene");
    }
    const QString error = active->AddNodeAt(typeId.toStdString(), QPointF(x, y));
    if (!error.isEmpty()) {
        return QStringLiteral("failed ") + error;
    }
    // Graph::AddNode appends on success, so the new instance is the last one.
    const auto& nodes = active->Graph().GetNodes();
    if (!nodes.empty()) {
        return QStringLiteral("ok ") + QString::fromStdString(nodes.back().id);
    }
    return QStringLiteral("failed no node after add");
}

GraphScene* MainWindow::ActiveGraphScene() {
    if (plantScreen_ && PlantTabActive()) {
        return plantScreen_->Scene();
    }
    return graphScene_.get();
}

void MainWindow::OnWireBendAction(bool add) {
    GraphScene* active = ActiveGraphScene();
    if (!active) {
        return;
    }
    const std::vector<QtNodes::ConnectionId> selection = active->SelectedConnections();
    if (selection.size() != 1) {
        ShowToast(QStringLiteral("Select exactly one wire to bend"));
        return;
    }
    const QString error = add ? active->AddWireBend(selection.front())
                              : active->RemoveLastWireBend(selection.front());
    if (!error.isEmpty()) {
        ShowToast(error);
    }
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
    // [plant] File>Save saves the ACTIVE tab's document with its own path.
    if (PlantTabActive()) {
        if (plantScreen_->CurrentPath().empty()) {
            OnSaveAs();
            return;
        }
        DoSavePlant(plantScreen_->CurrentPath());
        return;
    }
    if (currentPath_.empty()) {
        OnSaveAs();
        return;
    }
    DoSave(currentPath_);
}

void MainWindow::OnSaveAs() {
    // [plant] Save As applies to the ACTIVE tab's document.
    if (PlantTabActive()) {
        const QString fileName = QFileDialog::getSaveFileName(this,
                                                              QStringLiteral("Save Plant Graph"),
                                                              QString{},
                                                              QStringLiteral("JSON (*.json)"));
        if (!fileName.isEmpty()) {
            DoSavePlant(fileName.toStdString());
        }
        return;
    }
    const QString fileName = QFileDialog::getSaveFileName(this,
                                                          QStringLiteral("Save Graph"),
                                                          QString{},
                                                          QStringLiteral("JSON (*.json)"));
    if (!fileName.isEmpty()) {
        DoSave(fileName.toStdString());
    }
}

void MainWindow::OnUndo() {
    // [plant] The Plant Editor keeps its own history stack.
    if (PlantTabActive()) {
        plantScreen_->Undo();
        return;
    }
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
    // [unsaved] Undo can land the document back on the saved snapshot
    // (clean again); refresh the title marker alongside the history actions.
    UpdateWindowTitle();
}

void MainWindow::OnRedo() {
    // [plant] The Plant Editor keeps its own history stack.
    if (PlantTabActive()) {
        plantScreen_->Redo();
        return;
    }
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
    UpdateWindowTitle();  // [unsaved] redo can re-dirty the document
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
    // [unsaved] Dirty-change hook: a newly recorded snapshot may have flipped
    // the document's dirty state, so re-compose the title marker.
    UpdateWindowTitle();
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
    // [plant] Undo/Redo follow the active tab's history stack.
    const bool plantEditorActive = PlantTabActive();
    if (undoAction_) {
        undoAction_->setEnabled(
            (nodeEditorActive && !undoHistory_.empty())
            || (plantEditorActive && plantScreen_ && plantScreen_->CanUndo()));
    }
    if (redoAction_) {
        redoAction_->setEnabled(
            (nodeEditorActive && !redoHistory_.empty())
            || (plantEditorActive && plantScreen_ && plantScreen_->CanRedo()));
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
    for (QGraphicsItem* item : graphScene_->Scene()->selectedItems()) {
        if (auto* cgo =
                qgraphicsitem_cast<QtNodes::ConnectionGraphicsObject*>(item)) {
            inspector_->ShowConnection(cgo->connectionId());
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
