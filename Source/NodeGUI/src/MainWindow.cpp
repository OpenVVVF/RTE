#include "MainWindow.h"

#include "FrameRateMonitor.h"

#include <QAction>
#include <QApplication>
#include <QFileDialog>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QMouseEvent>
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
    view_->installEventFilter(this);

    // Note: measured on the target machine, a QOpenGLWidget viewport was ~3x
    // slower than the default raster viewport for this scene, so we keep the
    // raster path. Node-level DeviceCoordinateCache (set in GraphScene) is
    // what keeps pans and drags smooth.
    view_->setOptimizationFlags(QGraphicsView::DontAdjustForAntialiasing);

    layout->addWidget(view_);
    setCentralWidget(central);

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

    ConnectModelSignals();

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
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched != view_) {
        return QMainWindow::eventFilter(watched, event);
    }

    if (event->type() == QEvent::MouseButtonPress) {
        connectionCreatedThisDrag_ = false;
        if (graphScene_->Model()) {
            graphScene_->Model()->ClearRejectionState();
        }
    } else if (event->type() == QEvent::MouseButtonRelease) {
        if (!connectionCreatedThisDrag_ && graphScene_->Model()) {
            // Defer the check until QtNodes has finished processing the release,
            // so connectionCreated has already been emitted on success.
            QMetaObject::invokeMethod(this,
                                      &MainWindow::CheckForRejectionReason,
                                      Qt::QueuedConnection);
        }
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
            statusBar()->showMessage(reason, 4000);
        }
    }
}

}  // namespace NodeGUI
