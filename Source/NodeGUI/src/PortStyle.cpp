#include "PortStyle.h"

#include <QString>

#include <string>
#include <string_view>
#include <unordered_map>

namespace NodeGUI {

QColor QuantityColor(NodeAPI::Quantity quantity) {
    switch (quantity) {
        case NodeAPI::Quantity::Voltage:
            return QColor(255, 215, 0);    // gold
        case NodeAPI::Quantity::Current:
            return QColor(30, 144, 255);   // dodger blue
        case NodeAPI::Quantity::AngularVelocity:
            return QColor(50, 205, 50);    // lime green
        case NodeAPI::Quantity::Torque:
            return QColor(220, 20, 60);    // crimson
        case NodeAPI::Quantity::Temperature:
            return QColor(255, 140, 0);    // dark orange
        case NodeAPI::Quantity::Dimensionless:
            return QColor(147, 112, 219);  // medium purple
        case NodeAPI::Quantity::Boolean:
            return QColor(192, 192, 192);  // silver
        case NodeAPI::Quantity::String:
            return QColor(255, 105, 180);  // hot pink
    }
    return QColor(200, 200, 200);
}

PortShape FrameShape(NodeAPI::Frame frame) {
    switch (frame) {
        case NodeAPI::Frame::Scalar:
            return PortShape::Diamond;
        case NodeAPI::Frame::Abc:
            return PortShape::Circle;
        case NodeAPI::Frame::AlphaBeta:
            return PortShape::Square;
        case NodeAPI::Frame::Dq:
            return PortShape::Triangle;
    }
    return PortShape::Circle;
}

namespace {

std::optional<PortStyle> ParsePortStyleImpl(const QString& typeId) {
    const std::string id = typeId.toStdString();
    const std::string_view view(id);

    const std::size_t firstDot = view.find('.');
    if (firstDot == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t secondDot = view.find('.', firstDot + 1);
    if (secondDot == std::string_view::npos) {
        return std::nullopt;
    }

    const std::string_view quantityStr = view.substr(0, firstDot);
    const std::string_view frameStr = view.substr(firstDot + 1, secondDot - firstDot - 1);

    try {
        const NodeAPI::Quantity quantity = NodeAPI::QuantityFromString(quantityStr);
        const NodeAPI::Frame frame = NodeAPI::FrameFromString(frameStr);
        return PortStyle{QuantityColor(quantity), FrameShape(frame)};
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace

std::optional<PortStyle> ParsePortStyle(const QString& typeId) {
    // The set of wire types in a graph is small and fixed, so cache the parsed
    // style to avoid repeated string splitting and enum lookups on every paint.
    static std::unordered_map<QString, std::optional<PortStyle>> cache;

    auto it = cache.find(typeId);
    if (it != cache.end()) {
        return it->second;
    }

    const auto result = ParsePortStyleImpl(typeId);
    cache.emplace(typeId, result);
    return result;
}

std::optional<QColor> ParsePortColor(const QString& typeId) {
    const auto style = ParsePortStyle(typeId);
    if (style) {
        return style->color;
    }
    return std::nullopt;
}

}  // namespace NodeGUI
