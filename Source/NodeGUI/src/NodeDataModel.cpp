#include "NodeDataModel.h"

#include <NodeAPI/WireType.h>

#include <QString>

#include <algorithm>

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
    if (!domain_.empty()) {
        caption += QStringLiteral("\n[");
        caption += QString::fromStdString(domain_);
        caption += QStringLiteral("]");
    }
    return caption;
}

void NodeInstanceModel::SetDomain(std::string domain) {
    domain_ = std::move(domain);
    Q_EMIT requestNodeUpdate();
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

void NodeInstanceModel::SetParameters(std::map<std::string, std::string> parameters) {
    parameters_ = std::move(parameters);
    RebuildPortsAndBlock();
    Q_EMIT requestNodeUpdate();
}

void NodeInstanceModel::SetParameterInputs(std::vector<std::string> parameterInputs) {
    const std::size_t oldPortCount = inputPorts_.size();
    parameterInputs_ = std::move(parameterInputs);
    RebuildPortsAndBlock();

    if (inputPorts_.size() > oldPortCount) {
        Q_EMIT portsInserted();
    } else if (inputPorts_.size() < oldPortCount) {
        Q_EMIT portsDeleted();
    }
    Q_EMIT requestNodeUpdate();
}

std::vector<NodeAPI::Port> VisibleInputPorts(
    const NodeAPI::NodeType& nodeType,
    const std::vector<std::string>& parameterInputs,
    const std::set<std::string>& connectedInputs) {
    std::vector<NodeAPI::Port> ports;
    for (const auto& port : nodeType.inputPorts) {
        // Optional ports that fall back to a same-named parameter are hidden
        // while the parameter is neither wired nor connected.
        const bool hasParameterFallback = nodeType.parameterTypes.count(port.name) != 0;
        const bool wired = std::find(parameterInputs.begin(),
                                     parameterInputs.end(),
                                     port.name) != parameterInputs.end();
        if (port.optional && hasParameterFallback && !wired
            && !connectedInputs.count(port.name)) {
            continue;
        }
        ports.push_back(port);
    }

    // Synthesized parameter input ports, skipping names a type port covers.
    for (const auto& name : parameterInputs) {
        const bool covered = std::any_of(ports.begin(), ports.end(), [&](const NodeAPI::Port& p) {
            return p.name == name;
        });
        if (covered) {
            continue;
        }
        const auto typeIt = nodeType.parameterTypes.find(name);
        ports.push_back(NodeAPI::Port{.name = name,
                                      .direction = NodeAPI::PortDirection::Input,
                                      .type = typeIt != nodeType.parameterTypes.end()
                                                  ? typeIt->second
                                                  : NodeAPI::WireType{}});
    }
    return ports;
}

std::set<std::string> ConnectedOptionalInputs(const NodeAPI::Graph& graph,
                                              const NodeAPI::Node& node,
                                              const NodeAPI::NodeType& nodeType) {
    std::set<std::string> connected;
    for (const auto& port : nodeType.inputPorts) {
        if (!port.optional || nodeType.parameterTypes.count(port.name) == 0) {
            continue;
        }
        const NodeAPI::PortRef ref{node.id, port.name};
        for (const auto& connection : graph.GetConnections()) {
            if (connection.to == ref) {
                connected.insert(port.name);
            }
        }
        for (const auto& bridge : graph.GetBridges()) {
            if (bridge.consumer == ref) {
                connected.insert(port.name);
            }
        }
    }
    return connected;
}

void NodeInstanceModel::SetConnectedInputs(std::set<std::string> connectedInputs) {
    if (connectedInputs == connectedInputs_) {
        return;
    }
    const std::size_t oldPortCount = inputPorts_.size();
    connectedInputs_ = std::move(connectedInputs);
    RebuildPortsAndBlock();

    if (inputPorts_.size() > oldPortCount) {
        Q_EMIT portsInserted();
    } else if (inputPorts_.size() < oldPortCount) {
        Q_EMIT portsDeleted();
    }
    Q_EMIT requestNodeUpdate();
}

void NodeInstanceModel::RebuildPortsAndBlock() {
    // Type ports (optional ones visible only when wired or connected), then
    // synthesized parameter ports (same order GraphScene::FindPortIndex
    // uses).
    inputPorts_ = VisibleInputPorts(nodeType_, parameterInputs_, connectedInputs_);

    // Wired parameters leave the painted panel; they are bound by connection.
    std::map<std::string, std::string> constants = parameters_;
    for (const auto& name : parameterInputs_) {
        constants.erase(name);
    }
    parameterBlock_ = PrepareParameterBlock(constants);
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
