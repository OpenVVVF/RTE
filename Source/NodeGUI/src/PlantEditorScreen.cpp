#include "PlantEditorScreen.h"

#include "GraphScene.h"
#include "GraphView.h"
#include "InspectorPanel.h"
#include "NodeGraphModel.h"
#include "NodePalette.h"
#include "ResourcePaths.h"

#include <QtNodes/internal/NodeGraphicsObject.hpp>

#include <NodeAPI/Graph.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <unordered_set>

#include <QAction>
#include <QByteArray>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QJsonArray>
#include <nlohmann/json.hpp>

#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMetaObject>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QStringList>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace NodeGUI {

namespace {

// The plant templates inherit 'isEntryPoint' from the controller node format
// (Plant.Io.DutyIn / Plant.Io.MeasOut); the plant contract treats it as
// informational metadata (README F-18): MeasOut collects its channels through
// ordinary incoming wires, which the controller-side rules (entry points must
// be roots) would reject. Sanitize the in-memory type database of a document
// so the GUI validation applies plant semantics; saves re-embed the types,
// and the engines classify against their own template library, so nothing
// downstream can observe the cleared flag.
void ClearEntryPointFlags(QJsonObject& document) {
    QJsonArray types = document.value(QStringLiteral("nodeTypes")).toArray();
    bool changed = false;
    for (int i = 0; i < types.size(); ++i) {
        QJsonObject type = types.at(i).toObject();
        if (type.value(QStringLiteral("isEntryPoint")).toBool(false)) {
            type[QStringLiteral("isEntryPoint")] = false;
            types[i] = type;
            changed = true;
        }
    }
    if (changed) {
        document[QStringLiteral("nodeTypes")] = types;
    }
}

// Sibling of DefaultPlantTemplatesDir(): the worked plant examples the hint
// link's file dialog seeds from (installed share/rte first, project root
// second — the InstalledResourceRoot walk used by the other resolvers).
std::string DefaultPlantExamplesDir() {
    const std::filesystem::path installed = InstalledResourceRoot()
        / "Assets" / "PlantExamples";
    if (std::filesystem::is_directory(installed)) return installed.lexically_normal().string();
    return (ProjectRoot() / "Assets" / "PlantExamples").lexically_normal().string();
}

}  // namespace

PlantEditorScreen::PlantEditorScreen(std::string plantTemplatesDir,
                                     QWidget* parent)
    : QWidget(parent)
    , graphScene_{std::make_unique<GraphScene>(QStringLiteral("plant"))}
    , templatesDir_{std::move(plantTemplatesDir)} {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // One-line contract reminder: what makes a plant graph different from a
    // controller graph (single implicit domain, no bridges). The engines
    // enforce these at load; the screen enforces them while editing. The
    // trailing link opens a file dialog seeded at the worked plant examples.
    auto* hint = new QLabel(
        QStringLiteral("Plant nodegraph (HW side of the loop) - single implicit domain "
                       "'plant', bridges rejected. Closed-loop simulation: rte sim. "
                       "<a href=\"open-example\">Open an example plant\u2026</a>"),
        this);
    hint->setTextFormat(Qt::RichText);
    hint->setStyleSheet(QStringLiteral(
        "QLabel { color: palette(mid); padding: 3px 8px; "
        "border-bottom: 1px solid palette(midlight); }"));
    connect(hint, &QLabel::linkActivated, this, [this](const QString&) {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Open Example Plant Graph"),
            QString::fromStdString(DefaultPlantExamplesDir()),
            QStringLiteral("JSON (*.json)"));
        if (path.isEmpty()) {
            return;
        }
        // [unsaved] Route through the MainWindow's OpenPlantGraph so the
        // dirty-document prompt runs before the discard (onOpenRequest is
        // installed by MainWindow; fall back to a direct open without it).
        if (onOpenRequest) {
            onOpenRequest(path.toStdString());
            return;
        }
        const QString error = OpenGraph(path.toStdString());
        if (!error.isEmpty()) {
            ReportError(error);
        }
    });
    layout->addWidget(hint);

    view_ = new GraphView(graphScene_->Scene());
    // The raster viewport outperformed a QOpenGLWidget viewport 3:1 on the
    // target machine (measured for the Node Editor; same trade-off here).
    view_->setOptimizationFlags(QGraphicsView::DontAdjustForAntialiasing);

    // Node type dropped from the palette: instantiate it at the drop point.
    // The scene assigns the 'plant' domain itself (its constructor default).
    view_->onNodeTypeDropped = [this](const QString& typeId, const QPointF& scenePos) {
        const QString error = graphScene_->AddNodeAt(typeId.toStdString(), scenePos);
        if (!error.isEmpty()) {
            ShowToast(error);
        }
    };

    // Right-click on a node: rename only. No 'Set domain...' - plant graphs
    // run one implicit domain and the engines reject bridges (the contract
    // hint above says the same), so per-node domains are not offered here.
    view_->onNodeContextMenu = [this](const QPointF& globalPos, QtNodes::NodeId qtId) {
        ShowNodeContextMenu(globalPos, qtId);
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

    // Ctrl+double-click on a wire: quick-edit its net label through an
    // instance-modal dialog - identical to the Node Editor's gesture (same
    // prompt, same refusal/uniqueness semantics in GraphScene::
    // SetConnectionLabel, same optional "netLabel" document key on save).
    view_->onNetLabelRequested = [this](QtNodes::ConnectionId connId) {
        QInputDialog dialog(this);
        dialog.setInputMode(QInputDialog::TextInput);
        dialog.setWindowTitle(QStringLiteral("Net label"));
        dialog.setLabelText(QStringLiteral(
            "Net label (empty clears; unique across all connections):"));
        dialog.setTextValue(graphScene_->ConnectionLabel(connId));
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        const QString error =
            graphScene_->SetConnectionLabel(connId, dialog.textValue());
        if (!error.isEmpty()) {
            ShowToast(error);
        } else if (inspector_) {
            inspector_->RefreshNetLabel(connId,
                                        graphScene_->ConnectionLabel(connId));
        }
    };

    layout->addWidget(view_);

    // Side panels handed up to the MainWindow's per-screen docks (parentless
    // here; QDockWidget::setWidget takes ownership).
    palette_ = new NodePalette;
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
    inspector_ = new InspectorPanel;
    inspector_->Attach(graphScene_.get());
    inspector_->onError = [this](const QString& message) { ShowToast(message); };

    // Toast-style warning label, bottom-left of the canvas. Child of this
    // container (not the viewport, whose children get dragged by scroll
    // deltas), raised above the view, hidden until a refused action.
    toast_ = new QLabel(this);
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

    // The viewport's Resize event carries the final post-layout size (and
    // covers scrollbar appear/disappear); the mouse events feed the
    // connection-rejection toast below.
    view_->viewport()->installEventFilter(this);

    // Selection gestures offered by this screen (own actions so the Node
    // Editor's Edit-menu wiring never crosses documents).
    deleteAction_ = new QAction(QStringLiteral("Delete Selection"), this);
    deleteAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    deleteAction_->setShortcut(QKeySequence(QKeySequence::Delete));
    connect(deleteAction_, &QAction::triggered, this, [this] {
        if (view_) {
            view_->onDeleteSelectedObjects();
        }
    });
    view_->addAction(deleteAction_);

    clearSelectionAction_ = new QAction(QStringLiteral("Clear Selection"), this);
    clearSelectionAction_->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    clearSelectionAction_->setShortcut(QKeySequence(Qt::Key_Escape));
    connect(clearSelectionAction_, &QAction::triggered, this, [this] {
        if (graphScene_->Scene()) {
            graphScene_->Scene()->clearSelection();
        }
    });
    view_->addAction(clearSelectionAction_);

    // plant workflow rules on the interactive validator: cross-domain
    // bridges are disallowed, and the controller cycle rule does not apply
    // (the bench's physical feedback ring is legal under the frozen plant
    // scheduler's PRE/POST + cycle-lock semantics)
    graphScene_->SetPlantWorkflow(true);

    // Every NodeAPI-reaching mutation (add/drop, delete, connect, move,
    // rename, parameter edit) funnels through this callback.
    graphScene_->SetChangeCallback([this] { RecordHistorySnapshot(); });

    // Load the plant node-type templates right away so the palette and node
    // creation work before any graph file is opened; the sanitize pass drops
    // the informational controller entry-point flags (see the helper above).
    const QString templatesError = graphScene_->LoadTemplates(templatesDir_);
    if (templatesError.isEmpty()) {
        SanitizeEntryPointFlags();
        view_->setScene(graphScene_->Scene());
        StripBrokenViewActions();
        palette_->SetNodeTypes(graphScene_->Graph().GetNodeTypes());
        ConnectSceneSignals();
        if (!graphScene_->LoadWarning().isEmpty()) {
            ShowToast(graphScene_->LoadWarning());
        }
    } else {
        ShowToast(templatesError);
    }

    ResetHistory();
    // Fresh document starts clean (its first snapshot IS the saved anchor).
    savedSnapshot_ = currentHistorySnapshot_;
}

PlantEditorScreen::~PlantEditorScreen() {
    // Panels handed to the MainWindow docks are owned there by then; this
    // out-of-line destructor just keeps the incomplete-type members safe.
}

NodePalette* PlantEditorScreen::PalettePanel() const {
    return palette_;
}

InspectorPanel* PlantEditorScreen::InspectorPanelWidget() const {
    return inspector_;
}

QString PlantEditorScreen::OpenGraph(const std::string& path) {
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly)) {
        return QStringLiteral("Failed to read plant graph file: %1")
            .arg(QString::fromStdString(path));
    }
    QJsonParseError parseError;
    const QJsonDocument engineDoc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !engineDoc.isObject()) {
        return QStringLiteral("Failed to parse plant graph JSON: %1")
            .arg(parseError.errorString());
    }

    // Engine-format plant graphs (as produced by twin and consumed by the
    // plant engines) carry no GUI 'nodeTypes' array -- their types resolve
    // against the loaded template library. Merge the engine document onto a
    // snapshot of the template-only graph (the schemaVersion/name section
    // plus the embedded library), so the standard GUI document shape applies
    // regardless of whether the file came from an engine or from a previous
    // GUI save (saving re-embeds the library, which both engines accept).
    QJsonObject merged = QJsonDocument::fromJson(
                             QByteArray::fromStdString(graphScene_->Snapshot()))
                             .object();
    const QJsonObject engine = engineDoc.object();
    for (const char* key : {"name", "nodes", "connections", "bridges"}) {
        if (engine.contains(QLatin1String(key))) {
            merged[QString::fromLatin1(key)] = engine.value(QLatin1String(key));
        }
    }
    ClearEntryPointFlags(merged);

    // Same load-time semantic audit the shared GraphScene path enforces
    // (engines refuse these classes; NodeAPI load used to swallow them):
    // duplicate ids, dangling endpoints, double-wired inputs, unknown ports.
    {
        QStringList problems;
        std::unordered_set<std::string> ids;
        QJsonArray nodesArr = merged.value(QStringLiteral("nodes")).toArray();
        for (const QJsonValue& n : nodesArr) {
            const std::string id = n.toObject().value(QStringLiteral("id")).toString().toStdString();
            if (!ids.insert(id).second) {
                problems.push_back(QString::fromStdString("duplicate node id '" + id + "'"));
            }
        }
        QJsonArray connsArr = merged.value(QStringLiteral("connections")).toArray();
        std::unordered_set<std::string> connIds, wiredInputs;
        for (const QJsonValue& c : connsArr) {
            const QJsonObject& co = c.toObject();
            const std::string cid = co.value(QStringLiteral("id")).toString().toStdString();
            if (!cid.empty() && !connIds.insert(cid).second) {
                problems.push_back(QString::fromStdString("duplicate connection id '" + cid + "'"));
            }
            const QJsonObject from = co.value(QStringLiteral("from")).toObject();
            const QJsonObject to = co.value(QStringLiteral("to")).toObject();
            for (const auto& [ref, dir] : {std::pair{from, "from"}, std::pair{to, "to"}}) {
                const std::string nid = ref.value(QStringLiteral("nodeId")).toString().toStdString();
                if (ids.count(nid) == 0) {
                    problems.push_back(QString::fromStdString(
                        std::string("connection '") + cid + "' '" + dir +
                        "' endpoint references unknown node '" + nid + "'"));
                    continue;
                }
                const std::string type =
                    ref.value(QStringLiteral("type")).toString().toStdString();   // no-op when absent
                const std::string port = ref.value(QStringLiteral("portName")).toString().toStdString();
                // resolve via the node's type listed on the node record
                for (const QJsonValue& n : nodesArr) {
                    if (n.toObject().value(QStringLiteral("id")).toString().toStdString() == nid) {
                        const std::string nt = n.toObject().value(QStringLiteral("type")).toString().toStdString();
                        if (const auto t = graphScene_->Graph().FindNodeType(nt)) {
                            bool ok = false;
                            for (const auto& p : t->inputPorts) ok |= p.name == port;
                            for (const auto& p : t->outputPorts) ok |= p.name == port;
                            for (const auto& [pname, wtype] : t->parameterTypes) {
                                (void)wtype;
                                ok |= pname == port;
                            }
                            if (!ok) {
                                problems.push_back(QString::fromStdString(
                                    std::string("connection '") + cid + "' '" + dir + "' endpoint '" + nid +
                                    "' has no port '" + port + "'"));
                            }
                        }
                        break;
                    }
                }
            }
            const std::string dst = to.value(QStringLiteral("nodeId")).toString().toStdString() + "." +
                                    to.value(QStringLiteral("portName")).toString().toStdString();
            if (!wiredInputs.insert(dst).second) {
                problems.push_back(QString::fromStdString("input '" + dst + "' is wired more than once"));
            }
        }
        if (!problems.isEmpty()) {
            return QStringLiteral("Plant graph rejected (corrupt; %1 problems):\n%2")
                .arg(problems.size())
                .arg(problems.mid(0, 20).join(QStringLiteral("\n")));
        }
    }

    // Save-time extras merge needs the source document keys stashed (the
    // shared GraphScene::SaveGraph merges them back; description etc.).
    try {
        const nlohmann::json docJson = nlohmann::json::parse(
            QJsonDocument(engine).toJson(QJsonDocument::Compact).toStdString());
        graphScene_->originalTopKeys_.clear();
        static const std::unordered_set<std::string> kSchemaKeys = {
            "name", "schemaVersion", "nodes", "connections", "bridges", "nodeTypes",
        };
        for (const auto& [key, value] : docJson.items()) {
            if (kSchemaKeys.count(key) == 0) {
                graphScene_->originalTopKeys_[key] = value;
            }
        }
    } catch (const std::exception&) {
        graphScene_->originalTopKeys_.clear();   // extras loss beats a bad load
    }

    // Nodes whose type the loaded library does not resolve must hard-fail
    // here, the same way both engines reject the graph at load: the generic
    // scene load keeps such nodes in the document but renders nothing and
    // silently drops their connections on the next save.
    QStringList unknownTypes;
    const QJsonArray mergedNodes =
        merged.value(QStringLiteral("nodes")).toArray();
    for (const QJsonValue& value : mergedNodes) {
        const QString type =
            value.toObject().value(QStringLiteral("type")).toString();
        if (!graphScene_->Graph().FindNodeType(type.toStdString())) {
            if (!unknownTypes.contains(type)) {
                unknownTypes.push_back(type);
            }
        }
    }
    if (!unknownTypes.isEmpty()) {
        return QStringLiteral(
                   "Plant graph references node type(s) missing from the "
                   "loaded library '%1': %2")
            .arg(QString::fromStdString(templatesDir_),
                 unknownTypes.join(QStringLiteral(", ")));
    }

    // Detach the old scene before the restore below destroys and recreates it.
    if (view_) {
        view_->setScene(nullptr);
    }
    const QString loadError = graphScene_->RestoreSnapshot(
        QJsonDocument(merged).toJson(QJsonDocument::Compact).toStdString());
    if (!loadError.isEmpty()) {
        // RestoreSnapshot leaves the previous document untouched on failure;
        // re-attach so the canvas is not left blank. The still-alive scene
        // keeps its existing signal wiring (no re-connect needed).
        if (view_) {
            view_->setScene(graphScene_->Scene());
            StripBrokenViewActions();
        }
        return QStringLiteral("Failed to load plant graph: %1").arg(loadError);
    }

    view_->setScene(graphScene_->Scene());
    StripBrokenViewActions();
    palette_->SetNodeTypes(graphScene_->Graph().GetNodeTypes());
    ConnectSceneSignals();

    currentPath_ = path;
    ResetHistory();
    // [unsaved] A freshly loaded document is clean; anchor before the
    // change notification so the window title reads the settled state.
    savedSnapshot_ = currentHistorySnapshot_;
    NotifyDocumentChanged();
    NotifyPathAssigned();
    FrameToContent();
    return {};
}

QString PlantEditorScreen::NewGraph() {
    if (view_) {
        view_->setScene(nullptr);
    }
    const QString error = graphScene_->NewGraph();
    if (!error.isEmpty()) {
        // Re-attach so the canvas is not left blank on failure (mirrors the
        // Node Editor's unconditional re-attach in OnNew).
        if (view_) {
            view_->setScene(graphScene_->Scene());
            StripBrokenViewActions();
        }
        return error;
    }
    // NewGraph re-reads the templates from disk; re-apply the plant-side
    // entry-point sanitize to the freshly loaded type records.
    SanitizeEntryPointFlags();

    view_->setScene(graphScene_->Scene());
    StripBrokenViewActions();
    ConnectSceneSignals();

    currentPath_.clear();
    ResetHistory();
    // [unsaved] A new document starts clean.
    savedSnapshot_ = currentHistorySnapshot_;
    NotifyDocumentChanged();
    return {};
}

QString PlantEditorScreen::SaveGraph(const std::string& path) {
    graphScene_->SyncPositionsFromScene();
    const QString error = graphScene_->SaveGraph(path);
    if (!error.isEmpty()) {
        return QStringLiteral("Failed to save plant graph: %1").arg(error);
    }
    currentPath_ = path;
    // [unsaved] Successful save re-anchors the dirty baseline. Re-anchor the
    // history head to the same string too (a mid-drag SyncPositionsFromScene
    // can leave the recorded head one tick behind the persisted content).
    currentHistorySnapshot_ = graphScene_->Snapshot();
    savedSnapshot_ = currentHistorySnapshot_;
    NotifyDocumentChanged();
    NotifyPathAssigned();
    return {};
}

void PlantEditorScreen::SanitizeEntryPointFlags() {
    QJsonObject document = QJsonDocument::fromJson(
                               QByteArray::fromStdString(graphScene_->Snapshot()))
                               .object();
    ClearEntryPointFlags(document);
    // Rewrite the live graph with the sanitized type database (RebuildScene
    // keeps node ids/positions intact).
    const QString error = graphScene_->RestoreSnapshot(
        QJsonDocument(document).toJson(QJsonDocument::Compact).toStdString());
    if (!error.isEmpty()) {
        ShowToast(error);
    }
}

QString PlantEditorScreen::StatusText() const {
    const auto& graph = graphScene_->Graph();
    return QStringLiteral("Nodes: %1 | Connections: %2")
        .arg(graph.GetNodes().size())
        .arg(graph.GetConnections().size());
}

void PlantEditorScreen::FrameToContent() {
    if (!view_ || !graphScene_) {
        return;
    }
    const QRectF bounds = graphScene_->Scene()->itemsBoundingRect();
    if (bounds.isEmpty()) {
        return;
    }
    view_->fitInView(bounds.adjusted(-60, -60, 60, 60), Qt::KeepAspectRatio);
}

void PlantEditorScreen::SetPanMouseButton(Qt::MouseButton button) {
    if (view_) {
        view_->SetPanMouseButton(button);
    }
}

void PlantEditorScreen::SetGridSnap(bool on) {
    if (view_) {
        view_->SetGridSnap(on);
    }
    if (graphScene_) {
        graphScene_->SetGridSnap(on);
    }
}

void PlantEditorScreen::ConnectSceneSignals() {
    if (!graphScene_->Model() || !graphScene_->Scene()) {
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
    ApplyPlantChrome();
    connect(graphScene_->Scene(),
            &QGraphicsScene::selectionChanged,
            this,
            [this] { OnSceneSelectionChanged(); });
}

void PlantEditorScreen::OnSceneSelectionChanged() {
    if (!graphScene_->Scene()) {
        inspector_->Clear();
        ApplyPlantChrome();
        return;
    }
    for (QGraphicsItem* item : graphScene_->Scene()->selectedItems()) {
        if (auto* ngo = qgraphicsitem_cast<QtNodes::NodeGraphicsObject*>(item)) {
            inspector_->ShowNode(ngo->nodeId());
            ApplyPlantChrome();
            return;
        }
    }
    inspector_->Clear();
    ApplyPlantChrome();
}

void PlantEditorScreen::ApplyPlantChrome() {
    if (!inspector_) {
        return;
    }
    // The generic inspector's form is rebuilt per selection; lock down the
    // rows whose writes the plant engines reject at load, each time.
    for (QComboBox* combo : inspector_->findChildren<QComboBox*>()) {
        combo->setEnabled(false);
        combo->setToolTip(QStringLiteral(
            "Plant graphs run a single implicit domain ('plant'); per-node "
            "domains and bridges are rejected by the plant engines."));
    }
    for (QCheckBox* check : inspector_->findChildren<QCheckBox*>()) {
        const QString text = check->text();
        if (text == QStringLiteral("Exclude from compile")
            || text == QStringLiteral("and children")) {
            check->setEnabled(false);
            check->setToolTip(QStringLiteral(
                "Exclusion is a controller-graph feature; the plant engines "
                "reject 'excludeFromCompile' keys."));
        } else if (text == QStringLiteral("wire")) {
            check->setEnabled(false);
            check->setToolTip(QStringLiteral(
                "Parameter wiring is a controller-graph feature; plant "
                "graphs carry plain parameter values."));
        }
    }
}

void PlantEditorScreen::ShowNodeContextMenu(const QPointF& globalPos,
                                            QtNodes::NodeId qtId) {
    const std::string nodeId = graphScene_->NodeApiId(qtId);
    if (nodeId.empty()) {
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

    menu.exec(globalPos.toPoint());
}

void PlantEditorScreen::StripBrokenViewActions() {
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
        if (action == deleteAction_ || action == clearSelectionAction_) {
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

void PlantEditorScreen::ShowToast(const QString& message) {
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

void PlantEditorScreen::RepositionToast() {
    if (!toast_ || !view_) {
        return;
    }
    constexpr int margin = 12;
    const QPoint viewportBottomLeft =
        view_->viewport()->mapTo(this, QPoint(0, view_->viewport()->height()));
    toast_->move(viewportBottomLeft.x() + margin,
                 viewportBottomLeft.y() - toast_->height() - margin);
}

void PlantEditorScreen::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    RepositionToast();
}

bool PlantEditorScreen::eventFilter(QObject* watched, QEvent* event) {
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
                                          &PlantEditorScreen::CheckForRejectionReason,
                                          Qt::QueuedConnection);
            }
        }
        return false;
    }

    return QWidget::eventFilter(watched, event);
}

void PlantEditorScreen::CheckForRejectionReason() {
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

void PlantEditorScreen::ResetHistory() {
    undoHistory_.clear();
    redoHistory_.clear();
    currentHistorySnapshot_ = graphScene_ ? graphScene_->Snapshot() : std::string{};
}

void PlantEditorScreen::RecordHistorySnapshot() {
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

    if (static_cast<int>(undoHistory_.size()) > undoHistoryLimit_) {
        undoHistory_.erase(undoHistory_.begin(),
                           undoHistory_.begin()
                               + static_cast<std::ptrdiff_t>(undoHistory_.size()
                                   - static_cast<std::size_t>(undoHistoryLimit_)));
    }
    NotifyDocumentChanged();
}

void PlantEditorScreen::SetUndoHistoryLimit(int limit) {
    undoHistoryLimit_ = std::max(1, limit);
    if (static_cast<int>(undoHistory_.size()) > undoHistoryLimit_) {
        undoHistory_.erase(undoHistory_.begin(),
                           undoHistory_.begin()
                               + static_cast<std::ptrdiff_t>(undoHistory_.size()
                                   - static_cast<std::size_t>(undoHistoryLimit_)));
    }
}

void PlantEditorScreen::Undo() {
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
    NotifyDocumentChanged();
}

void PlantEditorScreen::Redo() {
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
    NotifyDocumentChanged();
}

bool PlantEditorScreen::RestoreHistorySnapshot(const std::string& snapshot) {
    if (!graphScene_ || !view_) {
        return false;
    }

    restoringHistory_ = true;
    view_->setScene(nullptr);
    const QString error = graphScene_->RestoreSnapshot(snapshot);
    view_->setScene(graphScene_->Scene());
    StripBrokenViewActions();
    palette_->SetNodeTypes(graphScene_->Graph().GetNodeTypes());
    ConnectSceneSignals();
    restoringHistory_ = false;

    if (!error.isEmpty()) {
        ShowToast(error);
        return false;
    }

    NotifyDocumentChanged();
    return true;
}

void PlantEditorScreen::NotifyDocumentChanged() {
    if (onDocumentChanged) {
        onDocumentChanged();
    }
    if (onStatusMessage) {
        onStatusMessage(StatusText());
    }
}

void PlantEditorScreen::NotifyPathAssigned() {
    // Fires only when currentPath_ is (re)assigned - open / save-as - never
    // on content edits, undo/redo, or NewGraph (whose cleared path must not
    // blank a manually browsed sim input downstream).
    if (onPathAssigned && !currentPath_.empty()) {
        onPathAssigned(currentPath_);
    }
}

}  // namespace NodeGUI

