#include "ParameterNodeGeometry.h"

#include "NodeDataModel.h"
#include "NodeGlyphs.h"
#include "ParameterBlock.h"

#include <QtNodes/internal/AbstractGraphModel.hpp>
#include <QtNodes/internal/DataFlowGraphModel.hpp>

#include <QFontMetricsF>

#include <algorithm>

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
    if (!delegate) {
        return;
    }

    QSize s = size(nodeId);
    if (!delegate->ParameterBlock().size.isNull()) {
        const QSizeF block = delegate->ParameterBlock().size;
        s.setWidth(std::max(s.width(),
                            static_cast<int>(block.width() + 2.0 * kParameterBlockMarginX)));
        s.setHeight(s.height()
                    + static_cast<int>(block.height() + kParameterBlockMarginBottom));
    }

    // Reserve space for the domain glyph below the port rows when the node's
    // current proportions cannot fit it anywhere (mirrors what the parameter
    // block does for its rows; see TypedNodePainter::DrawTypeGlyph).
    if (HasTypeGlyph(delegate->name())) {
        GlyphAreaInput input;
        input.nodeWidth = s.width();
        // DefaultHorizontalNodeGeometry fixes _portSpacing at 10 in its ctor.
        constexpr double kPortSpacing = 10.0;
        input.spacing = kPortSpacing;

        double top = input.spacing + captionRect(nodeId).height();
        if (_graphModel.nodeData(nodeId, QtNodes::NodeRole::LabelVisible).toBool()) {
            top += labelRect(nodeId).height() + input.spacing / 2.0;
        }
        input.areaTop = top + input.spacing;

        input.areaBottom = s.height() - input.spacing;
        if (!delegate->ParameterBlock().size.isNull()) {
            input.areaBottom =
                ParameterBlockRect(delegate->ParameterBlock(), s).top() - 6.0;
        }

        const QFontMetricsF metrics(QFont{});
        input.rowHeight = metrics.height();
        input.inRows = delegate->nPorts(QtNodes::PortType::In);
        input.outRows = delegate->nPorts(QtNodes::PortType::Out);
        auto columnWidth = [&](QtNodes::PortType portType) {
            double width = 0.0;
            for (QtNodes::PortIndex i = 0;
                 i < delegate->nPorts(portType);
                 ++i) {
                const QString text =
                    delegate->portCaptionVisible(portType, i)
                        ? delegate->portCaption(portType, i)
                        : delegate->dataType(portType, i).name;
                width = std::max(width, metrics.horizontalAdvance(text));
            }
            return width;
        };
        input.inColumnWidth = columnWidth(QtNodes::PortType::In);
        input.outColumnWidth = columnWidth(QtNodes::PortType::Out);

        const double padding = GlyphAreaPadding(input);
        if (padding > 0.0) {
            s.setHeight(s.height() + static_cast<int>(padding + 1.0));
        }
    }

    if (s != size(nodeId)) {
        _graphModel.setNodeData(nodeId, QtNodes::NodeRole::Size, s);
    }
}

}  // namespace NodeGUI
