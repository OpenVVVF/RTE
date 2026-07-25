#include "BridgeConnectionPainter.h"

#include "NodeGraphModel.h"
#include "PortStyle.h"

#include <QtNodes/Definitions>
#include <QtNodes/StyleCollection>
#include <QtNodes/internal/ConnectionGraphicsObject.hpp>
#include <QtNodes/internal/ConnectionState.hpp>

#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>

namespace NodeGUI {

namespace {

const NodeGraphModel* TryGetNodeModel(QtNodes::ConnectionGraphicsObject const& cgo) {
    auto& model = cgo.graphModel();
    return dynamic_cast<const NodeGraphModel*>(&model);
}

QColor ConnectionColor(QtNodes::ConnectionGraphicsObject const& cgo) {
    auto& model = cgo.graphModel();
    const auto cId = cgo.connectionId();

    const auto dataType = model
                              .portData(cId.outNodeId,
                                        QtNodes::PortType::Out,
                                        cId.outPortIndex,
                                        QtNodes::PortRole::DataType)
                              .value<QtNodes::NodeDataType>();

    if (const auto color = ParsePortColor(dataType.id)) {
        return *color;
    }

    return QtNodes::StyleCollection::connectionStyle().normalColor();
}

}  // namespace

QPainterPath BridgeConnectionPainter::cubicPath(QtNodes::ConnectionGraphicsObject const& connection) const {
    QPointF const in = connection.endPoint(QtNodes::PortType::In);
    QPointF const out = connection.endPoint(QtNodes::PortType::Out);

    auto const c1c2 = connection.pointsC1C2();

    QPainterPath cubic(out);
    cubic.cubicTo(c1c2.first, c1c2.second, in);
    return cubic;
}

void BridgeConnectionPainter::drawConnectionLine(QPainter* painter,
                                                 QtNodes::ConnectionGraphicsObject const& cgo,
                                                 bool isBridge) const {
    auto const& connectionStyle = QtNodes::StyleCollection::connectionStyle();

    const bool selected = cgo.isSelected();
    const bool hovered = cgo.connectionState().hovered();

    QColor color = ConnectionColor(cgo);
    if (selected) {
        color = color.darker(200);
    } else if (hovered) {
        color = connectionStyle.hoveredColor();
    }

    QPen pen;
    pen.setWidthF(connectionStyle.lineWidth());
    pen.setColor(color);
    pen.setStyle(isBridge ? Qt::DashLine : Qt::SolidLine);
    pen.setCapStyle(Qt::RoundCap);

    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(cubicPath(cgo));
}

void BridgeConnectionPainter::paint(QPainter* painter,
                                    QtNodes::ConnectionGraphicsObject const& cgo) const {
    const auto* model = TryGetNodeModel(cgo);
    const bool isBridge = model && model->IsBridge(cgo.connectionId());

    // Draw halo/selection background.
    bool const hovered = cgo.connectionState().hovered();
    bool const selected = cgo.isSelected();
    if (hovered || selected) {
        auto const& connectionStyle = QtNodes::StyleCollection::connectionStyle();
        QPen pen;
        pen.setWidthF(2.0 * connectionStyle.lineWidth());
        pen.setColor(selected ? connectionStyle.selectedHaloColor()
                              : connectionStyle.hoveredColor());
        pen.setStyle(isBridge ? Qt::DashLine : Qt::SolidLine);

        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(cubicPath(cgo));
    }

    drawConnectionLine(painter, cgo, isBridge);

    // Draw construction endpoints.
    auto const& connectionStyle = QtNodes::StyleCollection::connectionStyle();
    double const pointDiameter = connectionStyle.pointDiameter();
    painter->setPen(connectionStyle.constructionColor());
    painter->setBrush(connectionStyle.constructionColor());
    double const pointRadius = pointDiameter / 2.0;
    painter->drawEllipse(cgo.out(), pointRadius, pointRadius);
    painter->drawEllipse(cgo.in(), pointRadius, pointRadius);
}

QPainterPath BridgeConnectionPainter::getPainterStroke(
    QtNodes::ConnectionGraphicsObject const& cgo) const {
    auto cubic = cubicPath(cgo);

    QPointF const out = cgo.endPoint(QtNodes::PortType::Out);
    QPainterPath result(out);

    unsigned int constexpr segments = 20;
    for (auto i = 0ul; i < segments; ++i) {
        double ratio = double(i + 1) / segments;
        result.lineTo(cubic.pointAtPercent(ratio));
    }

    QPainterPathStroker stroker;
    stroker.setWidth(10.0);

    return stroker.createStroke(result);
}

}  // namespace NodeGUI
