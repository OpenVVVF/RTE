#pragma once

#include <NodeAPI/WireType.h>

#include <QColor>

#include <optional>
#include <string_view>

namespace NodeGUI {

enum class PortShape {
    Circle,
    Square,
    Diamond,
    Triangle,
};

struct PortStyle {
    QColor color;
    PortShape shape;
};

QColor QuantityColor(NodeAPI::Quantity quantity);
PortShape FrameShape(NodeAPI::Frame frame);

// Parses a NodeDataType id of the form "quantity.frame.dtype" (produced by
// NodeDataModel) into a color + shape.
std::optional<PortStyle> ParsePortStyle(const QString& typeId);

// Convenience: just the color for connection rendering.
std::optional<QColor> ParsePortColor(const QString& typeId);

}  // namespace NodeGUI
