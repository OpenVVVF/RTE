#pragma once

#include <QtNodes/AbstractConnectionPainter>

#include <QPainterPath>
#include <QPolygonF>

namespace NodeGUI {

// Paints Connections and Bridges using the color of the source port's quantity.
// Bridges keep a dashed line style; regular Connections stay solid.
// One wire shape everywhere: connections with manual routing waypoints render
// as the rectilinear polyline through the bends; everything else renders the
// rigid orthogonal default (the mini-router's corridor when it picked one,
// else the standard stub-elbow-stub route) — no smooth cubic anywhere.
class BridgeConnectionPainter : public QtNodes::AbstractConnectionPainter {
public:
    void paint(QPainter* painter, QtNodes::ConnectionGraphicsObject const& cgo) const override;

    QPainterPath getPainterStroke(QtNodes::ConnectionGraphicsObject const& cgo) const override;

    QRectF routeBounds(QtNodes::ConnectionGraphicsObject const& cgo) const override;

private:
    // The displayed wire path: the rectilinear waypoint route when bends are
    // stored, else the mini-router polyline, else the rigid orthogonal
    // default route. Always a polyline.
    QPainterPath wirePath(QtNodes::ConnectionGraphicsObject const& connection) const;
    // Full waypoint route polyline in connection-local coordinates; empty
    // polygon when the connection routes automatically.
    QPolygonF routePolygon(QtNodes::ConnectionGraphicsObject const& connection) const;
    void drawConnectionLine(QPainter* painter,
                            QtNodes::ConnectionGraphicsObject const& cgo,
                            bool isBridge) const;
    // Small square handles at the manual bends, shown while hovered/selected.
    void drawWaypointHandles(QPainter* painter,
                             QtNodes::ConnectionGraphicsObject const& cgo,
                             const QColor& color) const;
};

}  // namespace NodeGUI
