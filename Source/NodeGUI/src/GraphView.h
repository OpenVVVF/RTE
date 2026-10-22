#pragma once

#include <QtNodes/Definitions>
#include <QtNodes/GraphicsView>

#include <QPointF>
#include <QSize>
#include <QString>
#include <Qt>

#include <functional>

class QMouseEvent;
class QKeyEvent;
class QEvent;

namespace NodeGUI {

// GraphicsView that accepts node types dragged from the NodePalette. Real
// drag & drop reaches these handlers via the viewport; synthetic drops can be
// delivered straight to the view in tests.
class GraphView : public QtNodes::GraphicsView {
public:
    using QtNodes::GraphicsView::GraphicsView;

    // Selects Left or Middle Mouse for canvas panning. Unsupported values
    // safely fall back to Middle Mouse.
    void SetPanMouseButton(Qt::MouseButton button);
    Qt::MouseButton PanMouseButton() const { return panMouseButton_; }

    // Whether wire-bend positions snap to the scene grid (shared preference,
    // default on).
    void SetGridSnap(bool on) { gridSnap_ = on; }

    // Called with the dragged node-type id and the drop position in scene
    // coordinates.
    std::function<void(const QString& typeId, const QPointF& scenePos)> onNodeTypeDropped;

    // Called when a node is right-clicked: global menu position + node id.
    std::function<void(const QPointF& globalPos, QtNodes::NodeId nodeId)> onNodeContextMenu;

    std::function<bool(const QPointF& scenePos)> onDomainDoubleClicked;
    std::function<bool(const QPointF& scenePos)> onDomainDragStarted;
    std::function<void(const QPointF& delta)> onDomainDragged;
    std::function<void()> onDomainDragFinished;

    // Ctrl+double-click on a wire: quick-edit that connection's display-only
    // net label (host shows the input dialog; the view only routes the id).
    // Plain double-click keeps its current meaning (domain activation).
    std::function<void(QtNodes::ConnectionId connId)> onNetLabelRequested;

    // Drag-completion evidence: the node's id + scene position captured at
    // left-press; on release, if the captured node moved beyond a scene-unit
    // wiggle, the host calls this BEFORE the base-class release runs - so the
    // GraphScene's release-side handler can tell drags from plain clicks
    // (2026-10-21: plain clicks used to snap off-grid nodes + book an undo;
    // coldboot dossier finding).
    std::function<void(QtNodes::NodeId qtId)> onNodeDragProven;

    // Smoke: drive the REAL bend-drag state machine (press-armed Begin ->
    // Update -> release commit) for waypoint `index` of `connId` toward a
    // scene target, without synthesizing mouse events. Same guards, same
    // axis lock, same foldback clip, same model commit as the mouse path.
    bool SmokeDragWireBend(QtNodes::ConnectionId connId,
                           int waypointIndex,
                           const QPointF& targetScenePos);
protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    bool viewportEvent(QEvent* event) override;

    // QtNodes' showEvent recenters and re-fits the scene on EVERY show, which
    // destroys the user's pan/zoom when switching back from another tab.
    // Only allow that initial centering on the first show.
    void showEvent(QShowEvent* event) override;

private:
    void TrackNodeDragAfterPress(QMouseEvent* event);
    void QuantizeOngoingNodeDrag();
    void ClearNodeDragCursor();

    // Arrow-key move of the selected nodes: one WireRoute grid step per
    // arrow press, steps x5 with Shift. The move goes through the same
    // completion chain a real drag uses (see NudgeSelectedNodes). Returns
    // false when nothing movable is selected (caller defers to the base).
    bool NudgeSelectedNodes(const QPointF& direction, int steps);

    // Empty-canvas right-click: popup with the registered node types grouped
    // by the palette's family prefix; picking one instantiates it through
    // onNodeTypeDropped (same seam as palette drops, incl. grid snap).
    // Returns false when no type taxonomy is available.
    bool PopupAddNodeMenu(const QPoint& globalPos, const QPointF& scenePos);

    // Manual wire routing: a left press on a wire arms the gesture; dragging
    // past the platform threshold drags the bend that was grabbed (or inserts
    // a new one at the grab point). Release commits via the model's
    // onGraphChanged so the undo history sees one entry per gesture.
    bool BeginWireDrag();
    void UpdateWireDrag(const QPointF& scenePos);
    void FinishWireDrag(bool commit);

    // Net-label chip drag: a left press inside a labeled connection's tag
    // chip beats the wire stroke and arms this instead of the bend drag
    // (chip determinism, see ChipItemAt). Moves write the offset into the
    // model live (snap + 25-grid-cell clamp); release commits via
    // onGraphChanged (one undo entry); a button-loss cancel restores the
    // press-time offset.
    bool BeginChipDrag();
    void UpdateChipDrag(const QPointF& scenePos);
    void FinishChipDrag(bool commit);

    bool firstShow_ = true;
    Qt::MouseButton panMouseButton_ = Qt::MiddleButton;
    bool mousePanning_ = false;
    bool domainDragging_ = false;
    bool suppressDomainDoubleClickRelease_ = false;
    bool nodeRubberBandSelecting_ = false;
    bool nodeDragPending_ = false;
    // Left-press drag evidence: the grabbed node's qt id + scene position,
    // consumed at release by onNodeDragProven (see its header comment).
    QtNodes::NodeId nodePressGrabId_ = 0;
    bool nodePressGrabValid_ = false;
    QPointF nodePressPos_;
    bool nodeDragCursorActive_ = false;
    bool wireDragPending_ = false;
    bool wireDragActive_ = false;
    bool gridSnap_ = true;
    QtNodes::ConnectionId wireDragConnection_{};
    int wireDragHandleAtPress_ = -1;
    int wireDragWaypointIndex_ = -1;
    QPoint wireDragPressViewport_;
    QPointF wireDragPressScene_;
    QPointF wireDragAnchor_;
    bool chipDragPending_ = false;
    bool chipDragActive_ = false;
    QtNodes::ConnectionId chipDragConnection_{};
    QPoint chipDragPressViewport_;
    QPointF chipDragPressScene_;
    QPointF chipDragBaseOffset_;
    QPoint panLastPosition_;
    QPointF domainDragLastScenePosition_;
};

}  // namespace NodeGUI
