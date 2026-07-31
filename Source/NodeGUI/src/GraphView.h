#pragma once

#include <QtNodes/Definitions>
#include <QtNodes/GraphicsView>

#include <QPointF>
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

    // Called with the dragged node-type id and the drop position in scene
    // coordinates.
    std::function<void(const QString& typeId, const QPointF& scenePos)> onNodeTypeDropped;

    // Called when a node is right-clicked: global menu position + node id.
    std::function<void(const QPointF& globalPos, QtNodes::NodeId nodeId)> onNodeContextMenu;

    std::function<bool(const QPointF& scenePos)> onDomainDoubleClicked;
    std::function<bool(const QPointF& scenePos)> onDomainDragStarted;
    std::function<void(const QPointF& delta)> onDomainDragged;
    std::function<void()> onDomainDragFinished;

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    bool viewportEvent(QEvent* event) override;

    // QtNodes' showEvent recenters and re-fits the scene on EVERY show, which
    // destroys the user's pan/zoom when switching back from another tab.
    // Only allow that initial centering on the first show.
    void showEvent(QShowEvent* event) override;

private:
    void TrackNodeDragAfterPress(QMouseEvent* event);
    void ClearNodeDragCursor();

    bool firstShow_ = true;
    Qt::MouseButton panMouseButton_ = Qt::MiddleButton;
    bool mousePanning_ = false;
    bool domainDragging_ = false;
    bool suppressDomainDoubleClickRelease_ = false;
    bool nodeRubberBandSelecting_ = false;
    bool nodeDragPending_ = false;
    bool nodeDragCursorActive_ = false;
    QPoint panLastPosition_;
    QPointF domainDragLastScenePosition_;
};

}  // namespace NodeGUI
