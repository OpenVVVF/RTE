#pragma once

#include <QtNodes/Definitions>
#include <QtNodes/GraphicsView>

#include <QPointF>
#include <QString>

#include <functional>

namespace NodeGUI {

// GraphicsView that accepts node types dragged from the NodePalette. Real
// drag & drop reaches these handlers via the viewport; synthetic drops can be
// delivered straight to the view in tests.
class GraphView : public QtNodes::GraphicsView {
public:
    using QtNodes::GraphicsView::GraphicsView;

    // Called with the dragged node-type id and the drop position in scene
    // coordinates.
    std::function<void(const QString& typeId, const QPointF& scenePos)> onNodeTypeDropped;

    // Called when a node is right-clicked: global menu position + node id.
    std::function<void(const QPointF& globalPos, QtNodes::NodeId nodeId)> onNodeContextMenu;

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

    // QtNodes' showEvent recenters and re-fits the scene on EVERY show, which
    // destroys the user's pan/zoom when switching back from another tab.
    // Only allow that initial centering on the first show.
    void showEvent(QShowEvent* event) override;

private:
    bool firstShow_ = true;
};

}  // namespace NodeGUI
