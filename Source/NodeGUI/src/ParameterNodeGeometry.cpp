#include "ParameterNodeGeometry.h"

#include "NodeDataModel.h"
#include "ParameterBlock.h"

#include <QtNodes/internal/AbstractGraphModel.hpp>
#include <QtNodes/internal/DataFlowGraphModel.hpp>

namespace NodeGUI {

ParameterNodeGeometry::ParameterNodeGeometry(QtNodes::AbstractGraphModel& graphModel)
    : DefaultHorizontalNodeGeometry(graphModel) {}

void ParameterNodeGeometry::recomputeSize(QtNodes::NodeId const nodeId) const {
    DefaultHorizontalNodeGeometry::recomputeSize(nodeId);

    auto* model = dynamic_cast<QtNodes::DataFlowGraphModel*>(&_graphModel);
    if (!model) {
        return;
    }
    const auto* delegate = model->delegateModel<NodeInstanceModel>(nodeId);
    if (!delegate || delegate->ParameterBlock().size.isNull()) {
        return;
    }

    const QSizeF block = delegate->ParameterBlock().size;
    QSize s = size(nodeId);
    s.setWidth(std::max(s.width(),
                        static_cast<int>(block.width() + 2.0 * kParameterBlockMarginX)));
    s.setHeight(s.height()
                + static_cast<int>(block.height() + kParameterBlockMarginBottom));
    _graphModel.setNodeData(nodeId, QtNodes::NodeRole::Size, s);
}

}  // namespace NodeGUI
