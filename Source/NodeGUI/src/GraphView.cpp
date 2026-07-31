#include "GraphView.h"

#include "NodeDataModel.h"
#include "NodePalette.h"
#include "ParameterBlock.h"

#include <QtNodes/internal/AbstractNodeGeometry.hpp>
#include <QtNodes/internal/DataFlowGraphModel.hpp>
#include <QtNodes/internal/NodeGraphicsObject.hpp>

#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QHelpEvent>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QScrollBar>
#include <QToolTip>

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

}  // namespace

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

void GraphView::mousePressEvent(QMouseEvent* event) {
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
        return;
    }
    QtNodes::GraphicsView::mousePressEvent(event);
}

void GraphView::mouseMoveEvent(QMouseEvent* event) {
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
        if ((event->buttons() & Qt::LeftButton) &&
            !rubberBandRect().isNull()) {
            nodeRubberBandSelecting_ = true;
            KeepOnlyNodesSelected(scene());
        }
        return;
    }
    QtNodes::GraphicsView::mouseMoveEvent(event);
}

void GraphView::mouseReleaseEvent(QMouseEvent* event) {
    if (mousePanning_ && event->button() == panMouseButton_) {
        mousePanning_ = false;
        viewport()->unsetCursor();
        event->accept();
        return;
    }
    const bool completedNodeRubberBand =
        nodeRubberBandSelecting_ && event->button() == Qt::LeftButton;
    QtNodes::GraphicsView::mouseReleaseEvent(event);
    if (completedNodeRubberBand) {
        KeepOnlyNodesSelected(scene());
    }
    if (event->button() == Qt::LeftButton) {
        nodeRubberBandSelecting_ = false;
    }
}

void GraphView::keyReleaseEvent(QKeyEvent* event) {
    QtNodes::GraphicsView::keyReleaseEvent(event);
    if (panMouseButton_ == Qt::MiddleButton &&
        event->key() == Qt::Key_Shift) {
        // QtNodes restores ScrollHandDrag when rubber-band selection ends.
        setDragMode(QGraphicsView::RubberBandDrag);
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
        QToolTip::hideText();
        event->ignore();
        return true;
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
