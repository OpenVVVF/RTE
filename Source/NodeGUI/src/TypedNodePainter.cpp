#include "TypedNodePainter.h"

#include "NodeDataModel.h"
#include "ParameterBlock.h"
#include "PortStyle.h"

#include <QtNodes/Definitions>
#include <QtNodes/StyleCollection>
#include <QtNodes/internal/AbstractGraphModel.hpp>
#include <QtNodes/internal/AbstractNodeGeometry.hpp>
#include <QtNodes/internal/BasicGraphicsScene.hpp>
#include <QtNodes/internal/ConnectionGraphicsObject.hpp>
#include <QtNodes/internal/ConnectionIdUtils.hpp>
#include <QtNodes/internal/DataFlowGraphModel.hpp>
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QtNodes/internal/NodeState.hpp>

#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace NodeGUI {

namespace {

}  // namespace

const QtNodes::NodeStyle& TypedNodePainter::GetNodeStyle() const {
    if (!nodeStyleCache_) {
        // Identical to what DataFlowGraphModel serves for NodeRole::Style, but
        // parsed once instead of per node per frame.
        nodeStyleCache_.emplace(QtNodes::StyleCollection::nodeStyle());
    }
    return *nodeStyleCache_;
}

const PortStyle* TypedNodePainter::ResolvePortStyle(
    QtNodes::AbstractGraphModel& model,
    QtNodes::NodeId nodeId,
    QtNodes::PortType portType,
    QtNodes::PortIndex portIndex,
    const QtNodes::NodeDataType& dataType) const {
    static const PortStyle anyUnitStyle{QColor(160, 160, 160), PortShape::Circle};

    const bool anyUnitInput =
        portType == QtNodes::PortType::In
        && dataType.id.startsWith(QStringLiteral("dimensionless.scalar"));
    if (!anyUnitInput) {
        return GetPortStyle(dataType.id);
    }

    // Grey circle until connected; then adopt the producer's style.
    const auto connected = model.connections(nodeId, portType, portIndex);
    for (const auto& connectionId : connected) {
        const auto producerType =
            model
                .portData(connectionId.outNodeId,
                          QtNodes::PortType::Out,
                          connectionId.outPortIndex,
                          QtNodes::PortRole::DataType)
                .value<QtNodes::NodeDataType>();
        if (const PortStyle* style = GetPortStyle(producerType.id)) {
            return style;
        }
    }
    return &anyUnitStyle;
}

void TypedNodePainter::DrawParameterBlock(QPainter* painter,
                                          QtNodes::NodeGraphicsObject& ngo) const {
    auto* model = dynamic_cast<QtNodes::DataFlowGraphModel*>(&ngo.graphModel());
    if (!model) {
        return;
    }
    const auto* delegate = model->delegateModel<NodeInstanceModel>(ngo.nodeId());
    if (!delegate || delegate->ParameterBlock().size.isNull()) {
        return;
    }

    const QSize size = ngo.nodeScene()->nodeGeometry().size(ngo.nodeId());
    PaintParameterBlock(painter, delegate->ParameterBlock(), size);
}

const PortStyle* TypedNodePainter::GetPortStyle(const QString& typeId) const {
    auto it = styleCache_.find(typeId);
    if (it != styleCache_.end()) {
        return &it->second;
    }

    const auto style = ParsePortStyle(typeId);
    if (!style) {
        return nullptr;
    }

    const auto inserted = styleCache_.emplace(typeId, *style);
    return &inserted.first->second;
}

const QPolygonF& TypedNodePainter::GetBasePolygon(PortShape shape) const {
    auto it = shapeCache_.find(shape);
    if (it != shapeCache_.end()) {
        return it->second;
    }

    QPolygonF poly;
    switch (shape) {
        case PortShape::Square:
            poly << QPointF(-1.0, -1.0)
                 << QPointF(1.0, -1.0)
                 << QPointF(1.0, 1.0)
                 << QPointF(-1.0, 1.0);
            break;
        case PortShape::Diamond:
            poly << QPointF(0.0, -1.0)
                 << QPointF(1.0, 0.0)
                 << QPointF(0.0, 1.0)
                 << QPointF(-1.0, 0.0);
            break;
        case PortShape::Triangle:
            poly << QPointF(0.0, -1.0)
                 << QPointF(1.0, 1.0)
                 << QPointF(-1.0, 1.0);
            break;
        case PortShape::Circle:
            // Circles are rendered with drawEllipse; no polygon cache is used.
            break;
    }

    const auto inserted = shapeCache_.emplace(shape, std::move(poly));
    return inserted.first->second;
}

void TypedNodePainter::DrawPortShape(QPainter* painter,
                                     const QPointF& center,
                                     double radius,
                                     const PortStyle& style) const {
    const QPen originalPen = painter->pen();
    const QBrush originalBrush = painter->brush();

    QPen pen(style.color);
    pen.setWidthF(1.5);
    painter->setPen(pen);
    painter->setBrush(QBrush(style.color));

    if (style.shape == PortShape::Circle) {
        painter->drawEllipse(center, radius, radius);
    } else {
        const QPolygonF& base = GetBasePolygon(style.shape);
        QPolygonF scaled;
        scaled.reserve(base.size());
        for (const QPointF& pt : base) {
            scaled.append(center + pt * radius);
        }
        painter->drawPolygon(scaled);
    }

    painter->setPen(originalPen);
    painter->setBrush(originalBrush);
}

void TypedNodePainter::paint(QPainter* painter, QtNodes::NodeGraphicsObject& ngo) const {
    defaultPainter_.drawNodeRect(painter, ngo);

    DrawParameterBlock(painter, ngo);

    drawConnectionPoints(painter, ngo);
    drawFilledConnectionPoints(painter, ngo);

    defaultPainter_.drawNodeCaption(painter, ngo);
    defaultPainter_.drawEntryLabels(painter, ngo);
    defaultPainter_.drawProcessingIndicator(painter, ngo);
    defaultPainter_.drawResizeRect(painter, ngo);
    defaultPainter_.drawNodeLabel(painter, ngo);
    defaultPainter_.drawValidationIcon(painter, ngo);
    defaultPainter_.drawProgressValue(painter, ngo);
}

void TypedNodePainter::drawConnectionPoints(QPainter* painter,
                                            QtNodes::NodeGraphicsObject& ngo) const {
    auto& model = ngo.graphModel();
    const QtNodes::NodeId nodeId = ngo.nodeId();
    auto& geometry = ngo.nodeScene()->nodeGeometry();

    const QtNodes::NodeStyle& nodeStyle = GetNodeStyle();

    const float diameter = nodeStyle.ConnectionPointDiameter;
    const double reducedRadius = diameter * 0.6 / 2.0;

    for (const auto portType : {QtNodes::PortType::Out, QtNodes::PortType::In}) {
        const auto portCountRole = (portType == QtNodes::PortType::Out)
                                       ? QtNodes::NodeRole::OutPortCount
                                       : QtNodes::NodeRole::InPortCount;
        const std::size_t n = model.nodeData(nodeId, portCountRole).toUInt();

        for (QtNodes::PortIndex portIndex = 0; portIndex < static_cast<QtNodes::PortIndex>(n);
             ++portIndex) {
            const QPointF p = geometry.portPosition(nodeId, portType, portIndex);

            const auto dataType = model
                                      .portData(nodeId, portType, portIndex, QtNodes::PortRole::DataType)
                                      .value<QtNodes::NodeDataType>();

            double scale = 1.0;

            // Any-unit inputs preview the dragged wire's style at reduced
            // opacity when the connection is allowed here.
            const bool anyUnitInput =
                portType == QtNodes::PortType::In
                && dataType.id.startsWith(QStringLiteral("dimensionless.scalar"));
            const PortStyle* draftStyle = nullptr;

            const auto& state = ngo.nodeState();
            if (auto const* cgo = state.connectionForReaction()) {
                const QtNodes::PortType requiredPort = cgo->connectionState().requiredPort();
                if (requiredPort == portType) {
                    const QtNodes::ConnectionId possibleConnectionId =
                        QtNodes::makeCompleteConnectionId(cgo->connectionId(), nodeId, portIndex);

                    const bool possible = model.connectionPossible(possibleConnectionId);

                    if (possible && anyUnitInput) {
                        const auto draftId = cgo->connectionId();
                        const auto draftType =
                            model
                                .portData(draftId.outNodeId,
                                          QtNodes::PortType::Out,
                                          draftId.outPortIndex,
                                          QtNodes::PortRole::DataType)
                                .value<QtNodes::NodeDataType>();
                        draftStyle = GetPortStyle(draftType.id);
                    }

                    auto cp = cgo->sceneTransform().map(cgo->endPoint(requiredPort));
                    cp = ngo.sceneTransform().inverted().map(cp);

                    const auto diff = cp - p;
                    const double dist = std::sqrt(QPointF::dotProduct(diff, diff));

                    if (possible) {
                        constexpr double thres = 40.0;
                        scale = (dist < thres) ? (2.0 - dist / thres) : 1.0;
                    } else {
                        constexpr double thres = 80.0;
                        scale = (dist < thres) ? (dist / thres) : 1.0;
                    }
                }
            }

            const PortStyle* style =
                ResolvePortStyle(model, nodeId, portType, portIndex, dataType);
            if (style) {
                if (draftStyle) {
                    painter->save();
                    painter->setOpacity(0.5);
                    DrawPortShape(painter, p, reducedRadius * scale, *draftStyle);
                    painter->restore();
                } else {
                    DrawPortShape(painter, p, reducedRadius * scale, *style);
                }
            } else {
                painter->setPen(Qt::NoPen);
                painter->setBrush(nodeStyle.ConnectionPointColor);
                painter->drawEllipse(p, reducedRadius * scale, reducedRadius * scale);
            }
        }
    }

    if (ngo.nodeState().connectionForReaction()) {
        ngo.nodeState().resetConnectionForReaction();
    }
}

void TypedNodePainter::drawFilledConnectionPoints(QPainter* painter,
                                                  QtNodes::NodeGraphicsObject& ngo) const {
    auto& model = ngo.graphModel();
    const QtNodes::NodeId nodeId = ngo.nodeId();
    auto& geometry = ngo.nodeScene()->nodeGeometry();

    const QtNodes::NodeStyle& nodeStyle = GetNodeStyle();

    const auto diameter = nodeStyle.ConnectionPointDiameter;
    const double radius = diameter * 0.4;

    for (const auto portType : {QtNodes::PortType::Out, QtNodes::PortType::In}) {
        const auto portCountRole = (portType == QtNodes::PortType::Out)
                                       ? QtNodes::NodeRole::OutPortCount
                                       : QtNodes::NodeRole::InPortCount;
        const std::size_t n = model.nodeData(nodeId, portCountRole).toUInt();

        for (QtNodes::PortIndex portIndex = 0; portIndex < static_cast<QtNodes::PortIndex>(n);
             ++portIndex) {
            const QPointF p = geometry.portPosition(nodeId, portType, portIndex);

            const auto connected = model.connections(nodeId, portType, portIndex);
            if (!connected.empty()) {
                const auto dataType =
                    model.portData(nodeId, portType, portIndex, QtNodes::PortRole::DataType)
                        .value<QtNodes::NodeDataType>();

                const PortStyle* style =
                    ResolvePortStyle(model, nodeId, portType, portIndex, dataType);
                if (style) {
                    DrawPortShape(painter, p, radius, *style);
                } else {
                    painter->setPen(nodeStyle.FilledConnectionPointColor);
                    painter->setBrush(nodeStyle.FilledConnectionPointColor);
                    painter->drawEllipse(p, radius, radius);
                }
            }
        }
    }
}

}  // namespace NodeGUI
