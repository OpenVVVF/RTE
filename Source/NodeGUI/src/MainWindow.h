#pragma once

#include "GraphScene.h"
#include "GraphView.h"
#include "PreferencesDialog.h"

#include "runtime/FirmwareUpdater.h"
#include "runtime/HttpApiServer.h"
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
class QStackedWidget;
class QTabBar;
class QTabWidget;
class QTimer;

namespace NodeGUI {

class InspectorPanel;
class NodePalette;

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

    GraphScene* Scene() const { return graphScene_.get(); }

    // Adds the top-level Runtime and Firmware Update tabs, then starts the
    // telemetry client and HTTP API server.
    void SetupRuntime(const QString& serialPort,
                      bool simulate,
                      runtime::Protocol protocol = runtime::Protocol::Legacy);

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

    void SetupMenu();
    void RegisterShortcut(QAction* action,
                          const QString& id,
                          const QString& category,
                          const QString& label,
                          const QKeySequence& defaultSequence);
    void ApplyPreferences(const AppPreferences& preferences);
    void UpdateStatus();
    bool DoSave(const std::string& path);
    bool EnsureGraphSaved();
    void StartBuildCommand(BuildCommand command);
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
    // Runtime-screen docks (created in SetupRuntime).
    QPointer<QDockWidget> signalTableDock_;
    QPointer<QDockWidget> telemetryConsoleDock_;
    // Node-screen detachable debug dock (device console + build logs).
    QPointer<QDockWidget> editorConsoleDock_;
    QPointer<QTabWidget> editorConsoleTabs_;
    QPointer<QPlainTextEdit> buildLogView_;
    QMenu* viewMenu_ = nullptr;
    QAction* arrangeAction_ = nullptr;
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
    QVector<ShortcutBinding> shortcutBindings_;
    AppPreferences preferences_;
    std::vector<std::string> undoHistory_;
    std::vector<std::string> redoHistory_;
    std::string currentHistorySnapshot_;
    bool restoringHistory_ = false;
    // Per-tab QMainWindow dock layouts, saved/restored on tab switches.
    QByteArray screenStates_[3];
    int previousTab_ = 0;
    std::unique_ptr<runtime::RuntimeController> runtimeController_;
    std::unique_ptr<runtime::FirmwareUpdater> firmwareUpdater_;
    std::unique_ptr<runtime::HttpApiServer> httpApiServer_;
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
