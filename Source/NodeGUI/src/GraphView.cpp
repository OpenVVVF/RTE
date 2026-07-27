#include "GraphView.h"

#include "NodePalette.h"

#include <QtNodes/internal/NodeGraphicsObject.hpp>

#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QGraphicsItem>
#include <QMimeData>

namespace NodeGUI {

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
    QtNodes::GraphicsView::contextMenuEvent(event);
}

}  // namespace NodeGUI
