#pragma once

#include "GraphScene.h"
#include "GraphView.h"

#include "runtime/FirmwareUpdater.h"
#include "runtime/HttpApiServer.h"
#include "runtime/RuntimeController.h"

#include <QByteArray>
#include <QMainWindow>
#include <QPointer>
#include <memory>

class QAction;
class QDockWidget;
class QLabel;
class QMenu;
class QTabWidget;
class QTimer;

namespace NodeGUI {

class InspectorPanel;
class NodePalette;

namespace runtime {
class RuntimeTab;
}

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    // Open a graph file at startup.
    bool OpenGraph(const std::string& path);

    GraphScene* Scene() const { return graphScene_.get(); }

    // Adds the "Runtime" tab (telemetry + firmware update) and starts the
    // telemetry client and HTTP API server.
    void SetupRuntime(const QString& serialPort,
                      bool simulate,
                      runtime::Protocol protocol = runtime::Protocol::Legacy);

private slots:
    void OnOpen();
    void OnNew();
    void OnSave();
    void OnSaveAs();
    void OnAutoArrange();
    void OnExit();
    void CheckForRejectionReason();
    void OnTabChanged(int index);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void SetupMenu();
    void UpdateStatus();
    bool DoSave(const std::string& path);
    void ConnectModelSignals();
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

    std::unique_ptr<GraphScene> graphScene_;
    QPointer<GraphView> view_;
    QPointer<NodePalette> palette_;
    QPointer<InspectorPanel> inspector_;
    QPointer<QLabel> toast_;
    QTimer* toastTimer_ = nullptr;
    std::string currentPath_;
    bool connectionCreatedThisDrag_ = false;

    QTabWidget* tabs_ = nullptr;
    QPointer<QDockWidget> toolboxDock_;
    QPointer<QDockWidget> inspectorDock_;
    // Runtime-screen docks (created in SetupRuntime).
    QPointer<QDockWidget> signalTableDock_;
    QPointer<QDockWidget> telemetryConsoleDock_;
    // Node-screen console dock (bottom area, hidden by default).
    QPointer<QDockWidget> editorConsoleDock_;
    QMenu* viewMenu_ = nullptr;
    QAction* arrangeAction_ = nullptr;
    // Per-tab QMainWindow dock layouts, saved/restored on tab switches.
    QByteArray screenStates_[2];
    int previousTab_ = 0;
    std::unique_ptr<runtime::RuntimeController> runtimeController_;
    std::unique_ptr<runtime::FirmwareUpdater> firmwareUpdater_;
    std::unique_ptr<runtime::HttpApiServer> httpApiServer_;
    QPointer<runtime::RuntimeTab> runtimeTab_;
};

}  // namespace NodeGUI
