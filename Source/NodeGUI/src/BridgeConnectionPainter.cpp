#include "BridgeConnectionPainter.h"

#include "NodeGraphModel.h"
#include "PortStyle.h"
#include "WireRoute.h"

#include <QtNodes/Definitions>
#include <QtNodes/StyleCollection>
#include <QtNodes/internal/ConnectionGraphicsObject.hpp>
#include <QtNodes/internal/ConnectionState.hpp>

#include <QApplication>
#include <QPalette>
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

// Wire selection follows the active app theme (QPalette::Highlight), falling
// back to the connection style when no application palette exists.
QColor WireSelectionColor() {
    if (qApp) {
        const QColor highlight = qApp->palette().color(QPalette::Highlight);
        if (highlight.isValid()) {
            return highlight;
        }
    }
    return QtNodes::StyleCollection::connectionStyle().selectedHaloColor();
}

// Draws a port-shaped marker (same shapes as the node ports).
void DrawStyleShape(QPainter* painter,
                    const QPointF& center,
                    double radius,
                    const PortStyle& style) {
    QPen pen(style.color);
    pen.setWidthF(1.5);
    painter->setPen(pen);
    painter->setBrush(QBrush(style.color));

    QPolygonF poly;
    switch (style.shape) {
        case PortShape::Circle:
            painter->drawEllipse(center, radius, radius);
            return;
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
    }

    QPolygonF scaled;
    scaled.reserve(poly.size());
    for (const QPointF& pt : poly) {
        scaled.append(center + pt * radius);
    }
    painter->drawPolygon(scaled);
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

QPolygonF BridgeConnectionPainter::routePolygon(QtNodes::ConnectionGraphicsObject const& connection) const {
    const auto* model = TryGetNodeModel(connection);
    if (!model) {
        return {};
    }
    const std::vector<QPointF>* waypoints = model->ConnectionWaypoints(connection.connectionId());
    if (!waypoints || waypoints->empty()) {
        return {};
    }

    const QTransform toLocal = connection.sceneTransform().inverted();
    std::vector<QPointF> local;
    local.reserve(waypoints->size());
    for (const QPointF& p : *waypoints) {
        local.push_back(toLocal.map(p));
    }

    return WireRoute::BuildRoute(connection.endPoint(QtNodes::PortType::Out),
                                 connection.endPoint(QtNodes::PortType::In),
                                 local);
}

QPainterPath BridgeConnectionPainter::wirePath(QtNodes::ConnectionGraphicsObject const& connection) const {
    const QPolygonF routed = routePolygon(connection);
    QPolygonF source = routed;
    if (source.isEmpty()) {
        // Waypoint-free: honor the mini-router's polyline when it displaced
        // the legacy cubic; legacy keeps today's smooth bezier.
        if (const auto* model = TryGetNodeModel(connection)) {
            if (const auto* choice = model->AutoRoute(connection.connectionId());
                choice && !choice->legacy && !choice->line.isEmpty()) {
                const QTransform toLocal = connection.sceneTransform().inverted();
                QPolygonF local;
                local.reserve(choice->line.size());
                for (const QPointF& p : choice->line) {
                    local << toLocal.map(p);
                }
                source = local;
            }
        }
    }
    if (source.isEmpty()) {
        return cubicPath(connection);
    }

    QPainterPath path(source.constFirst());
    for (int i = 1; i < source.size(); ++i) {
        path.lineTo(source[i]);
    }
    return path;
}

void BridgeConnectionPainter::drawConnectionLine(QPainter* painter,
                                                 QtNodes::ConnectionGraphicsObject const& cgo,
                                                 bool isBridge) const {
    auto const& connectionStyle = QtNodes::StyleCollection::connectionStyle();

    const bool selected = cgo.isSelected();
    const bool hovered = cgo.connectionState().hovered();

    const auto* model = TryGetNodeModel(cgo);
    const bool excluded = model && model->IsExcluded(cgo.connectionId());

    QColor color = excluded ? QColor(110, 112, 118) : ConnectionColor(cgo);
    if (selected) {
        color = WireSelectionColor();
    } else if (hovered) {
        color = connectionStyle.hoveredColor();
    }

    QPen pen;
    pen.setWidthF(connectionStyle.lineWidth() + (selected ? 0.5 : 0.0));
    pen.setColor(color);
    pen.setStyle(isBridge ? Qt::DashLine : Qt::SolidLine);
    pen.setCapStyle(Qt::RoundCap);

    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(wirePath(cgo));
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
        QColor halo = selected ? WireSelectionColor() : connectionStyle.hoveredColor();
        if (selected) {
            halo.setAlpha(110);
        }
        pen.setColor(halo);
        pen.setStyle(isBridge ? Qt::DashLine : Qt::SolidLine);

        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(wirePath(cgo));
    }

    drawConnectionLine(painter, cgo, isBridge);

    // Draw construction endpoints. While drafting, the loose end tracks the
    // cursor and gets the dragged wire's style at reduced opacity (matching
    // the port preview); the attached end keeps the plain dot.
    auto const& connectionStyle = QtNodes::StyleCollection::connectionStyle();
    double const pointRadius = connectionStyle.pointDiameter() / 2.0;

    const auto& state = cgo.connectionState();
    const bool drafting = state.requiresPort() || state.frozen();
    if (drafting) {
        const QtNodes::PortType required = state.requiredPort();
        const auto cId = cgo.connectionId();

        const QtNodes::NodeDataType wireType =
            (required == QtNodes::PortType::In)
                ? cgo.graphModel()
                      .portData(cId.outNodeId,
                                QtNodes::PortType::Out,
                                cId.outPortIndex,
                                QtNodes::PortRole::DataType)
                      .value<QtNodes::NodeDataType>()
                : cgo.graphModel()
                      .portData(cId.inNodeId,
                                QtNodes::PortType::In,
                                cId.inPortIndex,
                                QtNodes::PortRole::DataType)
                      .value<QtNodes::NodeDataType>();

        const QPointF attached = (required == QtNodes::PortType::In) ? cgo.out() : cgo.in();
        const QPointF tracking = (required == QtNodes::PortType::In) ? cgo.in() : cgo.out();

        painter->setPen(connectionStyle.constructionColor());
        painter->setBrush(connectionStyle.constructionColor());
        painter->drawEllipse(attached, pointRadius, pointRadius);

        if (const auto style = ParsePortStyle(wireType.id)) {
            painter->save();
            painter->setOpacity(0.5);
            DrawStyleShape(painter, tracking, pointRadius * 1.4, *style);
            painter->restore();
        } else {
            painter->drawEllipse(tracking, pointRadius, pointRadius);
        }
        return;
    }

    // Attached-endpoint dots, greyed like the line when excluded.
    const bool excludedEndpoints = model && model->IsExcluded(cgo.connectionId());
    const QColor dotColor = excludedEndpoints ? QColor(110, 112, 118)
                                              : connectionStyle.constructionColor();
    painter->setPen(dotColor);
    painter->setBrush(dotColor);
    painter->drawEllipse(cgo.out(), pointRadius, pointRadius);
    painter->drawEllipse(cgo.in(), pointRadius, pointRadius);

    if ((hovered || selected) && model) {
        QColor handleColor = ConnectionColor(cgo);
        if (excludedEndpoints) {
            handleColor = QColor(110, 112, 118);
        } else if (selected) {
            handleColor = WireSelectionColor();
        }
        drawWaypointHandles(painter, cgo, handleColor);
    }
}

void BridgeConnectionPainter::drawWaypointHandles(QPainter* painter,
                                                  QtNodes::ConnectionGraphicsObject const& cgo,
                                                  const QColor& color) const {
    const auto* model = TryGetNodeModel(cgo);
    if (!model) {
        return;
    }
    const std::vector<QPointF>* waypoints = model->ConnectionWaypoints(cgo.connectionId());
    if (!waypoints || waypoints->empty()) {
        return;
    }

    constexpr double kHandleHalf = 4.0;
    const QTransform toLocal = cgo.sceneTransform().inverted();

    painter->save();
    QPen pen(color.darker(170));
    pen.setWidthF(1.0);
    painter->setPen(pen);
    painter->setBrush(QBrush(color));
    for (const QPointF& p : *waypoints) {
        const QPointF local = toLocal.map(p);
        painter->drawRect(QRectF(local.x() - kHandleHalf,
                                 local.y() - kHandleHalf,
                                 2.0 * kHandleHalf,
                                 2.0 * kHandleHalf));
    }
    painter->restore();
}

QPainterPath BridgeConnectionPainter::getPainterStroke(
    QtNodes::ConnectionGraphicsObject const& cgo) const {
    if (!routePolygon(cgo).isEmpty()) {
        QPainterPathStroker stroker;
        stroker.setWidth(10.0);
        return stroker.createStroke(wirePath(cgo));
    }
    if (const auto* model = TryGetNodeModel(cgo)) {
        if (const auto* choice = model->AutoRoute(cgo.connectionId());
            choice && !choice->legacy && !choice->line.isEmpty()) {
            QPainterPathStroker stroker;
            stroker.setWidth(10.0);
            return stroker.createStroke(wirePath(cgo));
        }
    }

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

QRectF BridgeConnectionPainter::routeBounds(QtNodes::ConnectionGraphicsObject const& cgo) const {
    const auto* model = TryGetNodeModel(cgo);
    if (!model) {
        return {};
    }
    const std::vector<QPointF>* waypoints = model->ConnectionWaypoints(cgo.connectionId());
    if (waypoints && !waypoints->empty()) {
        const QTransform toLocal = cgo.sceneTransform().inverted();
        QPolygonF local;
        local.reserve(static_cast<int>(waypoints->size()));
        for (const QPointF& p : *waypoints) {
            local << toLocal.map(p);
        }
        // Slack covers the waypoint handles and the pen width.
        return local.boundingRect().adjusted(-8.0, -8.0, 8.0, 8.0);
    }

    if (const auto* choice = model->AutoRoute(cgo.connectionId());
        choice && !choice->legacy && !choice->line.isEmpty()) {
        const QTransform toLocal = cgo.sceneTransform().inverted();
        QPolygonF local;
        local.reserve(choice->line.size());
        for (const QPointF& p : choice->line) {
            local << toLocal.map(p);
        }
        return local.boundingRect().adjusted(-4.0, -4.0, 4.0, 4.0);
    }
    return {};
}

}  // namespace NodeGUI
