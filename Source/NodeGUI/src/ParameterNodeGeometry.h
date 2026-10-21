#pragma once

#include <QtNodes/internal/DefaultHorizontalNodeGeometry.hpp>

namespace NodeGUI {

// Horizontal node geometry that additionally reserves room at the bottom of
// the node for the painted parameter block (see ParameterBlock.h). Replaces
// the default geometry so nodes size themselves without an embedded widget.
// All vertical metrics (header band, port-row pitch, body height) and the
// node width are rounded UP to WireRoute::kGridPitch so port sockets land on
// grid coordinates whenever the node origin is snapped.
class ParameterNodeGeometry : public QtNodes::DefaultHorizontalNodeGeometry {
public:
    explicit ParameterNodeGeometry(QtNodes::AbstractGraphModel& graphModel);

    void recomputeSize(QtNodes::NodeId nodeId) const override;

    QPointF portPosition(QtNodes::NodeId nodeId,
                         QtNodes::PortType portType,
                         QtNodes::PortIndex index) const override;

    // Layout terms on the grid, also used by the node painter to place the
    // domain glyph exactly against the port rows.
    double HeaderBand(QtNodes::NodeId nodeId) const;
    double PortRowPitch() const;
    double FirstPortRowCenter(QtNodes::NodeId nodeId) const;

private:
    // Caption + label (when visible) measured against this node.
    double HeaderExtent(QtNodes::NodeId nodeId) const;
    // Y of port row `index` center, grid-aligned.
    double PortRowCenter(QtNodes::NodeId nodeId, QtNodes::PortIndex index) const;
};

}  // namespace NodeGUI
