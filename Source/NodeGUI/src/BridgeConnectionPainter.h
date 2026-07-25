#pragma once

#include <QtNodes/AbstractConnectionPainter>
#include <QtNodes/internal/DefaultConnectionPainter.hpp>

namespace NodeGUI {

// Paints NodeAPI Bridges as dashed, colored lines while leaving regular
// Connections rendered by QtNodes' default painter.
class BridgeConnectionPainter : public QtNodes::AbstractConnectionPainter {
public:
    void paint(QPainter* painter, QtNodes::ConnectionGraphicsObject const& cgo) const override;

    QPainterPath getPainterStroke(QtNodes::ConnectionGraphicsObject const& cgo) const override;

private:
    QtNodes::DefaultConnectionPainter defaultPainter_;

    QPainterPath cubicPath(QtNodes::ConnectionGraphicsObject const& connection) const;
    void drawBridgeLine(QPainter* painter, QtNodes::ConnectionGraphicsObject const& cgo) const;
};

}  // namespace NodeGUI
