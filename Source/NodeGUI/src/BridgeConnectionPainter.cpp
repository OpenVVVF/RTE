#include "BridgeConnectionPainter.h"

#include "NodeGraphModel.h"

#include <QtNodes/Definitions>
#include <QtNodes/StyleCollection>
#include <QtNodes/internal/ConnectionGraphicsObject.hpp>
#include <QtNodes/internal/ConnectionState.hpp>

#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>

namespace NodeGUI {

namespace {

QColor BridgeColor() {
    // Distinct orange for cross-domain bridges.
    return QColor(255, 140, 0);
}

const NodeGraphModel* TryGetNodeModel(QtNodes::ConnectionGraphicsObject const& cgo) {
    auto& model = cgo.graphModel();
    return dynamic_cast<const NodeGraphModel*>(&model);
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

void BridgeConnectionPainter::drawBridgeLine(QPainter* painter,
                                             QtNodes::ConnectionGraphicsObject const& cgo) const {
    auto const& connectionStyle = QtNodes::StyleCollection::connectionStyle();

    const bool selected = cgo.isSelected();
    const bool hovered = cgo.connectionState().hovered();

    QColor color = BridgeColor();
    if (selected) {
        color = color.darker(200);
    } else if (hovered) {
        color = connectionStyle.hoveredColor();
    }

    QPen pen;
    pen.setWidthF(connectionStyle.lineWidth());
    pen.setColor(color);
    pen.setStyle(Qt::DashLine);
    pen.setCapStyle(Qt::RoundCap);

    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(cubicPath(cgo));
}

void BridgeConnectionPainter::paint(QPainter* painter,
                                    QtNodes::ConnectionGraphicsObject const& cgo) const {
    const auto* model = TryGetNodeModel(cgo);
    const bool isBridge = model && model->IsBridge(cgo.connectionId());

    if (!isBridge) {
        defaultPainter_.paint(painter, cgo);
        return;
    }

    // Draw halo/selection background using the default style.
    bool const hovered = cgo.connectionState().hovered();
    bool const selected = cgo.isSelected();
    if (hovered || selected) {
        auto const& connectionStyle = QtNodes::StyleCollection::connectionStyle();
        QPen pen;
        pen.setWidthF(2.0 * connectionStyle.lineWidth());
        pen.setColor(selected ? connectionStyle.selectedHaloColor()
                              : connectionStyle.hoveredColor());
        pen.setStyle(Qt::DashLine);

        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(cubicPath(cgo));
    }

    drawBridgeLine(painter, cgo);

    // Draw construction endpoints so the dash line still visibly terminates.
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
