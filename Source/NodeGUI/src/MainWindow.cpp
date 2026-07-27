#include "MainWindow.h"

#include "FrameRateMonitor.h"
#include "NodePalette.h"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QFileDialog>
#include <QInputDialog>
#include <QLineEdit>
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
#include <set>

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

    view_ = new GraphView(graphScene_->Scene());
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
            statusBar()->showMessage(error, 4000);
        }
    };

    // Right-click on a node: offer the domain selection menu.
    view_->onNodeContextMenu = [this](const QPointF& globalPos, QtNodes::NodeId qtId) {
        ShowNodeDomainMenu(globalPos, qtId);
    };

    layout->addWidget(view_);
    setCentralWidget(central);

    // Toolbox dock listing the available node types; entries are dragged onto
    // the view to instantiate them. Populated on OpenGraph.
    palette_ = new NodePalette(this);
    auto* toolboxDock = new QDockWidget(QStringLiteral("Node Toolbox"), this);
    toolboxDock->setObjectName(QStringLiteral("nodeToolboxDock"));
    toolboxDock->setWidget(palette_);
    addDockWidget(Qt::LeftDockWidgetArea, toolboxDock);

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

    palette_->SetNodeTypes(graphScene_->Graph().GetNodeTypes());

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

void MainWindow::ShowNodeDomainMenu(const QPointF& globalPos, QtNodes::NodeId qtId) {
    const std::string nodeId = graphScene_->NodeApiId(qtId);
    const auto node = nodeId.empty() ? std::nullopt : graphScene_->Graph().FindNode(nodeId);
    if (!node) {
        return;
    }

    // Types locked to a domain cannot be reassigned.
    const auto nodeType = graphScene_->Graph().FindNodeType(node->type);
    if (nodeType && !nodeType->domain.empty()) {
        statusBar()->showMessage(QStringLiteral("'%1' is locked to domain '%2' by its node type")
                                     .arg(QString::fromStdString(nodeId),
                                          QString::fromStdString(nodeType->domain)),
                                 4000);
        return;
    }

    // Existing domains in the graph, plus "no domain" and a custom entry.
    std::set<std::string> domains;
    for (const auto& n : graphScene_->Graph().GetNodes()) {
        if (!n.domain.empty()) {
            domains.insert(n.domain);
        }
    }

    QMenu menu(this);
    auto addDomainAction = [&](const QString& label, const std::string& value) {
        QAction* action = menu.addAction(label);
        action->setCheckable(true);
        action->setChecked(node->domain == value);
        connect(action, &QAction::triggered, this, [this, qtId, value] {
            const QString error = graphScene_->SetNodeDomain(qtId, value);
            if (!error.isEmpty()) {
                statusBar()->showMessage(error, 4000);
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
                statusBar()->showMessage(error, 4000);
            }
        }
    });

    menu.exec(globalPos.toPoint());
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
