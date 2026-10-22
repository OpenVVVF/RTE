#include "BridgeConnectionPainter.h"

#include "NodeGraphModel.h"
#include "PortStyle.h"
#include "WireRoute.h"

#include <QtNodes/Definitions>
#include <QtNodes/StyleCollection>
#include <QtNodes/internal/ConnectionGraphicsObject.hpp>
#include <QtNodes/internal/ConnectionState.hpp>

#include <QApplication>
#include <QFont>
#include <QFontMetricsF>
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

// Net-label tag geometry: 3-grid-cell exit stub from each port toward the
// peer, terminated by a small rounded tag chip carrying the label text.
namespace NetLabel {

constexpr double kStubLength = 3.0 * WireRoute::kGridPitch;  // 45 px
constexpr double kChipPadX = 6.0;
constexpr double kChipPadY = 3.0;
constexpr double kChipGap = 2.0;    // stub tip -> chip distance
constexpr double kChipRadius = 4.0;

// The two open stub segments of a labeled connection (halo / hit / paint).
QPainterPath StubPath(QtNodes::ConnectionGraphicsObject const& cgo,
                      const BridgeConnectionPainter::NetLabelLayout& layout) {
    QPainterPath path(cgo.endPoint(QtNodes::PortType::Out));
    path.lineTo(layout.outTip);
    path.moveTo(cgo.endPoint(QtNodes::PortType::In));
    path.lineTo(layout.inTip);
    return path;
}

// Shared selection/hover/excluded treatment color for the whole tag layer
// (stubs, chip borders, label text) — same rules as the wire line itself.
QColor TreatmentColor(QtNodes::ConnectionGraphicsObject const& cgo,
                      const NodeGraphModel* model) {
    const bool selected = cgo.isSelected();
    const bool hovered = cgo.connectionState().hovered();
    const bool excluded = model && model->IsExcluded(cgo.connectionId());
    QColor color = excluded ? QColor(110, 112, 118) : ConnectionColor(cgo);
    if (selected) {
        color = WireSelectionColor();
    } else if (hovered) {
        color = QtNodes::StyleCollection::connectionStyle().hoveredColor();
    }
    return color;
}

}  // namespace NetLabel

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
    QPolygonF source = routePolygon(connection);
    if (source.isEmpty()) {
        // Waypoint-free: honor the mini-router's polyline when it displaced
        // the default corridor.
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
        // One wire behavior: the un-routed default is the same rigid
        // orthogonal route the drag machinery produces (horizontal port
        // stubs, single elbow per leg). Covers pending drafts too - a draft
        // has neither waypoints nor a router entry, so it lands here and
        // tracks the cursor as an orthogonal wire.
        source = WireRoute::BuildRoute(connection.endPoint(QtNodes::PortType::Out),
                                       connection.endPoint(QtNodes::PortType::In),
                                       {});
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
    const auto* model = TryGetNodeModel(cgo);
    const QColor color = NetLabel::TreatmentColor(cgo, model);

    QPen pen;
    pen.setWidthF(connectionStyle.lineWidth() + (selected ? 0.5 : 0.0));
    pen.setColor(color);
    pen.setStyle(isBridge ? Qt::DashLine : Qt::SolidLine);
    pen.setCapStyle(Qt::RoundCap);

    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(wirePath(cgo));
}

std::optional<BridgeConnectionPainter::NetLabelLayout>
BridgeConnectionPainter::ComputeNetLabelLayout(const QPointF& out,
                                               const QPointF& in,
                                               const QString& label,
                                               const QPointF& offset) {
    if (label.isEmpty()) {
        return std::nullopt;
    }

    // Horizontal stub direction toward the peer; on exact vertical stacks the
    // out stub defaults rightward and the in stub leftward (off the node
    // bodies).
    const double outDir = in.x() >= out.x() ? 1.0 : -1.0;
    const double inDir = out.x() <= in.x() ? -1.0 : 1.0;

    NetLabelLayout layout;
    layout.outTip = QPointF(out.x() + NetLabel::kStubLength * outDir, out.y());
    layout.inTip = QPointF(in.x() + NetLabel::kStubLength * inDir, in.y());

    const QFontMetricsF metrics(qApp ? qApp->font() : QFont{});
    const double chipWidth =
        metrics.horizontalAdvance(label) + 2.0 * NetLabel::kChipPadX;
    const double chipHeight = metrics.height() + 2.0 * NetLabel::kChipPadY;

    auto chipAt = [&](const QPointF& tip, double stubY, double dir) {
        const double x = (dir > 0.0) ? tip.x() + NetLabel::kChipGap
                                     : tip.x() - NetLabel::kChipGap - chipWidth;
        return QRectF(x, stubY - chipHeight / 2.0, chipWidth, chipHeight);
    };
    // The user's displacement translates both chips of the pair; an on-anchor
    // chip pair (null offset) sits exactly at the computed stubs.
    layout.outChip = chipAt(layout.outTip, out.y(), outDir).translated(offset);
    layout.inChip = chipAt(layout.inTip, in.y(), inDir).translated(offset);
    return layout;
}

std::optional<BridgeConnectionPainter::NetLabelLayout>
BridgeConnectionPainter::NetLabelGeometry(
    QtNodes::ConnectionGraphicsObject const& cgo) const {
    const auto* model = TryGetNodeModel(cgo);
    if (!model) {
        return std::nullopt;
    }
    const QString* label = model->ConnectionLabel(cgo.connectionId());
    if (!label || label->isEmpty()) {
        return std::nullopt;
    }
    const QPointF offset = model->ConnectionLabelOffset(cgo.connectionId())
                               ? *model->ConnectionLabelOffset(cgo.connectionId())
                               : QPointF();
    return ComputeNetLabelLayout(cgo.endPoint(QtNodes::PortType::Out),
                                 cgo.endPoint(QtNodes::PortType::In),
                                 *label,
                                 offset);
}

QPainterPath BridgeConnectionPainter::NetLabelStroke(
    QtNodes::ConnectionGraphicsObject const& cgo,
    const NetLabelLayout& layout) const {
    QPainterPathStroker stroker;
    stroker.setWidth(10.0);
    QPainterPath stroke = stroker.createStroke(NetLabel::StubPath(cgo, layout));
    stroke.addRoundedRect(layout.outChip, NetLabel::kChipRadius, NetLabel::kChipRadius);
    stroke.addRoundedRect(layout.inChip, NetLabel::kChipRadius, NetLabel::kChipRadius);
    return stroke;
}

void BridgeConnectionPainter::drawNetLabelTags(
    QPainter* painter,
    QtNodes::ConnectionGraphicsObject const& cgo,
    const NetLabelLayout& layout,
    bool isBridge) const {
    auto const& connectionStyle = QtNodes::StyleCollection::connectionStyle();
    const auto* model = TryGetNodeModel(cgo);
    const bool selected = cgo.isSelected();
    const QColor color = NetLabel::TreatmentColor(cgo, model);

    QPen pen;
    pen.setWidthF(connectionStyle.lineWidth() + (selected ? 0.5 : 0.0));
    pen.setColor(color);
    pen.setStyle(isBridge ? Qt::DashLine : Qt::SolidLine);
    pen.setCapStyle(Qt::RoundCap);

    painter->setPen(pen);
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(NetLabel::StubPath(cgo, layout));

    // Tag chips: canvas-Base fill, wire-colored border (dashed like the stub
    // on bridges), label text in the treatment color.
    QPen chipPen;
    chipPen.setColor(color);
    chipPen.setStyle(isBridge ? Qt::DashLine : Qt::SolidLine);
    painter->setPen(chipPen);
    painter->setBrush(QBrush(qApp ? qApp->palette().color(QPalette::Base)
                                  : QColor(255, 255, 255)));
    painter->drawRoundedRect(layout.outChip, NetLabel::kChipRadius, NetLabel::kChipRadius);
    painter->drawRoundedRect(layout.inChip, NetLabel::kChipRadius, NetLabel::kChipRadius);

    if (const QString* label = model->ConnectionLabel(cgo.connectionId())) {
        QPen textPen;
        textPen.setColor(color);
        painter->setPen(textPen);
        painter->drawText(layout.outChip, Qt::AlignCenter, *label);
        painter->drawText(layout.inChip, Qt::AlignCenter, *label);
    }
}

void BridgeConnectionPainter::paint(QPainter* painter,
                                    QtNodes::ConnectionGraphicsObject const& cgo) const {
    const auto* model = TryGetNodeModel(cgo);
    const bool isBridge = model && model->IsBridge(cgo.connectionId());
    const auto labelLayout = NetLabelGeometry(cgo);

    // Draw halo/selection background. A labeled connection has no long wire:
    // the halo wraps the two stubs and their tag chips instead.
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
        if (labelLayout) {
            painter->drawPath(NetLabel::StubPath(cgo, *labelLayout));
            painter->drawRoundedRect(labelLayout->outChip,
                                     NetLabel::kChipRadius, NetLabel::kChipRadius);
            painter->drawRoundedRect(labelLayout->inChip,
                                     NetLabel::kChipRadius, NetLabel::kChipRadius);
        } else {
            painter->drawPath(wirePath(cgo));
        }
    }

    if (labelLayout) {
        drawNetLabelTags(painter, cgo, *labelLayout, isBridge);
    } else {
        drawConnectionLine(painter, cgo, isBridge);
    }

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
        if (!labelLayout) {
            // Labeled wires ignore their stored bends at render time; the
            // handles go with the bends (data kept for the un-collapse).
            drawWaypointHandles(painter, cgo, handleColor);
        }
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
    // The hit shape follows the drawn wire exactly (every route wirePath
    // emits is already a polyline). A labeled connection's hit shape covers
    // the stubs and both tag chips — the wire itself is not drawn.
    if (const auto labelLayout = NetLabelGeometry(cgo)) {
        return NetLabelStroke(cgo, *labelLayout);
    }
    QPainterPathStroker stroker;
    stroker.setWidth(10.0);
    return stroker.createStroke(wirePath(cgo));
}

QRectF BridgeConnectionPainter::routeBounds(QtNodes::ConnectionGraphicsObject const& cgo) const {
    const auto* model = TryGetNodeModel(cgo);
    if (!model) {
        return {};
    }
    // Labeled: stubs + tag chips stick out past the endpoint rect.
    if (const auto labelLayout = NetLabelGeometry(cgo)) {
        QRectF bounds = labelLayout->outChip.united(labelLayout->inChip);
        bounds |= QRectF(cgo.endPoint(QtNodes::PortType::Out), labelLayout->outTip);
        bounds |= QRectF(cgo.endPoint(QtNodes::PortType::In), labelLayout->inTip);
        return bounds.normalized().adjusted(-8.0, -8.0, 8.0, 8.0);
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
    // The default orthogonal route stays inside the endpoint rect, which the
    // base item bounding rect already covers.
    return {};
}

}  // namespace NodeGUI
