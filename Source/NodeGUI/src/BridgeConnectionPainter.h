#pragma once

#include <QtNodes/AbstractConnectionPainter>

#include <QPainterPath>

namespace NodeGUI {

// Paints Connections and Bridges using the color of the source port's quantity.
// Bridges keep a dashed line style; regular Connections stay solid.
class BridgeConnectionPainter : public QtNodes::AbstractConnectionPainter {
public:
    void paint(QPainter* painter, QtNodes::ConnectionGraphicsObject const& cgo) const override;

    QPainterPath getPainterStroke(QtNodes::ConnectionGraphicsObject const& cgo) const override;

private:
    QPainterPath cubicPath(QtNodes::ConnectionGraphicsObject const& connection) const;
    void drawConnectionLine(QPainter* painter,
                            QtNodes::ConnectionGraphicsObject const& cgo,
                            bool isBridge) const;
};

}  // namespace NodeGUI
