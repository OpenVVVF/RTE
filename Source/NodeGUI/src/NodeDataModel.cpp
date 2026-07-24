#include "NodeDataModel.h"

#include <NodeAPI/WireType.h>

#include <QString>

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
