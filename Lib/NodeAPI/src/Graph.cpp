#include "NodeAPI/Graph.h"

#include <algorithm>

namespace NodeAPI {

bool Graph::AddNodeType(NodeType nodeType) {
    if (nodeType.id.empty()) return false;
    if (TypeIdTaken(nodeType.id)) return false;
    nodeTypes_.push_back(std::move(nodeType));
    return true;
}

bool Graph::RemoveNodeType(const std::string& typeId) {
    for (auto it = nodeTypes_.begin(); it != nodeTypes_.end(); ++it) {
        if (it->id == typeId) {
            nodeTypes_.erase(it);
            return true;
        }
    }
    return false;
}

std::optional<NodeType> Graph::FindNodeType(const std::string& typeId) const {
    for (const auto& nodeType : nodeTypes_) {
        if (nodeType.id == typeId) return nodeType;
    }
    return std::nullopt;
}

bool Graph::AddNode(Node node) {
    if (node.id.empty()) return false;
    if (NodeIdTaken(node.id)) return false;

    const auto nodeType = FindNodeType(node.type);
    if (!nodeType.has_value()) return false;
    if (nodeType->maxInstances > 0 && CountInstances(node.type) >= nodeType->maxInstances) return false;
    if (!nodeType->domain.empty()) {
        node.domain = nodeType->domain;
    }

    nodes_.push_back(std::move(node));
    return true;
}

bool Graph::RemoveNode(const std::string& nodeId) {
    for (auto it = nodes_.begin(); it != nodes_.end(); ++it) {
        if (it->id == nodeId) {
            nodes_.erase(it);
            connections_.erase(
                std::remove_if(connections_.begin(), connections_.end(),
                               [&nodeId](const Connection& c) {
                                   return c.from.nodeId == nodeId || c.to.nodeId == nodeId;
                               }),
                connections_.end());
            bridges_.erase(
                std::remove_if(bridges_.begin(), bridges_.end(),
                               [&nodeId](const Bridge& b) {
                                   return b.producer.nodeId == nodeId || b.consumer.nodeId == nodeId;
                               }),
                bridges_.end());
            return true;
        }
    }
    return false;
}

std::optional<Node> Graph::FindNode(const std::string& nodeId) const {
    for (const auto& node : nodes_) {
        if (node.id == nodeId) return node;
    }
    return std::nullopt;
}

bool Graph::SetNodePosition(const std::string& nodeId, Position position) {
    for (auto& node : nodes_) {
        if (node.id == nodeId) {
            node.position = position;
            return true;
        }
    }
    return false;
}

bool Graph::SetNodeParameters(const std::string& nodeId,
                              std::map<std::string, std::string> parameters) {
    for (auto& node : nodes_) {
        if (node.id == nodeId) {
            node.parameters = std::move(parameters);
            return true;
        }
    }
    return false;
}

bool Graph::SetNodeDomain(const std::string& nodeId, std::string domain) {
    for (auto& node : nodes_) {
        if (node.id == nodeId) {
            node.domain = std::move(domain);
            return true;
        }
    }
    return false;
}

bool Graph::Connect(Connection connection) {
    if (connection.id.empty()) return false;
    if (ConnectionIdTaken(connection.id)) return false;
    if (!EndpointExists(connection.from, PortDirection::Output)) return false;
    if (!EndpointExists(connection.to, PortDirection::Input)) return false;
    if (!TypeCheck(connection)) return false;
    if (ConsumerHasBridge(connection.to)) return false;
    connections_.push_back(std::move(connection));
    return true;
}

bool Graph::Disconnect(const std::string& connectionId) {
    for (auto it = connections_.begin(); it != connections_.end(); ++it) {
        if (it->id == connectionId) {
            connections_.erase(it);
            return true;
        }
    }
    return false;
}

bool Graph::AddBridge(Bridge bridge) {
    if (bridge.id.empty()) return false;
    if (BridgeIdTaken(bridge.id)) return false;
    if (!EndpointExists(bridge.producer, PortDirection::Output)) return false;
    if (!EndpointExists(bridge.consumer, PortDirection::Input)) return false;

    const auto producerPort = FindPort(bridge.producer);
    const auto consumerPort = FindPort(bridge.consumer);
    if (!producerPort || !consumerPort) return false;

    /* The consumer's type must match the bridge exactly.  The producer may
     * be a voltage/current scalar feeding a dimensionless bridge (implicit
     * unit extraction at the store, same rule as connections). */
    if (consumerPort->type != bridge.type) return false;
    if (producerPort->type != bridge.type) {
        const bool extractOk =
            bridge.type.frame == Frame::Scalar &&
            bridge.type.quantity == Quantity::Dimensionless &&
            producerPort->type.frame == Frame::Scalar &&
            (producerPort->type.quantity == Quantity::Voltage ||
             producerPort->type.quantity == Quantity::Current);
        if (!extractOk) return false;
    }

    if (ConsumerHasConnection(bridge.consumer)) return false;
    if (ConsumerHasBridge(bridge.consumer)) return false;

    bridges_.push_back(std::move(bridge));
    return true;
}

bool Graph::RemoveBridge(const std::string& bridgeId) {
    for (auto it = bridges_.begin(); it != bridges_.end(); ++it) {
        if (it->id == bridgeId) {
            bridges_.erase(it);
            return true;
        }
    }
    return false;
}

std::optional<Bridge> Graph::FindBridge(const std::string& bridgeId) const {
    for (const auto& bridge : bridges_) {
        if (bridge.id == bridgeId) return bridge;
    }
    return std::nullopt;
}

std::optional<Connection> Graph::FindConnection(const std::string& connectionId) const {
    for (const auto& connection : connections_) {
        if (connection.id == connectionId) return connection;
    }
    return std::nullopt;
}

std::optional<Port> Graph::FindPort(const PortRef& ref) const {
    const auto node = FindNode(ref.nodeId);
    if (!node) return std::nullopt;

    const auto type = FindNodeType(node->type);
    if (!type) return std::nullopt;

    if (const auto port = type->FindOutputPort(ref.portName)) return *port;
    if (const auto port = type->FindInputPort(ref.portName)) return *port;

    /* A parameter flagged as parameterInput on this instance is exposed as an
     * input port, synthesized from its parameterType. */
    for (const auto& name : node->parameterInputs) {
        if (name == ref.portName) {
            if (const auto paramType = type->FindParameterType(ref.portName)) {
                return Port{.name = ref.portName,
                            .direction = PortDirection::Input,
                            .type = *paramType};
            }
        }
    }
    return std::nullopt;
}

bool Graph::TypeCheck(const Connection& connection) const {
    const auto fromPort = FindPort(connection.from);
    const auto toPort = FindPort(connection.to);
    if (!fromPort || !toPort) return false;

    if (fromPort->type == toPort->type) return true;

    /* Implicit unit extraction: a dimensionless scalar input accepts a
     * voltage or current scalar output; codegen emits the .in(unit)
     * extraction at the binding site. */
    if (toPort->type.frame == Frame::Scalar &&
        toPort->type.quantity == Quantity::Dimensionless &&
        fromPort->type.frame == Frame::Scalar &&
        (fromPort->type.quantity == Quantity::Voltage ||
         fromPort->type.quantity == Quantity::Current)) {
        return true;
    }

    return false;
}

bool Graph::NodeIdTaken(const std::string& nodeId) const {
    for (const auto& node : nodes_) {
        if (node.id == nodeId) return true;
    }
    return false;
}

std::size_t Graph::CountInstances(const std::string& typeId) const {
    std::size_t count = 0;
    for (const auto& node : nodes_) {
        if (node.type == typeId) ++count;
    }
    return count;
}

bool Graph::TypeIdTaken(const std::string& typeId) const {
    for (const auto& nodeType : nodeTypes_) {
        if (nodeType.id == typeId) return true;
    }
    return false;
}

bool Graph::ConnectionIdTaken(const std::string& connectionId) const {
    for (const auto& connection : connections_) {
        if (connection.id == connectionId) return true;
    }
    return false;
}

bool Graph::BridgeIdTaken(const std::string& bridgeId) const {
    for (const auto& bridge : bridges_) {
        if (bridge.id == bridgeId) return true;
    }
    return false;
}

bool Graph::EndpointExists(const PortRef& ref, PortDirection expectedDirection) const {
    const auto port = FindPort(ref);
    if (!port) return false;
    return port->direction == expectedDirection;
}

bool Graph::ConsumerHasConnection(const PortRef& ref) const {
    for (const auto& connection : connections_) {
        if (connection.to == ref) return true;
    }
    return false;
}

bool Graph::ConsumerHasBridge(const PortRef& ref) const {
    for (const auto& bridge : bridges_) {
        if (bridge.consumer == ref) return true;
    }
    return false;
}

}  // namespace NodeAPI
