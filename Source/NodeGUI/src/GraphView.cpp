#include "GraphView.h"

#include "NodeDataModel.h"
#include "NodeGraphModel.h"
#include "NodePalette.h"
#include "ParameterBlock.h"
#include "WireRoute.h"

#include <QtNodes/internal/AbstractNodeGeometry.hpp>
#include <QtNodes/internal/BasicGraphicsScene.hpp>
#include <QtNodes/internal/ConnectionGraphicsObject.hpp>
#include <QtNodes/internal/ConnectionState.hpp>
#include <QtNodes/internal/DataFlowGraphModel.hpp>
#include <QtNodes/internal/NodeGraphicsObject.hpp>

#include <QApplication>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QHelpEvent>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMainWindow>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QGraphicsProxyWidget>
#include <QScrollBar>
#include <QStatusBar>
#include <QToolTip>

#include <cmath>
#include <algorithm>
#include <map>
#include <vector>

namespace NodeGUI {

namespace {

void KeepOnlyNodesSelected(QGraphicsScene* scene) {
    if (!scene) {
        return;
    }
    for (QGraphicsItem* item : scene->selectedItems()) {
        if (!qgraphicsitem_cast<QtNodes::NodeGraphicsObject*>(item)) {
            item->setSelected(false);
        }
    }
}

QtNodes::NodeGraphicsObject* MovableNodeAncestor(QGraphicsItem* item) {
    for (; item; item = item->parentItem()) {
        // Controls embedded in a node manage their own mouse gestures.
        if (qgraphicsitem_cast<QGraphicsProxyWidget*>(item)) {
            return nullptr;
        }
        if (auto* node =
                qgraphicsitem_cast<QtNodes::NodeGraphicsObject*>(item)) {
            return node->flags().testFlag(QGraphicsItem::ItemIsMovable)
                       ? node
                       : nullptr;
        }
    }
    return nullptr;
}

// The topmost complete connection under the cursor, but only if no node sits
// above it (a node overlapping a wire wins the press, as it does today).
QtNodes::ConnectionGraphicsObject* WireItemAt(QGraphicsView* view,
                                              const QPoint& viewportPos) {
    for (QGraphicsItem* item : view->items(viewportPos)) {
        if (qgraphicsitem_cast<QtNodes::NodeGraphicsObject*>(item)) {
            return nullptr;
        }
        if (auto* cgo = qgraphicsitem_cast<QtNodes::ConnectionGraphicsObject*>(item)) {
            return cgo;
        }
    }
    return nullptr;
}

NodeGraphModel* WireModel(QtNodes::ConnectionGraphicsObject& cgo) {
    return dynamic_cast<NodeGraphModel*>(&cgo.graphModel());
}

// Half-size of the square a bend accepts grabs in (scene units).
constexpr double kWaypointPickRadius = 6.0;

int WaypointHandleAt(QtNodes::ConnectionGraphicsObject& cgo,
                     const NodeGraphModel& model,
                     const QPointF& scenePos) {
    const std::vector<QPointF>* waypoints = model.ConnectionWaypoints(cgo.connectionId());
    if (!waypoints) {
        return -1;
    }
    int best = -1;
    double bestDistance = kWaypointPickRadius;
    for (int i = 0; i < static_cast<int>(waypoints->size()); ++i) {
        const QPointF d = (*waypoints)[static_cast<std::size_t>(i)] - scenePos;
        const double distance = std::sqrt(QPointF::dotProduct(d, d));
        if (distance <= bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return best;
}

// Drops one bend from the wire and notifies the scene change pipeline.
void RemoveWireBendAt(QtNodes::ConnectionGraphicsObject& cgo,
                      NodeGraphModel& model,
                      int index) {
    const std::vector<QPointF>* stored = model.ConnectionWaypoints(cgo.connectionId());
    if (!stored || index < 0 || static_cast<std::size_t>(index) >= stored->size()) {
        return;
    }
    std::vector<QPointF> points = *stored;
    points.erase(points.begin() + index);

    cgo.move();
    model.SetConnectionWaypoints(cgo.connectionId(), std::move(points));
    cgo.update();
    if (model.onGraphChanged) {
        model.onGraphChanged();
    }
}

}  // namespace

void GraphView::TrackNodeDragAfterPress(QMouseEvent* event) {
    nodeDragPending_ = false;
    nodePressGrabValid_ = false;
    if (event->button() != Qt::LeftButton || !scene()) {
        return;
    }

    auto* node = MovableNodeAncestor(scene()->mouseGrabberItem());
    nodeDragPending_ = node && !node->nodeState().resizing();
    if (nodeDragPending_) {
        nodePressGrabId_ = node->nodeId();
        nodePressPos_ = node->pos();
        nodePressGrabValid_ = true;
    }
}

void GraphView::QuantizeOngoingNodeDrag() {
    // Release-side snapping alone corrects by at most half a pitch, which is
    // invisible mid-gesture - "the nodes never snap". Quantize DURING the
    // drag so the node glides in grid steps and lands visibly on-grid.
    if (!gridSnap_ || !scene()) {
        return;
    }
    auto* node = MovableNodeAncestor(scene()->mouseGrabberItem());
    if (!node || node->nodeState().resizing()) {
        return;
    }
    const QPointF snapped = WireRoute::SnapToGrid(node->pos());
    if (snapped != node->pos()) {
        node->setPos(snapped);
    }
}

void GraphView::ClearNodeDragCursor() {
    nodeDragPending_ = false;
    if (!nodeDragCursorActive_) {
        return;
    }
    nodeDragCursorActive_ = false;
    viewport()->unsetCursor();
}

void GraphView::SetPanMouseButton(Qt::MouseButton button) {
    panMouseButton_ =
        button == Qt::LeftButton ? Qt::LeftButton : Qt::MiddleButton;
    setDragMode(panMouseButton_ == Qt::LeftButton
                    ? QGraphicsView::ScrollHandDrag
                    : QGraphicsView::RubberBandDrag);
    if (mousePanning_) {
        mousePanning_ = false;
        viewport()->unsetCursor();
    }
    nodeRubberBandSelecting_ = false;
}

void GraphView::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasFormat(kNodeTypeMimeFormat)) {
        event->acceptProposedAction();
        return;
    }
    QtNodes::GraphicsView::dragEnterEvent(event);
}

void GraphView::dragMoveEvent(QDragMoveEvent* event) {
    if (event->mimeData()->hasFormat(kNodeTypeMimeFormat)) {
        event->acceptProposedAction();
        return;
    }
    QtNodes::GraphicsView::dragMoveEvent(event);
}

void GraphView::dropEvent(QDropEvent* event) {
    if (event->mimeData()->hasFormat(kNodeTypeMimeFormat)) {
        if (onNodeTypeDropped) {
            const QString typeId =
                QString::fromUtf8(event->mimeData()->data(kNodeTypeMimeFormat));
            onNodeTypeDropped(typeId, mapToScene(event->position().toPoint()));
        }
        event->acceptProposedAction();
        return;
    }
    QtNodes::GraphicsView::dropEvent(event);
}

void GraphView::contextMenuEvent(QContextMenuEvent* event) {
    // Right-click on a bend handle removes it.
    if (scene()) {
        if (auto* cgo = WireItemAt(this, event->pos())) {
            if (auto* model = WireModel(*cgo);
                model && !cgo->connectionState().requiresPort()) {
                const int handle =
                    WaypointHandleAt(*cgo, *model, mapToScene(event->pos()));
                if (handle >= 0) {
                    RemoveWireBendAt(*cgo, *model, handle);
                    event->accept();
                    return;
                }
            }
        }
    }
    if (onNodeContextMenu) {
        // Walk up from the hit item so clicks on the painted parameter block
        // region (or any child) resolve to the node.
        QGraphicsItem* item = itemAt(event->pos());
        while (item) {
            if (auto* ngo = qgraphicsitem_cast<QtNodes::NodeGraphicsObject*>(item)) {
                onNodeContextMenu(event->globalPos(), ngo->nodeId());
                event->accept();
                return;
            }
            item = item->parentItem();
        }
    }
    // Empty canvas: offer "Add node here" - the registry's type list grouped
    // by the palette's family prefix - instead of a dead right-click.
    if (onNodeTypeDropped) {
        QGraphicsItem* item = itemAt(event->pos());
        while (item && !qgraphicsitem_cast<QtNodes::NodeGraphicsObject*>(item)) {
            item = item->parentItem();
        }
        if (!item && PopupAddNodeMenu(event->globalPos(), mapToScene(event->pos()))) {
            event->accept();
            return;
        }
    }
    QtNodes::GraphicsView::contextMenuEvent(event);
}

void GraphView::mousePressEvent(QMouseEvent* event) {
    ClearNodeDragCursor();

    if (event->button() == Qt::LeftButton &&
        onDomainDragStarted &&
        onDomainDragStarted(
            mapToScene(event->position().toPoint()))) {
        domainDragging_ = true;
        domainDragLastScenePosition_ =
            mapToScene(event->position().toPoint());
        viewport()->setCursor(Qt::SizeAllCursor);
        event->accept();
        return;
    }

    // Left press on a wire (its stroke, via the scene hit shape): Ctrl+click
    // on a bend handle deletes that bend; unmodified press click-selects and
    // arms a bend drag that the move handler materializes past the platform
    // drag threshold.
    if (event->button() == Qt::LeftButton && scene()) {
        if (auto* cgo = WireItemAt(this, event->position().toPoint())) {
            auto* model = WireModel(*cgo);
            const auto& cstate = cgo->connectionState();
            if (model && !cstate.requiresPort() && !cstate.frozen()) {
                const QPointF scenePos = mapToScene(event->position().toPoint());
                const int handle = WaypointHandleAt(*cgo, *model, scenePos);
                if (event->modifiers() & Qt::ControlModifier) {
                    if (handle >= 0) {
                        RemoveWireBendAt(*cgo, *model, handle);
                        event->accept();
                        return;
                    }
                } else {
                    if (event->modifiers() & Qt::ShiftModifier) {
                        cgo->setSelected(!cgo->isSelected());
                    } else if (!cgo->isSelected()) {
                        scene()->clearSelection();
                        cgo->setSelected(true);
                    }
                    wireDragPending_ = true;
                    wireDragConnection_ = cgo->connectionId();
                    wireDragHandleAtPress_ = handle;
                    wireDragPressViewport_ = event->position().toPoint();
                    wireDragPressScene_ = scenePos;
                    event->accept();
                    return;
                }
            }
        }
    }

    // QtNodes already implements left-button background panning while still
    // allowing normal node selection and dragging. Only intercept the middle
    // button; delegating the Left Mouse preference preserves those semantics.
    if (panMouseButton_ == Qt::MiddleButton && event->button() == Qt::MiddleButton) {
        mousePanning_ = true;
        panLastPosition_ = event->position().toPoint();
        viewport()->setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (panMouseButton_ == Qt::MiddleButton) {
        if (event->button() == Qt::LeftButton) {
            nodeRubberBandSelecting_ = false;
        }
        // Skip QtNodes' left-button scene translation while retaining normal
        // QGraphicsView node selection, rubber-band selection, and dragging.
        QGraphicsView::mousePressEvent(event);
        TrackNodeDragAfterPress(event);
        return;
    }
    QtNodes::GraphicsView::mousePressEvent(event);
    TrackNodeDragAfterPress(event);
}

void GraphView::mouseMoveEvent(QMouseEvent* event) {
    if (wireDragActive_) {
        if (!(event->buttons() & Qt::LeftButton)) {
            FinishWireDrag(false);
            event->accept();
            return;
        }
        UpdateWireDrag(mapToScene(event->position().toPoint()));
        event->accept();
        return;
    }
    if (wireDragPending_) {
        if (!(event->buttons() & Qt::LeftButton)) {
            wireDragPending_ = false;
            event->accept();
            return;
        }
        if ((event->position().toPoint() - wireDragPressViewport_).manhattanLength()
            >= QApplication::startDragDistance()) {
            if (BeginWireDrag()) {
                UpdateWireDrag(mapToScene(event->position().toPoint()));
            }
            event->accept();
            return;
        }
        event->accept();
        return;
    }

    if (domainDragging_) {
        if (!(event->buttons() & Qt::LeftButton)) {
            domainDragging_ = false;
            viewport()->unsetCursor();
            if (onDomainDragFinished) {
                onDomainDragFinished();
            }
            event->accept();
            return;
        }
        const QPointF currentScenePosition =
            mapToScene(event->position().toPoint());
        if (onDomainDragged) {
            onDomainDragged(currentScenePosition -
                            domainDragLastScenePosition_);
        }
        domainDragLastScenePosition_ = currentScenePosition;
        event->accept();
        return;
    }

    if (nodeDragPending_) {
        if (event->buttons() & Qt::LeftButton) {
            nodeDragCursorActive_ = true;
            viewport()->setCursor(Qt::SizeAllCursor);
        } else {
            ClearNodeDragCursor();
        }
    }

    if (mousePanning_) {
        if (!(event->buttons() & panMouseButton_)) {
            mousePanning_ = false;
            viewport()->unsetCursor();
            event->accept();
            return;
        }
        const QPoint currentPosition = event->position().toPoint();
        const QPoint difference = currentPosition - panLastPosition_;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - difference.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - difference.y());
        panLastPosition_ = currentPosition;
        event->accept();
        return;
    }
    if (panMouseButton_ == Qt::MiddleButton) {
        QGraphicsView::mouseMoveEvent(event);
        QuantizeOngoingNodeDrag();
        if ((event->buttons() & Qt::LeftButton) &&
            !rubberBandRect().isNull()) {
            nodeRubberBandSelecting_ = true;
            KeepOnlyNodesSelected(scene());
        }
        return;
    }
    QtNodes::GraphicsView::mouseMoveEvent(event);
    QuantizeOngoingNodeDrag();
}

void GraphView::mouseReleaseEvent(QMouseEvent* event) {
    if ((wireDragActive_ || wireDragPending_) && event->button() == Qt::LeftButton) {
        const bool commit = wireDragActive_;
        FinishWireDrag(commit);
        event->accept();
        return;
    }
    if (domainDragging_ && event->button() == Qt::LeftButton) {
        domainDragging_ = false;
        viewport()->unsetCursor();
        if (onDomainDragFinished) {
            onDomainDragFinished();
        }
        event->accept();
        return;
    }
    if (suppressDomainDoubleClickRelease_ &&
        event->button() == Qt::LeftButton) {
        suppressDomainDoubleClickRelease_ = false;
        event->accept();
        return;
    }

    if (mousePanning_ && event->button() == panMouseButton_) {
        mousePanning_ = false;
        viewport()->unsetCursor();
        event->accept();
        return;
    }
    const bool completedNodeRubberBand =
        nodeRubberBandSelecting_ && event->button() == Qt::LeftButton;

    // Drag-completion evidence BEFORE the base release (the base leads into
    // the graph items' release handlers, including the swap that triggers
    // the GraphScene's nodeClicked gate).
    if (event->button() == Qt::LeftButton && nodePressGrabValid_) {
        if (auto* basicScene = dynamic_cast<QtNodes::BasicGraphicsScene*>(scene())) {
            if (auto* ngo = basicScene->nodeGraphicsObject(nodePressGrabId_)) {
                const QPointF delta = ngo->pos() - nodePressPos_;
                if (onNodeDragProven
                    && std::max(std::abs(delta.x()), std::abs(delta.y())) > 1.0) {
                    onNodeDragProven(nodePressGrabId_);
                }
            }
        }
        nodePressGrabValid_ = false;
    }
    QtNodes::GraphicsView::mouseReleaseEvent(event);
    if (event->button() == Qt::LeftButton) {
        ClearNodeDragCursor();
    }
    if (completedNodeRubberBand) {
        KeepOnlyNodesSelected(scene());
    }
    if (event->button() == Qt::LeftButton) {
        nodeRubberBandSelecting_ = false;
    }
}

void GraphView::mouseDoubleClickEvent(QMouseEvent* event) {
    ClearNodeDragCursor();

    if (event->button() == Qt::LeftButton &&
        onDomainDoubleClicked &&
        onDomainDoubleClicked(
            mapToScene(event->position().toPoint()))) {
        const QPointF scenePosition =
            mapToScene(event->position().toPoint());
        domainDragging_ =
            onDomainDragStarted &&
            onDomainDragStarted(scenePosition);
        if (domainDragging_) {
            domainDragLastScenePosition_ = scenePosition;
            viewport()->setCursor(Qt::SizeAllCursor);
            suppressDomainDoubleClickRelease_ = false;
        } else {
            suppressDomainDoubleClickRelease_ = true;
        }
        event->accept();
        return;
    }
    QtNodes::GraphicsView::mouseDoubleClickEvent(event);
    TrackNodeDragAfterPress(event);
}

void GraphView::keyPressEvent(QKeyEvent* event) {
    // Arrow keys move the selected nodes: one grid step per press, x5 with
    // Shift. Any other modifier (or key) defers to the base handlers, so
    // Ctrl+arrows and friends keep their platform meaning. Key autorepeat is
    // welcome: each press is one nudge (one undo entry per press is within
    // the sanctioned 'per series' design for this gesture).
    if ((event->modifiers() & ~Qt::ShiftModifier) == 0 &&
        scene()) {
        QPointF direction;
        switch (event->key()) {
        case Qt::Key_Left:  direction = {-1.0, 0.0}; break;
        case Qt::Key_Right: direction = { 1.0, 0.0}; break;
        case Qt::Key_Up:    direction = {0.0, -1.0}; break;
        case Qt::Key_Down:  direction = {0.0,  1.0}; break;
        default: break;
        }
        if (!direction.isNull() &&
            NudgeSelectedNodes(direction,
                               (event->modifiers() & Qt::ShiftModifier) ? 5 : 1)) {
            event->accept();
            return;
        }
    }
    QtNodes::GraphicsView::keyPressEvent(event);
}

bool GraphView::NudgeSelectedNodes(const QPointF& direction, int steps) {
    auto* basicScene = dynamic_cast<QtNodes::BasicGraphicsScene*>(scene());
    if (!basicScene) {
        return false;
    }
    std::vector<QtNodes::NodeGraphicsObject*> nodes;
    for (QGraphicsItem* item : scene()->selectedItems()) {
        if (auto* node = qgraphicsitem_cast<QtNodes::NodeGraphicsObject*>(item);
            node && node->flags().testFlag(QGraphicsItem::ItemIsMovable)) {
            nodes.push_back(node);
        }
    }
    if (nodes.empty()) {
        return false;
    }

    // A key nudge is a programmatic drag: mimic GraphScene::DragNodeAsUser
    // through the public chain (move ONLY the graphics object - never the
    // model's Position directly), leave the real-drag mark via
    // onNodeDragProven, then fire the release-side nodeClicked whose gated
    // handler completes the move: grid snap, domain outlines, position sync
    // into the graph, change notification (hence undo booking).
    const QPointF delta = direction * (WireRoute::kGridPitch * steps);
    for (auto* node : nodes) {
        const QtNodes::NodeId qtId = node->nodeId();
        node->setPos(node->pos() + delta);
        if (onNodeDragProven) {
            onNodeDragProven(qtId);
        }
        basicScene->nodeClicked(qtId);
    }

    // Cheap, discoverable, transient feedback: the moved node's snapped
    // scene position on the host window's status line for a moment.
    if (auto* mainWindow = qobject_cast<QMainWindow*>(window())) {
        const QPointF finalPos = nodes.front()->pos();
        mainWindow->statusBar()->showMessage(
            QStringLiteral("Node moved to (%1, %2)")
                .arg(finalPos.x(), 0, 'f', 0)
                .arg(finalPos.y(), 0, 'f', 0),
            2500);
    }
    return true;
}

bool GraphView::PopupAddNodeMenu(const QPoint& globalPos, const QPointF& scenePos) {
    auto* basicScene = dynamic_cast<QtNodes::BasicGraphicsScene*>(scene());
    auto* model = basicScene
                      ? dynamic_cast<QtNodes::DataFlowGraphModel*>(&basicScene->graphModel())
                      : nullptr;
    const auto registry = model ? model->dataModelRegistry() : nullptr;
    if (!registry || registry->registeredModelCreators().empty()) {
        return false;
    }

    // Type ids from the model's own type registry (the loader the palette's
    // SetNodeTypes mirrors), grouped with the palette's family-prefix rule -
    // same taxonomy, nothing re-derived from template files.
    std::map<QString, std::vector<QString>> families;
    for (const auto& [typeName, creator] : registry->registeredModelCreators()) {
        (void)creator;
        families[NodePalette::CategoryOf(typeName.toStdString())].push_back(typeName);
    }

    auto* menu = new QMenu(QStringLiteral("Add node here"));
    menu->setAttribute(Qt::WA_DeleteOnClose);
    for (auto& [family, typeIds] : families) {
        std::sort(typeIds.begin(), typeIds.end());
        QMenu* submenu = families.size() > 1 ? menu->addMenu(family) : menu;
        for (const QString& typeId : typeIds) {
            const QString label = typeId.mid(typeId.lastIndexOf(QLatin1Char('.')) + 1);
            QAction* action = submenu->addAction(label);
            action->setToolTip(typeId);
            QObject::connect(action, &QAction::triggered, this,
                             [this, typeId, scenePos]() {
                // Same seam as palette drops: the host's AddNodeAt applies
                // domain adoption and the grid snap itself.
                if (onNodeTypeDropped) {
                    onNodeTypeDropped(typeId, scenePos);
                }
            });
        }
    }
    menu->popup(globalPos);
    return true;
}

void GraphView::keyReleaseEvent(QKeyEvent* event) {
    QtNodes::GraphicsView::keyReleaseEvent(event);
    if (panMouseButton_ == Qt::MiddleButton &&
        event->key() == Qt::Key_Shift) {
        // QtNodes restores ScrollHandDrag when rubber-band selection ends.
        setDragMode(QGraphicsView::RubberBandDrag);
    }
}

bool GraphView::BeginWireDrag() {
    auto* basicScene = dynamic_cast<QtNodes::BasicGraphicsScene*>(scene());
    auto* cgo = basicScene
                    ? basicScene->connectionGraphicsObject(wireDragConnection_)
                    : nullptr;
    auto* model = cgo ? WireModel(*cgo) : nullptr;
    if (!model) {
        wireDragPending_ = false;
        return false;
    }

    std::vector<QPointF> points;
    if (const auto* stored = model->ConnectionWaypoints(wireDragConnection_)) {
        points = *stored;
    }

    if (wireDragHandleAtPress_ >= 0 &&
        static_cast<std::size_t>(wireDragHandleAtPress_) < points.size()) {
        // Grabbed an existing bend.
        wireDragWaypointIndex_ = wireDragHandleAtPress_;
        wireDragAnchor_ = points[static_cast<std::size_t>(wireDragWaypointIndex_)];
    } else {
        // Grabbed bare wire: split the nearest rendered leg with a new bend
        // at the grab point, then drag that bend.
        const QPointF out = cgo->mapToScene(cgo->endPoint(QtNodes::PortType::Out));
        const QPointF in = cgo->mapToScene(cgo->endPoint(QtNodes::PortType::In));
        const int leg = WireRoute::NearestRouteLeg(out, in, points, wireDragPressScene_, nullptr);
        if (leg < 0) {
            wireDragPending_ = false;
            return false;
        }
        wireDragWaypointIndex_ = leg;
        wireDragAnchor_ = gridSnap_
                              ? WireRoute::SnapToGrid(wireDragPressScene_)
                              : wireDragPressScene_;
        points.insert(points.begin() + leg, wireDragAnchor_);
        cgo->move();
        model->SetConnectionWaypoints(wireDragConnection_, points);
        cgo->update();
    }

    wireDragPending_ = false;
    wireDragActive_ = true;
    viewport()->setCursor(Qt::SizeAllCursor);
    return true;
}

void GraphView::UpdateWireDrag(const QPointF& scenePos) {
    auto* basicScene = dynamic_cast<QtNodes::BasicGraphicsScene*>(scene());
    auto* cgo = basicScene
                    ? basicScene->connectionGraphicsObject(wireDragConnection_)
                    : nullptr;
    auto* model = cgo ? WireModel(*cgo) : nullptr;
    const std::vector<QPointF>* stored =
        model ? model->ConnectionWaypoints(wireDragConnection_) : nullptr;
    if (!stored || wireDragWaypointIndex_ < 0 ||
        static_cast<std::size_t>(wireDragWaypointIndex_) >= stored->size()) {
        FinishWireDrag(false);
        return;
    }

    QPointF target = gridSnap_ ? WireRoute::SnapToGrid(scenePos) : scenePos;

    // Axis lock: when the pull direction is dominantly one axis, keep the
    // other axis pinned at the drag-start reference so legs born orthogonal
    // stay orthogonal. The dominant axis is measured from the press point
    // (unclamped), so wobble around the snap grid does not flip the lock.
    const QPointF direction = scenePos - wireDragPressScene_;
    if (std::abs(direction.x()) >= 2.0 * std::abs(direction.y()) &&
        std::abs(direction.x()) > 1e-3) {
        target.setY(wireDragAnchor_.y());
    } else if (std::abs(direction.y()) >= 2.0 * std::abs(direction.x())) {
        target.setX(wireDragAnchor_.x());
    }

    std::vector<QPointF> points = *stored;
    QPointF& current = points[static_cast<std::size_t>(wireDragWaypointIndex_)];
    if (current == target) {
        return;
    }
    current = target;

    cgo->move();
    model->SetConnectionWaypoints(wireDragConnection_, std::move(points));
    cgo->update();
}

void GraphView::FinishWireDrag(bool commit) {
    wireDragActive_ = false;
    wireDragPending_ = false;
    wireDragHandleAtPress_ = -1;
    wireDragWaypointIndex_ = -1;
    viewport()->unsetCursor();

    if (!commit) {
        return;
    }
    auto* basicScene = dynamic_cast<QtNodes::BasicGraphicsScene*>(scene());
    auto* cgo = basicScene
                    ? basicScene->connectionGraphicsObject(wireDragConnection_)
                    : nullptr;
    auto* model = cgo ? WireModel(*cgo) : nullptr;
    if (model && model->onGraphChanged) {
        model->onGraphChanged();
    }
}

bool GraphView::viewportEvent(QEvent* event) {
    if (event->type() != QEvent::ToolTip) {
        return QtNodes::GraphicsView::viewportEvent(event);
    }

    auto* helpEvent = static_cast<QHelpEvent*>(event);
    QtNodes::NodeGraphicsObject* nodeObject = nullptr;
    for (QGraphicsItem* hitItem : items(helpEvent->pos())) {
        QGraphicsItem* item = hitItem;
        while (item && !nodeObject) {
            nodeObject =
                qgraphicsitem_cast<QtNodes::NodeGraphicsObject*>(item);
            item = item->parentItem();
        }
        if (nodeObject) {
            break;
        }
    }

    if (!nodeObject) {
        return QtNodes::GraphicsView::viewportEvent(event);
    }

    auto* graphModel =
        dynamic_cast<QtNodes::DataFlowGraphModel*>(
            &nodeObject->graphModel());
    const auto* delegate =
        graphModel
            ? graphModel->delegateModel<NodeInstanceModel>(
                  nodeObject->nodeId())
            : nullptr;
    if (!delegate) {
        return QtNodes::GraphicsView::viewportEvent(event);
    }

    const QPointF nodePoint =
        nodeObject->mapFromScene(mapToScene(helpEvent->pos()));
    auto& geometry = nodeObject->nodeScene()->nodeGeometry();
    QString toolTip;
    for (const QtNodes::PortType portType :
         {QtNodes::PortType::In, QtNodes::PortType::Out}) {
        QtNodes::PortIndex portIndex =
            geometry.checkPortHit(nodeObject->nodeId(),
                                  portType,
                                  nodePoint);
        if (portIndex == QtNodes::InvalidPortIndex) {
            const QFontMetricsF metrics(QFont{});
            for (QtNodes::PortIndex index = 0;
                 index < delegate->nPorts(portType);
                 ++index) {
                const QString caption =
                    delegate->portCaption(portType, index);
                const QPointF textPosition =
                    geometry.portTextPosition(nodeObject->nodeId(),
                                              portType,
                                              index);
                const QRectF textRect(
                    textPosition.x() - 4.0,
                    textPosition.y() - metrics.ascent() - 4.0,
                    metrics.horizontalAdvance(caption) + 8.0,
                    metrics.height() + 8.0);
                if (textRect.contains(nodePoint)) {
                    portIndex = index;
                    break;
                }
            }
        }
        if (portIndex != QtNodes::InvalidPortIndex) {
            toolTip = delegate->PortToolTip(portType, portIndex);
            break;
        }
    }
    if (toolTip.isEmpty()) {
        const QSize nodeSize =
            geometry.size(nodeObject->nodeId());
        if (const auto parameter =
                ParameterAtPosition(delegate->ParameterBlock(),
                                    nodeSize,
                                    nodePoint)) {
            toolTip = delegate->ParameterToolTip(*parameter);
        }
    }
    if (toolTip.isEmpty()) {
        toolTip = delegate->NodeToolTip();
    }

    QToolTip::showText(helpEvent->globalPos(),
                       toolTip,
                       viewport());
    event->accept();
    return true;
}

void GraphView::showEvent(QShowEvent* event) {
    if (firstShow_) {
        firstShow_ = false;
        QtNodes::GraphicsView::showEvent(event);  // performs the initial centerScene()
        return;
    }
    QGraphicsView::showEvent(event);
}

}  // namespace NodeGUI
