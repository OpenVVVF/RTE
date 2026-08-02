#pragma once

#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QStaticText>
#include <QString>

#include <map>
#include <optional>
#include <string>
#include <vector>

class QPainter;

namespace NodeGUI {

using ParameterMap = std::map<std::string, std::string>;

// Pre-laid-out parameter panel content. Building the row strings and
// measuring them is done once per parameter change (PrepareParameterBlock),
// so the per-frame paint is a few cheap draw calls.
struct ParameterBlockRow {
    std::string name;
    QStaticText text;
};

struct ParameterBlockData {
    std::vector<ParameterBlockRow> rows;
    QSizeF size;  // Natural panel size, margins included; null when no rows.
};

// Builds the painted content for a node's parameter map.
ParameterBlockData PrepareParameterBlock(const ParameterMap& parameters);

// Rectangle the panel occupies inside a node of the given size. The panel is
// anchored to the bottom of the node with the standard margins; its width is
// the natural text width clamped to the available space.
QRectF ParameterBlockRect(const ParameterBlockData& block, const QSize& nodeSize);

// Returns the property name under a point in node-local coordinates.
std::optional<std::string> ParameterAtPosition(const ParameterBlockData& block,
                                               const QSize& nodeSize,
                                               const QPointF& position);

// Margins between the panel and the node edges. Exposed so the geometry can
// reserve exactly the space the painter uses.
constexpr double kParameterBlockMarginX = 10.0;
constexpr double kParameterBlockMarginBottom = 6.0;

// Draws the panel (tinted rounded background + amber monospace rows) inside
// the given node size. Cheap: a handful of QPainter calls, no widgets.
void PaintParameterBlock(QPainter* painter,
                         const ParameterBlockData& block,
                         const QSize& nodeSize);

}  // namespace NodeGUI
