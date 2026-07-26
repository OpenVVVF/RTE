#pragma once

#include "PortStyle.h"

#include <QtNodes/AbstractNodePainter>
#include <QtNodes/internal/DefaultNodePainter.hpp>

#include <QString>
#include <QPolygonF>

#include <unordered_map>

class QPainter;

namespace NodeGUI {

// Renders node ports with a shape determined by the wire frame and a color
// determined by the wire quantity, so matching types are visually obvious.
class TypedNodePainter : public QtNodes::AbstractNodePainter {
public:
    void paint(QPainter* painter, QtNodes::NodeGraphicsObject& ngo) const override;

private:
    QtNodes::DefaultNodePainter defaultPainter_;

    void drawConnectionPoints(QPainter* painter, QtNodes::NodeGraphicsObject& ngo) const;
    void drawFilledConnectionPoints(QPainter* painter, QtNodes::NodeGraphicsObject& ngo) const;

    void DrawPortShape(QPainter* painter,
                       const QPointF& center,
                       double radius,
                       const PortStyle& style) const;

    // Cache parsed styles and base (unit-radius) polygons so we do not re-parse
    // the wire type string or rebuild the shape geometry every frame.
    mutable std::unordered_map<QString, PortStyle> styleCache_;
    mutable std::unordered_map<PortShape, QPolygonF> shapeCache_;

    const PortStyle* GetPortStyle(const QString& typeId) const;
    const QPolygonF& GetBasePolygon(PortShape shape) const;
};

}  // namespace NodeGUI
