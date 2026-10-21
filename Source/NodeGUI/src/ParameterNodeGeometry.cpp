#include "ParameterNodeGeometry.h"

#include "NodeDataModel.h"
#include "NodeGlyphs.h"
#include "ParameterBlock.h"
#include "WireRoute.h"

#include <QtNodes/internal/AbstractGraphModel.hpp>
#include <QtNodes/internal/DataFlowGraphModel.hpp>

#include <QFontMetricsF>

#include <algorithm>

namespace NodeGUI {

namespace {

// Scene grid multiples (WireRoute::kGridPitch == 10): port-row pitch and the
// gap between the header band and the first port row. Chosen so sockets sit
// on grid lines when the node origin snaps (row centers are grid multiples).
constexpr double kSpacing = 10.0;
constexpr double kRowPitch = 30.0;

}  // namespace

ParameterNodeGeometry::ParameterNodeGeometry(QtNodes::AbstractGraphModel& graphModel)
    : DefaultHorizontalNodeGeometry(graphModel) {}

double ParameterNodeGeometry::HeaderExtent(QtNodes::NodeId nodeId) const {
    double extent = captionRect(nodeId).height();
    if (_graphModel.nodeData(nodeId, QtNodes::NodeRole::LabelVisible).toBool()) {
        extent += labelRect(nodeId).height() + kSpacing / 2.0;
    }
    return extent;
}

double ParameterNodeGeometry::PortRowPitch() const {
    return kRowPitch;
}

double ParameterNodeGeometry::HeaderBand(QtNodes::NodeId nodeId) const {
    return WireRoute::RoundUpToGrid(HeaderExtent(nodeId));
}

double ParameterNodeGeometry::FirstPortRowCenter(QtNodes::NodeId nodeId) const {
    return HeaderBand(nodeId) + 2.0 * WireRoute::kGridPitch;
}

double ParameterNodeGeometry::PortRowCenter(QtNodes::NodeId nodeId,
                                            QtNodes::PortIndex index) const {
    return FirstPortRowCenter(nodeId) + static_cast<double>(index) * kRowPitch;
}

QPointF ParameterNodeGeometry::portPosition(QtNodes::NodeId const nodeId,
                                            QtNodes::PortType const portType,
                                            QtNodes::PortIndex const portIndex) const {
    const QSize s = size(nodeId);
    const double y = PortRowCenter(nodeId, portIndex);
    return QPointF(portType == QtNodes::PortType::In ? 0.0 : s.width(), y);
}

void ParameterNodeGeometry::recomputeSize(QtNodes::NodeId const nodeId) const {
    auto* model = dynamic_cast<QtNodes::DataFlowGraphModel*>(&_graphModel);
    const auto* delegate =
        model ? model->delegateModel<NodeInstanceModel>(nodeId) : nullptr;

    const unsigned inPorts =
        _graphModel.nodeData(nodeId, QtNodes::NodeRole::InPortCount).toUInt();
    const unsigned outPorts =
        _graphModel.nodeData(nodeId, QtNodes::NodeRole::OutPortCount).toUInt();
    const unsigned rows = std::max(inPorts, outPorts);

    const QFontMetricsF metrics(QFont{});
    auto columnWidth = [&](QtNodes::PortType portType) {
        if (!delegate) {
            return 0.0;
        }
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
    const double inTextWidth = columnWidth(QtNodes::PortType::In);
    const double outTextWidth = columnWidth(QtNodes::PortType::Out);
    double textWidth = captionRect(nodeId).width();
    if (_graphModel.nodeData(nodeId, QtNodes::NodeRole::LabelVisible).toBool()) {
        textWidth = std::max(textWidth, labelRect(nodeId).width());
    }

    const double headerBand = WireRoute::RoundUpToGrid(HeaderExtent(nodeId));
    double width = WireRoute::RoundUpToGrid(
        std::max(inTextWidth + outTextWidth + 4.0 * kSpacing,
                 textWidth + 2.0 * kSpacing));
    double height = rows > 0 ? headerBand + 2.0 * WireRoute::kGridPitch +
                                   static_cast<double>(rows) * kRowPitch
                             : headerBand + 2.0 * WireRoute::kGridPitch;

    QSize s(static_cast<int>(width), static_cast<int>(height));

    if (delegate && !delegate->ParameterBlock().size.isNull()) {
        const QSizeF block = delegate->ParameterBlock().size;
        s.setWidth(std::max(
            s.width(),
            static_cast<int>(WireRoute::RoundUpToGrid(
                block.width() + 2.0 * kParameterBlockMarginX))));
        s.setHeight(static_cast<int>(WireRoute::RoundUpToGrid(
            static_cast<double>(s.height()) + block.height() + kParameterBlockMarginBottom)));
    }

    // Reserve space for the domain glyph below the port rows when the node's
    // current proportions cannot fit it anywhere (mirrors what the parameter
    // block does for its rows; see TypedNodePainter::DrawTypeGlyph).
    if (delegate && HasTypeGlyph(delegate->name())) {
        GlyphAreaInput input;
        input.nodeWidth = s.width();
        input.spacing = kSpacing;
        input.areaTop = headerBand + kSpacing;
        input.areaBottom = s.height() - kSpacing;
        if (!delegate->ParameterBlock().size.isNull()) {
            input.areaBottom =
                ParameterBlockRect(delegate->ParameterBlock(), s).top() - 6.0;
        }
        input.rowHeight = kRowPitch - kSpacing;   // pitch minus spacing share
        input.inRows = inPorts;
        input.outRows = outPorts;
        input.inColumnWidth = inTextWidth;
        input.outColumnWidth = outTextWidth;

        const double padding = GlyphAreaPadding(input);
        if (padding > 0.0) {
            s.setHeight(static_cast<int>(
                WireRoute::RoundUpToGrid(s.height() + padding)));
        }
    }

    _graphModel.setNodeData(nodeId, QtNodes::NodeRole::Size, s);
}

}  // namespace NodeGUI
