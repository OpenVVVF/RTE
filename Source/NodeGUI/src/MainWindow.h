#pragma once

#include "GraphScene.h"

#include <QtNodes/GraphicsView>

#include <QMainWindow>
#include <QPointer>
#include <memory>

namespace NodeGUI {

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    // Open a graph file at startup.
    bool OpenGraph(const std::string& path);

private slots:
    void OnOpen();
    void OnAutoArrange();
    void OnExit();

private:
    void SetupMenu();
    void UpdateStatus();

    std::unique_ptr<GraphScene> graphScene_;
    QPointer<QtNodes::GraphicsView> view_;
};

}  // namespace NodeGUI
