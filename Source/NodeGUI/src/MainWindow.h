#pragma once

#include "GraphScene.h"
#include "GraphView.h"

#include <QMainWindow>
#include <QPointer>
#include <memory>

class QLabel;
class QTimer;

namespace NodeGUI {

class InspectorPanel;
class NodePalette;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    // Open a graph file at startup.
    bool OpenGraph(const std::string& path);

    GraphScene* Scene() const { return graphScene_.get(); }

private slots:
    void OnOpen();
    void OnNew();
    void OnSave();
    void OnSaveAs();
    void OnAutoArrange();
    void OnExit();
    void CheckForRejectionReason();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void SetupMenu();
    void UpdateStatus();
    bool DoSave(const std::string& path);
    void ConnectModelSignals();
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
};

}  // namespace NodeGUI
