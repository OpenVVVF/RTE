#pragma once

#include <QtNodes/internal/DefaultHorizontalNodeGeometry.hpp>

namespace NodeGUI {

// Horizontal node geometry that additionally reserves room at the bottom of
// the node for the painted parameter block (see ParameterBlock.h). Replaces
// the default geometry so nodes size themselves without an embedded widget.
class ParameterNodeGeometry : public QtNodes::DefaultHorizontalNodeGeometry {
public:
    explicit ParameterNodeGeometry(QtNodes::AbstractGraphModel& graphModel);

    void recomputeSize(QtNodes::NodeId nodeId) const override;
};

}  // namespace NodeGUI
