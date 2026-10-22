#pragma once

#include <QtNodes/AbstractConnectionPainter>

#include <QPainterPath>
#include <QPolygonF>

#include <optional>

namespace NodeGUI {

// Paints Connections and Bridges using the color of the source port's quantity.
// Bridges keep a dashed line style; regular Connections stay solid.
// One wire shape everywhere: connections with manual routing waypoints render
// as the rectilinear polyline through the bends; everything else renders the
// rigid orthogonal default (the mini-router's corridor when it picked one,
// else the standard stub-elbow-stub route) — no smooth cubic anywhere.
// Exception: connections carrying a display-only net label render as two stub
// + tag-chip ends (KiCad-style) instead of the long route.
class BridgeConnectionPainter : public QtNodes::AbstractConnectionPainter {
public:
    // Net-label tag geometry (KiCad-style): a labeled connection collapses
    // the long wire into a short exit stub per port, each terminated by a
    // tag chip carrying the label text. Connection-local coordinates.
    struct NetLabelLayout {
        QPointF outTip, inTip;   // stub free ends
        QRectF outChip, inChip;  // tag chip rects
    };

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

    // Returns no layout when the connection has no net label (the renderer
    // then draws the regular long-wire route).
    std::optional<NetLabelLayout> NetLabelGeometry(
        QtNodes::ConnectionGraphicsObject const& cgo) const;
    // The whole labeled hit silhouette: both stubs stroked + both chips
    // filled, in connection-local coordinates.
    QPainterPath NetLabelStroke(QtNodes::ConnectionGraphicsObject const& cgo,
                                const NetLabelLayout& layout) const;
    // Stubs + tag chips with the usual wire color treatment (selection /
    // hover / excluded / bridge dash).
    void drawNetLabelTags(QPainter* painter,
                          QtNodes::ConnectionGraphicsObject const& cgo,
                          const NetLabelLayout& layout,
                          bool isBridge) const;
};

}  // namespace NodeGUI
