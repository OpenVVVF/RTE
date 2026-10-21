#pragma once

#include "GraphScene.h"
#include "GraphView.h"
#include "PreferencesDialog.h"

#include "runtime/LocalSessionServer.h"
#include "runtime/RuntimeController.h"

#include <QByteArray>
#include <QMainWindow>
#include <QPointer>
#include <QVector>
#include <memory>
#include <string>
#include <vector>

class QAction;
class QDockWidget;
class QFileSystemWatcher;
class QFrame;
class QLabel;
class QMenu;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QStackedWidget;
class QTabBar;
class QTabWidget;
class QTimer;

namespace NodeGUI {

class InspectorPanel;
class NodePalette;
class PlantEditorScreen;  // [plant]
namespace sim { class SimulateScreen; }  // [sim]

namespace runtime {
class FlashPanel;
class RuntimeTab;
}

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    // Open a graph file at startup.
    bool OpenGraph(const std::string& path);

    // MRU (most-recently-used) graph files, shown in the File menu's
    // "Recent Files" submenu and persisted under file/recentFiles.
    // NoteRecentFile is called from every successful open path (both
    // editors); the submenu rebuilds from the settings each time the File
    // menu opens (persona + clunk dossier MRU-files row).
    QMenu* recentFilesMenu_ = nullptr;
    void NoteRecentFile(const QString& path);
    void RebuildRecentFilesMenu();

    // Frame the editor canvas to the full document content (called after
    // every successful load; also exposed in the View menu).
    void FrameCanvasToContent();

    GraphScene* Scene() const { return graphScene_.get(); }

    // Adds the top-level Runtime and Firmware Update tabs, then starts the
    // telemetry client and authenticated local automation session.
    void SetupRuntime(const QString& serialPort,
                      bool simulate,
                      runtime::Protocol protocol = runtime::Protocol::Legacy);

    // Smoke-driver seams (headless visual verification; see the RTE_SMOKE_DIR
    // env hook in Main.cpp): screen switching + count for scripted grabs.
    int SmokeScreenCount() const;
    void SmokeSelectScreen(int index);
    // [smoke] select+inspect a node by API id on the ACTIVE editor tab's
    // scene exactly like a user click would (fires the panel wiring).
    void SmokePickNode(const std::string& nodeId);
    // [smoke] select a wire by its graph id on the ACTIVE editor tab's scene.
    bool SmokePickConnection(const std::string& graphId);
    // [smoke] shift a node by (dx, dy) through the drag-complete path (snaps
    // when the grid preference is on). Returns false for unknown nodes.
    bool SmokeMoveNode(const std::string& nodeId, double dx, double dy);
    bool SmokeDragNode(const std::string& nodeId, double dx, double dy);
    bool SmokePlainClick(const std::string& nodeId);
    bool SmokeMouseDragNode(const std::string& nodeId, double dx, double dy);
    // [smoke] node origin + port socket scene positions for grid asserts.
    QString SmokeDumpNodeGeometry(const std::string& nodeId);
    // [smoke] save the ACTIVE editor tab's document to a path (menu-free).
    QString SmokeSaveActiveAs(const QString& path);
    // [smoke] load a sim session file through the Simulate screen's own
    // "Load session..." path (returns false on load failure).
    bool SmokeLoadSimSession(const QString& path);

private slots:
    void OnOpen();
    void OnNew();
    void OnSave();
    void OnSaveAs();
    void OnUndo();
    void OnRedo();
    void OnPreferences();
    void OnAutoArrange();
    void OnExit();
    void CheckForRejectionReason();
    void OnTabChanged(int index);
    void OnGraphFileChanged(const QString& path);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    enum class BuildCommand {
        Generate,
        Flash,
        GenerateAndFlash,
    };
    enum class CliStage { None, Generate, Build, Flash };

    void SetupMenu();
    void RegisterShortcut(QAction* action,
                          const QString& id,
                          const QString& category,
                          const QString& label,
                          const QKeySequence& defaultSequence);
    void ApplyPreferences(const AppPreferences& preferences);
    void ApplyThemeMode(int mode);
    void UpdateStatus();
    bool DoSave(const std::string& path);
    bool EnsureGraphSaved();
    void StartBuildCommand(BuildCommand command);
    void StartCliFlash(const QString& firmwarePath);
    void OnBuildCancel();
    void HandleCliOutput();
    void FinishCliOperation(bool success, int exitCode);
    void ShowBuildLogs();
    void AppendBuildLog(const QString& text);
    void SetBuildActionsEnabled(bool enabled);
    void ConnectModelSignals();
    void ResetHistory();
    void RecordHistorySnapshot();
    bool RestoreHistorySnapshot(const std::string& snapshot);
    void UpdateHistoryActions();
    // Rebuilds the View menu for the active tab: each screen has its own set
    // of dockable panels, and Auto Arrange only exists on the Node Editor.
    void RebuildViewMenu();
    // [plant] Plant Editor glue: routing plant graphs to the plant tab, the
    // per-active-tab File/Edit dispatch, and the two-document window title.
    bool OpenPlantGraph(const std::string& path);
    bool DoSavePlant(const std::string& path);
    bool PlantTabActive() const;
    void UpdateWindowTitle();
    // The scene the user is looking at right now (plant tab vs node tab).
    GraphScene* ActiveGraphScene();
    // Edit-menu route.* verbs: add/remove a manual bend on the single
    // selected wire of the active editor.
    void OnWireBendAction(bool add);
    // Right-click menu on a node: rename + pick its timing domain.
    void ShowNodeDomainMenu(const QPointF& globalPos, QtNodes::NodeId qtId);
    // Forwards scene selection to the inspector dock.
    void OnSceneSelectionChanged();

    // QtNodes' undo stack and paste/duplicate paths restore nodes straight
    // into its own model, bypassing the NodeAPI graph. Strip those shortcuts
    // rather than offer actions that silently desync a later save.
    void StripBrokenSceneActions();

    // Toast-style warning label anchored bottom-left of the canvas. Pairs
    // with an instant tooltip at the cursor; auto-hides on a timer.
    void ShowToast(const QString& message);
    void RepositionToast();

    // On-disk change notification for the currently opened graph: a
    // persistent banner at the top of the Node Editor offers Load / Save
    // Copy / Ignore until the user picks one (GitHub issue #33).
    void UpdateGraphWatcher();
    void ShowReloadBanner();
    void HideReloadBanner();

    std::unique_ptr<GraphScene> graphScene_;
    QPointer<GraphView> view_;
    QPointer<NodePalette> palette_;
    QPointer<InspectorPanel> inspector_;
    QPointer<QLabel> toast_;
    QTimer* toastTimer_ = nullptr;
    std::string currentPath_;
    bool connectionCreatedThisDrag_ = false;

    // Application-level screen switcher. The tab bar lives in the main-window
    // chrome above all docks; the stack contains only each screen's central
    // content.
    QTabBar* appSwitcher_ = nullptr;
    QStackedWidget* screens_ = nullptr;
    QPointer<QDockWidget> toolboxDock_;
    QPointer<QDockWidget> inspectorDock_;
    // [plant] Plant Editor screen (tab between Node Editor and Runtime): own
    // document + undo history, own palette/inspector panels hosted in these
    // per-screen docks.
    QPointer<PlantEditorScreen> plantScreen_;
    // [sim] Simulate screen instance (created after the Plant Editor).
    QPointer<sim::SimulateScreen> simulateScreen_;
    QPointer<QDockWidget> plantToolboxDock_;
    QPointer<QDockWidget> plantInspectorDock_;
    // Runtime-screen docks (created in SetupRuntime).
    QPointer<QDockWidget> signalTableDock_;
    QPointer<QDockWidget> telemetryConsoleDock_;
    // Node-screen detachable debug dock (device console + build logs).
    QPointer<QDockWidget> editorConsoleDock_;
    QPointer<QTabWidget> editorConsoleTabs_;
    QPointer<QPlainTextEdit> buildLogView_;
    QPointer<QPushButton> buildCancelButton_;
    QMenu* viewMenu_ = nullptr;
    QMenu* runtimeMenu_ = nullptr;
    QAction* saveFramKeysAction_ = nullptr;
    QAction* loadFramKeysAction_ = nullptr;
    QAction* frameContentAction_ = nullptr;
    QAction* arrangeAction_ = nullptr;
    // The Ctrl+1..5 screen-switching actions, in tab order; RebuildViewMenu
    // lists them permanently so the shortcuts are menu-discoverable.
    QVector<QAction*> screenActions_;
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QAction* cutAction_ = nullptr;
    QAction* copyAction_ = nullptr;
    QAction* deleteAction_ = nullptr;
    QAction* clearSelectionAction_ = nullptr;
    QAction* generateAction_ = nullptr;
    QAction* flashAction_ = nullptr;
    QAction* generateFlashAction_ = nullptr;
    QProcess* buildProcess_ = nullptr;
    CliStage cliStage_ = CliStage::None;
    BuildCommand activeBuildCommand_ = BuildCommand::Generate;
    QByteArray cliOutputBuffer_;
    // Set by the Cancel button before buildProcess_ is killed; the finished
    // handler consumes it to report a user cancellation instead of a failure
    // (mirrors SimulateScreen::cancelledByUser_).
    bool cancelRequested_ = false;
    QString pendingFirmwarePath_;
    QVector<ShortcutBinding> shortcutBindings_;
    AppPreferences preferences_;
    std::vector<std::string> undoHistory_;
    std::vector<std::string> redoHistory_;
    std::string currentHistorySnapshot_;
    bool restoringHistory_ = false;
    // Per-tab QMainWindow dock layouts, saved/restored on tab switches.
    // [plant] sized for Node + Plant + Runtime + Firmware Update screens.
    QByteArray screenStates_[5];
    int previousTab_ = 0;
    std::unique_ptr<runtime::RuntimeController> runtimeController_;
    std::unique_ptr<runtime::LocalSessionServer> localSessionServer_;
    QPointer<runtime::RuntimeTab> runtimeTab_;
    QPointer<runtime::FlashPanel> firmwareUpdateTab_;

    // Disk-change watch on the currently opened graph (see ShowReloadBanner).
    QFileSystemWatcher* graphWatcher_ = nullptr;
    QFrame* reloadBanner_ = nullptr;
    QLabel* reloadBannerText_ = nullptr;
    QTimer* reloadDebounce_ = nullptr;
    // True while we save the watched file ourselves, so our own write does
    // not raise the reload banner.
    bool suppressWatch_ = false;
};

}  // namespace NodeGUI
