#include "NodeGraphModel.h"

#include "NodeDataModel.h"

#include <NodeAPI/Timing.h>

#include <QPointF>

#include <sstream>

namespace NodeGUI {

namespace {

bool ConsumerHasConnection(const NodeAPI::Graph& graph,
                           const NodeAPI::PortRef& consumer) {
    for (const auto& c : graph.GetConnections()) {
        if (c.to == consumer) return true;
    }
    return false;
}

bool ConsumerHasBridge(const NodeAPI::Graph& graph,
                       const NodeAPI::PortRef& consumer) {
    for (const auto& b : graph.GetBridges()) {
        if (b.consumer == consumer) return true;
    }
    return false;
}

}  // namespace

NodeGraphModel::NodeGraphModel(std::shared_ptr<QtNodes::NodeDelegateModelRegistry> registry,
                               NodeAPI::Graph& graph)
    : DataFlowGraphModel(std::move(registry))
    , graph_(graph) {}

void NodeGraphModel::BuildNodeIdMap(const std::map<std::string, QtNodes::NodeId>& nodeIdMap) {
    qtIdToNodeId_.clear();
    for (const auto& [nodeId, qtId] : nodeIdMap) {
        qtIdToNodeId_[qtId] = nodeId;
    }
}

void NodeGraphModel::RegisterExistingConnection(QtNodes::ConnectionId qtId,
                                                const std::string& graphId,
                                                bool isBridge) {
    connectionMap_[qtId] = GraphConnection{graphId, isBridge};
}

std::string NodeGraphModel::ProducerNodeId(QtNodes::NodeId qtId) const {
    const auto it = qtIdToNodeId_.find(qtId);
    return it != qtIdToNodeId_.end() ? it->second : std::string{};
}

std::string NodeGraphModel::ConsumerNodeId(QtNodes::NodeId qtId) const {
    return ProducerNodeId(qtId);
}

std::optional<NodeAPI::PortRef> NodeGraphModel::MakePortRef(QtNodes::NodeId qtId,
                                                            QtNodes::PortIndex index,
                                                            QtNodes::PortType type) const {
    const std::string nodeId = ProducerNodeId(qtId);
    if (nodeId.empty()) {
        return std::nullopt;
    }

    const auto node = graph_.FindNode(nodeId);
    if (!node) {
        return std::nullopt;
    }

    const auto nodeType = graph_.FindNodeType(node->type);
    if (!nodeType) {
        return std::nullopt;
    }

    if (type == QtNodes::PortType::In) {
        // Same ordering as the delegate: visible type ports (optional ones
        // only when wired or connected), then synthesized parameter ports.
        const auto connected = ConnectedOptionalInputs(graph_, *node, *nodeType);
        const auto ports = VisibleInputPorts(*nodeType, node->parameterInputs, connected);
        if (static_cast<std::size_t>(index) < ports.size()) {
            return NodeAPI::PortRef{nodeId, ports[static_cast<std::size_t>(index)].name};
        }
        return std::nullopt;
    }

    if (static_cast<std::size_t>(index) < nodeType->outputPorts.size()) {
        return NodeAPI::PortRef{nodeId, nodeType->outputPorts[static_cast<std::size_t>(index)].name};
    }
    return std::nullopt;
}

std::string NodeGraphModel::GenerateId() const {
    while (true) {
        std::ostringstream id;
        id << "gui_conn_" << nextId_++;
        const std::string candidate = id.str();

        bool taken = false;
        for (const auto& c : graph_.GetConnections()) {
            if (c.id == candidate) {
                taken = true;
                break;
            }
        }
        for (const auto& b : graph_.GetBridges()) {
            if (b.id == candidate) {
                taken = true;
                break;
            }
        }
        if (!taken) {
            return candidate;
        }
    }
}

QString NodeGraphModel::Validate(QtNodes::ConnectionId const connectionId,
                                 bool& asBridge) const {
    asBridge = false;

    const auto producerRef = MakePortRef(connectionId.outNodeId,
                                         connectionId.outPortIndex,
                                         QtNodes::PortType::Out);
    const auto consumerRef = MakePortRef(connectionId.inNodeId,
                                         connectionId.inPortIndex,
                                         QtNodes::PortType::In);

    if (!producerRef || !consumerRef) {
        return QStringLiteral("Invalid connection endpoints");
    }

    const auto producerPort = graph_.FindPort(*producerRef);
    const auto consumerPort = graph_.FindPort(*consumerRef);
    if (!producerPort || !consumerPort) {
        return QStringLiteral("Connection endpoints do not exist");
    }

    if (producerPort->direction != NodeAPI::PortDirection::Output) {
        return QStringLiteral("Producer port must be an output");
    }
    if (consumerPort->direction != NodeAPI::PortDirection::Input) {
        return QStringLiteral("Consumer port must be an input");
    }

    // Delegate type compatibility to NodeAPI (exact match or implicit unit
    // extraction), so the GUI cannot diverge from the graph's own rules.
    NodeAPI::Connection probe;
    probe.from = *producerRef;
    probe.to = *consumerRef;
    if (!graph_.TypeCheck(probe)) {
        return QStringLiteral("Port types do not match");
    }

    const auto producerNode = graph_.FindNode(producerRef->nodeId);
    const auto consumerNode = graph_.FindNode(consumerRef->nodeId);
    if (!producerNode || !consumerNode) {
        return QStringLiteral("Connected nodes do not exist");
    }

    const auto consumerType = graph_.FindNodeType(consumerNode->type);
    if (consumerType && consumerType->isEntryPoint) {
        return QStringLiteral("Entry-point node '%1' cannot have incoming connections")
            .arg(QString::fromStdString(consumerNode->id));
    }

    // A domainless node adopts its peer's domain (unless its node type locks
    // one, in which case validation below will fail as before).
    const auto producerType = graph_.FindNodeType(producerNode->type);
    const bool producerLocked = producerType && !producerType->domain.empty();
    const bool consumerLocked = consumerType && !consumerType->domain.empty();

    std::string producerDomain = producerNode->domain;
    std::string consumerDomain = consumerNode->domain;
    std::string assignProducer;
    std::string assignConsumer;
    if (producerDomain.empty() && !consumerDomain.empty() && !producerLocked) {
        assignProducer = consumerDomain;
        producerDomain = consumerDomain;
    } else if (consumerDomain.empty() && !producerDomain.empty() && !consumerLocked) {
        assignConsumer = producerDomain;
        consumerDomain = producerDomain;
    }

    const bool sameDomain = producerDomain == consumerDomain;
    asBridge = !sameDomain;

    if (asBridge) {
        if (ConsumerHasConnection(graph_, *consumerRef)) {
            return QStringLiteral("Consumer '%1.%2' already has an intra-domain connection; "
                                   "use a connection instead of a bridge")
                .arg(QString::fromStdString(consumerRef->nodeId),
                     QString::fromStdString(consumerRef->portName));
        }
        if (ConsumerHasBridge(graph_, *consumerRef)) {
            return QStringLiteral("Consumer '%1.%2' already has a bridge")
                .arg(QString::fromStdString(consumerRef->nodeId),
                     QString::fromStdString(consumerRef->portName));
        }
    } else {
        if (ConsumerHasBridge(graph_, *consumerRef)) {
            return QStringLiteral("Consumer '%1.%2' already has a bridge; "
                                   "remove it before adding a connection")
                .arg(QString::fromStdString(consumerRef->nodeId),
                     QString::fromStdString(consumerRef->portName));
        }
        if (ConsumerHasConnection(graph_, *consumerRef)) {
            return QStringLiteral("Input '%1.%2' already has a wire (one wire per input)")
                .arg(QString::fromStdString(consumerRef->nodeId),
                     QString::fromStdString(consumerRef->portName));
        }
    }

    NodeAPI::Graph temp = graph_;
    // Apply the domain adoption on the temp copy so the timing validator sees
    // the post-connection state.
    if (!assignProducer.empty()) {
        temp.SetNodeDomain(producerRef->nodeId, assignProducer);
    }
    if (!assignConsumer.empty()) {
        temp.SetNodeDomain(consumerRef->nodeId, assignConsumer);
    }

    if (asBridge) {
        NodeAPI::Bridge bridge;
        bridge.id = GenerateId();
        // The bridge carries the consumer's type; NodeAPI lets the producer
        // feed it via implicit unit extraction.
        bridge.type = consumerPort->type;
        bridge.producer = *producerRef;
        bridge.consumer = *consumerRef;
        if (!temp.AddBridge(std::move(bridge))) {
            return QStringLiteral("Bridge rejected by graph model");
        }
    } else {
        NodeAPI::Connection connection;
        connection.id = GenerateId();
        connection.from = *producerRef;
        connection.to = *consumerRef;
        if (!temp.Connect(std::move(connection))) {
            return QStringLiteral("Connection rejected by graph model");
        }
    }

    NodeAPI::Timing::Validator validator;
    const auto result = validator.Validate(temp);
    if (!result.ok && !result.errors.empty()) {
        return QString::fromStdString(result.errors.front());
    }

    return QString{};
}

bool NodeGraphModel::connectionPossible(QtNodes::ConnectionId const connectionId) const {
    bool asBridge = false;
    const QString error = Validate(connectionId, asBridge);
    if (!error.isEmpty()) {
        lastRejectionReason_ = error;
        return false;
    }
    return true;
}

QString NodeGraphModel::TakeLastRejectionReason() {
    QString result = lastRejectionReason_;
    lastRejectionReason_.clear();
    return result;
}

void NodeGraphModel::ClearRejectionState() {
    lastRejectionReason_.clear();
}

void NodeGraphModel::addConnection(QtNodes::ConnectionId const connectionId) {
    DataFlowGraphModel::addConnection(connectionId);

    bool asBridge = false;
    const QString error = Validate(connectionId, asBridge);
    if (!error.isEmpty()) {
        // Defensive: if something slipped past connectionPossible, remove it.
        DataFlowGraphModel::deleteConnection(connectionId);
        return;
    }

    const auto producerRef = MakePortRef(connectionId.outNodeId,
                                         connectionId.outPortIndex,
                                         QtNodes::PortType::Out);
    const auto consumerRef = MakePortRef(connectionId.inNodeId,
                                         connectionId.inPortIndex,
                                         QtNodes::PortType::In);
    if (!producerRef || !consumerRef) {
        DataFlowGraphModel::deleteConnection(connectionId);
        return;
    }

    const auto producerPort = graph_.FindPort(*producerRef);
    const auto consumerPort = graph_.FindPort(*consumerRef);
    if (!producerPort || !consumerPort) {
        DataFlowGraphModel::deleteConnection(connectionId);
        return;
    }

    // Domain adoption (approved by Validate on its temp copy): a domainless
    // node takes the peer's domain.
    const auto producerNode = graph_.FindNode(producerRef->nodeId);
    const auto consumerNode = graph_.FindNode(consumerRef->nodeId);
    if (producerNode && consumerNode) {
        if (producerNode->domain.empty() && !consumerNode->domain.empty()) {
            graph_.SetNodeDomain(producerRef->nodeId, consumerNode->domain);
            Q_EMIT nodeDomainAssigned(connectionId.outNodeId, consumerNode->domain);
        } else if (consumerNode->domain.empty() && !producerNode->domain.empty()) {
            graph_.SetNodeDomain(consumerRef->nodeId, producerNode->domain);
            Q_EMIT nodeDomainAssigned(connectionId.inNodeId, producerNode->domain);
        }
    }

    const std::string id = GenerateId();

    bool graphChanged = false;
    if (asBridge) {
        NodeAPI::Bridge bridge;
        bridge.id = id;
        // The bridge carries the consumer's type; NodeAPI lets the producer
        // feed it via implicit unit extraction.
        bridge.type = consumerPort->type;
        bridge.producer = *producerRef;
        bridge.consumer = *consumerRef;
        if (graph_.AddBridge(std::move(bridge))) {
            RegisterExistingConnection(connectionId, id, true);
            graphChanged = true;
        } else {
            DataFlowGraphModel::deleteConnection(connectionId);
        }
    } else {
        NodeAPI::Connection connection;
        connection.id = id;
        connection.from = *producerRef;
        connection.to = *consumerRef;
        if (graph_.Connect(std::move(connection))) {
            RegisterExistingConnection(connectionId, id, false);
            graphChanged = true;
        } else {
            DataFlowGraphModel::deleteConnection(connectionId);
        }
    }

    // A new connection may make an optional, parameter-backed port visible.
    RefreshOptionalPortVisibility(consumerRef->nodeId);
    if (graphChanged && onGraphChanged) {
        onGraphChanged();
    }
}

void NodeGraphModel::RefreshOptionalPortVisibility(const std::string& nodeId) {
    QtNodes::NodeId qtId = QtNodes::InvalidNodeId;
    for (const auto& [qt, id] : qtIdToNodeId_) {
        if (id == nodeId) {
            qtId = qt;
            break;
        }
    }
    if (qtId == QtNodes::InvalidNodeId) {
        return;
    }

    auto* delegate = delegateModel<NodeInstanceModel>(qtId);
    const auto node = graph_.FindNode(nodeId);
    if (!delegate || !node) {
        return;
    }
    const auto nodeType = graph_.FindNodeType(node->type);
    if (!nodeType) {
        return;
    }
    delegate->SetConnectedInputs(ConnectedOptionalInputs(graph_, *node, *nodeType));
}

bool NodeGraphModel::deleteConnection(QtNodes::ConnectionId const connectionId) {
    // Resolve the consumer before teardown so we can refresh its optional
    // port visibility afterwards (a freed optional port may hide again).
    const auto consumerRef = MakePortRef(connectionId.inNodeId,
                                         connectionId.inPortIndex,
                                         QtNodes::PortType::In);

    bool graphChanged = false;
    const auto it = connectionMap_.find(connectionId);
    if (it != connectionMap_.end()) {
        if (it->second.isBridge) {
            graphChanged = graph_.RemoveBridge(it->second.graphId);
        } else {
            graphChanged = graph_.Disconnect(it->second.graphId);
        }
        connectionMap_.erase(it);
    }

    const bool result = DataFlowGraphModel::deleteConnection(connectionId);

    if (consumerRef) {
        RefreshOptionalPortVisibility(consumerRef->nodeId);
    }
    if (graphChanged && onGraphChanged) {
        onGraphChanged();
    }
    return result;
}

bool NodeGraphModel::deleteNode(QtNodes::NodeId const nodeId) {
    bool graphChanged = false;
    const auto it = qtIdToNodeId_.find(nodeId);
    if (it != qtIdToNodeId_.end()) {
        // NodeAPI::Graph::RemoveNode also removes the node's connections and
        // bridges. The base-class teardown below still emits connectionDeleted
        // for each QtNodes connection; our deleteConnection override then just
        // finds them already gone and cleans up the id map.
        graphChanged = graph_.RemoveNode(it->second);
    }
    const bool result = DataFlowGraphModel::deleteNode(nodeId);
    if (graphChanged && onGraphChanged) {
        onGraphChanged();
    }
    return result;
}

bool NodeGraphModel::IsBridge(QtNodes::ConnectionId const connectionId) const {
    const auto it = connectionMap_.find(connectionId);
    return it != connectionMap_.end() && it->second.isBridge;
}

}  // namespace NodeGUI
