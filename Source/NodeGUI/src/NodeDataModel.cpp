#include "NodeDataModel.h"

#include <NodeAPI/WireType.h>

#include <QLabel>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

namespace NodeGUI {

namespace {

QString WireTypeToString(const NodeAPI::WireType& type) {
    return QString::fromStdString(NodeAPI::ToString(type.quantity) + "."
                                  + NodeAPI::ToString(type.frame) + "."
                                  + NodeAPI::ToString(type.dtype));
}

}  // namespace

TypedNodeData::TypedNodeData(QString typeId, QString typeName)
    : type_{std::move(typeId), std::move(typeName)} {}

QtNodes::NodeDataType TypedNodeData::type() const {
    return type_;
}

NodeInstanceModel::NodeInstanceModel(NodeAPI::Node node, NodeAPI::NodeType nodeType)
    : node_{std::move(node)}
    , nodeType_{std::move(nodeType)}
    , inputPorts_{nodeType_.inputPorts}
    , outputPorts_{nodeType_.outputPorts}
    , outputData_{std::make_shared<TypedNodeData>(WireTypeToString(NodeAPI::WireType{}),
                                                  QStringLiteral("void"))} {}

QString NodeInstanceModel::name() const {
    return QString::fromStdString(nodeType_.id);
}

QString NodeInstanceModel::caption() const {
    QString caption;
    if (!node_.displayName.empty()) {
        caption = QString::fromStdString(node_.displayName);
    } else {
        caption = QString::fromStdString(node_.id);
    }
    return caption;
}

unsigned int NodeInstanceModel::nPorts(QtNodes::PortType portType) const {
    switch (portType) {
        case QtNodes::PortType::In:
            return static_cast<unsigned int>(inputPorts_.size());
        case QtNodes::PortType::Out:
            return static_cast<unsigned int>(outputPorts_.size());
        default:
            return 0;
    }
}

QtNodes::NodeDataType NodeInstanceModel::dataType(QtNodes::PortType portType,
                                                  QtNodes::PortIndex portIndex) const {
    const NodeAPI::Port* port = nullptr;
    if (portType == QtNodes::PortType::In && portIndex < inputPorts_.size()) {
        port = &inputPorts_[portIndex];
    } else if (portType == QtNodes::PortType::Out && portIndex < outputPorts_.size()) {
        port = &outputPorts_[portIndex];
    }

    if (!port) {
        return QtNodes::NodeDataType{QStringLiteral("invalid"), QStringLiteral("invalid")};
    }

    const QString id = WireTypeToString(port->type);
    return QtNodes::NodeDataType{id, QString::fromStdString(port->name)};
}

QString NodeInstanceModel::portCaption(QtNodes::PortType portType,
                                       QtNodes::PortIndex portIndex) const {
    const NodeAPI::Port* port = nullptr;
    if (portType == QtNodes::PortType::In && portIndex < inputPorts_.size()) {
        port = &inputPorts_[portIndex];
    } else if (portType == QtNodes::PortType::Out && portIndex < outputPorts_.size()) {
        port = &outputPorts_[portIndex];
    }
    return port ? QString::fromStdString(port->name) : QString{};
}

bool NodeInstanceModel::portCaptionVisible(QtNodes::PortType /*portType*/,
                                           QtNodes::PortIndex /*portIndex*/) const {
    return true;
}

QtNodes::ConnectionPolicy NodeInstanceModel::portConnectionPolicy(QtNodes::PortType portType,
                                                                  QtNodes::PortIndex /*portIndex*/) const {
    return portType == QtNodes::PortType::Out ? QtNodes::ConnectionPolicy::Many
                                              : QtNodes::ConnectionPolicy::One;
}

void NodeInstanceModel::SetParameters(
    const std::map<std::string, std::string>& parameters,
    const std::map<std::string, NodeAPI::WireType>& parameterTypes) {
    const bool refreshing = (parameterWidget_ != nullptr);

    if (!refreshing) {
        if (parameters.empty()) {
            return;
        }

        parameterWidget_ = new QWidget;
        // Tinted, rounded panel so the parameter block reads as a distinct
        // region inside the node rather than more port captions. The widget
        // stays hidden until the scene embeds it; showing it beforehand would
        // make it a top-level window and break the node's size computation.
        parameterWidget_->setStyleSheet(QStringLiteral(
            "background: rgba(255, 255, 255, 14);"
            "border: 1px solid rgba(255, 255, 255, 45);"
            "border-radius: 4px;"));

        auto* layout = new QVBoxLayout(parameterWidget_);
        layout->setContentsMargins(6, 3, 6, 3);
        layout->setSpacing(1);
    }

    // Rebuild the rows in place so the widget can be refreshed after an edit
    // without re-embedding it into the scene.
    auto* layout = parameterWidget_->layout();
    while (QLayoutItem* item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    // Visibility may only be toggled once the widget is embedded (i.e. on a
    // refresh), so an emptied parameter map collapses the panel.
    if (refreshing) {
        parameterWidget_->setVisible(!parameters.empty());
    }

    for (const auto& [name, value] : parameters) {
        auto* row = new QLabel(QStringLiteral("%1: %2")
                                   .arg(QString::fromStdString(name),
                                        QString::fromStdString(value)),
                               parameterWidget_);
        // Amber italic monospace contrasts with the plain white port captions.
        row->setStyleSheet(QStringLiteral(
            "color: #e8c07a;"
            "font-style: italic;"
            "font-family: monospace;"
            "background: transparent;"
            "border: none;"));

        const auto typeIt = parameterTypes.find(name);
        if (typeIt != parameterTypes.end()) {
            row->setToolTip(WireTypeToString(typeIt->second));
        }

        layout->addWidget(row);
    }
}

void NodeInstanceModel::setInData(std::shared_ptr<QtNodes::NodeData> /*nodeData*/,
                                  QtNodes::PortIndex const /*portIndex*/) {
    // Viewer is read-only; no dataflow computation needed.
}

std::shared_ptr<QtNodes::NodeData> NodeInstanceModel::outData(QtNodes::PortIndex const port) {
    const NodeAPI::Port* outPort = nullptr;
    if (port < outputPorts_.size()) {
        outPort = &outputPorts_[port];
    }

    if (outPort) {
        return std::make_shared<TypedNodeData>(WireTypeToString(outPort->type),
                                               QString::fromStdString(outPort->name));
    }
    return nullptr;
}

}  // namespace NodeGUI
