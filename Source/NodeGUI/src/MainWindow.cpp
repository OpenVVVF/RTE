#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QFileDialog>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QScreen>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

#include <filesystem>

namespace NodeGUI {

namespace {

std::string DefaultTemplatesDir() {
#ifdef RTE_NODE_TEMPLATES_DIR
    return RTE_NODE_TEMPLATES_DIR;
#else
    return (std::filesystem::current_path() / "Assets" / "NodeTemplates").string();
#endif
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

    view_ = new QtNodes::GraphicsView(graphScene_->Scene());
    layout->addWidget(view_);
    setCentralWidget(central);

    SetupMenu();
    UpdateStatus();

    if (QScreen* screen = QApplication::primaryScreen()) {
        move(screen->availableGeometry().center() - frameGeometry().center());
    }
}

bool MainWindow::OpenGraph(const std::string& path) {
    // Detach the old scene before LoadGraph destroys it and recreates it.
    if (view_) {
        view_->setScene(nullptr);
    }

    const QString error = graphScene_->LoadGraph(path, DefaultTemplatesDir());
    if (!error.isEmpty()) {
        QMessageBox::critical(this, QStringLiteral("Failed to load graph"), error);
        return false;
    }

    if (view_) {
        view_->setScene(graphScene_->Scene());
    }

    currentPath_ = path;
    setWindowTitle(QStringLiteral("NodeGUI - %1").arg(QString::fromStdString(path)));
    UpdateStatus();
    return true;
}

void MainWindow::SetupMenu() {
    QMenu* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));

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

    QAction* exitAction = fileMenu->addAction(QStringLiteral("E&xit"));
    exitAction->setShortcuts(QKeySequence::Quit);
    connect(exitAction, &QAction::triggered, this, &MainWindow::OnExit);

    QMenu* viewMenu = menuBar()->addMenu(QStringLiteral("&View"));

    QAction* arrangeAction = viewMenu->addAction(QStringLiteral("&Auto Arrange"));
    arrangeAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+A")));
    connect(arrangeAction, &QAction::triggered, this, &MainWindow::OnAutoArrange);
}

bool MainWindow::DoSave(const std::string& path) {
    graphScene_->SyncPositionsFromScene();

    const QString error = graphScene_->SaveGraph(path);
    if (!error.isEmpty()) {
        QMessageBox::critical(this, QStringLiteral("Failed to save graph"), error);
        return false;
    }

    currentPath_ = path;
    setWindowTitle(QStringLiteral("NodeGUI - %1").arg(QString::fromStdString(path)));
    return true;
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

}  // namespace NodeGUI
