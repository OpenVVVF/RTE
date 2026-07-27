#pragma once

#include "GraphScene.h"
#include "GraphView.h"

#include <QMainWindow>
#include <QPointer>
#include <memory>

namespace NodeGUI {

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
    void OnSave();
    void OnSaveAs();
    void OnAutoArrange();
    void OnExit();
    void CheckForRejectionReason();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void SetupMenu();
    void UpdateStatus();
    bool DoSave(const std::string& path);
    void ConnectModelSignals();
    // Right-click menu on a node: pick its timing domain.
    void ShowNodeDomainMenu(const QPointF& globalPos, QtNodes::NodeId qtId);

    std::unique_ptr<GraphScene> graphScene_;
    QPointer<GraphView> view_;
    QPointer<NodePalette> palette_;
    std::string currentPath_;
    bool connectionCreatedThisDrag_ = false;
};

}  // namespace NodeGUI
