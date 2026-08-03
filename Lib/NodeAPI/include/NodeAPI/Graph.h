#pragma once

#include "NodeAPI/Node.h"
#include "NodeAPI/NodeType.h"
#include "NodeAPI/Port.h"

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace NodeAPI {

struct Connection {
    std::string id;
    PortRef from;
    PortRef to;

    friend bool operator==(const Connection& lhs, const Connection& rhs) = default;
    friend bool operator!=(const Connection& lhs, const Connection& rhs) = default;
};

struct Bridge {
    std::string id;
    WireType type;
    PortRef producer;
    PortRef consumer;

    friend bool operator==(const Bridge& lhs, const Bridge& rhs) = default;
    friend bool operator!=(const Bridge& lhs, const Bridge& rhs) = default;
};

class Graph {
public:
    Graph() = default;

    // Node type database.
    bool AddNodeType(NodeType nodeType);
    bool RemoveNodeType(const std::string& typeId);
    std::optional<NodeType> FindNodeType(const std::string& typeId) const;
    const std::vector<NodeType>& GetNodeTypes() const { return nodeTypes_; }

    // Node instances.
    bool AddNode(Node node);
    bool RemoveNode(const std::string& nodeId);
    std::optional<Node> FindNode(const std::string& nodeId) const;
    const std::vector<Node>& GetNodes() const { return nodes_; }

    // Updates the stored canvas position of an existing node. Returns false if
    // the node id is not found.
    bool SetNodePosition(const std::string& nodeId, Position position);

    // Replaces the parameter map of an existing node. Returns false if the
    // node id is not found.
    bool SetNodeParameters(const std::string& nodeId,
                           std::map<std::string, std::string> parameters);

    // Updates the timing domain of an existing node. Returns false if the
    // node id is not found. An empty domain means "unassigned".
    bool SetNodeDomain(const std::string& nodeId, std::string domain);

    // Replaces the list of parameters exposed as wireable input ports on an
    // existing node. Returns false if the node id is not found.
    bool SetNodeParameterInputs(const std::string& nodeId,
                                std::vector<std::string> parameterInputs);

    // Sets the exclude-from-compile flag of an existing node. Returns false
    // if the node id is not found.
    bool SetNodeExcludeFromCompile(const std::string& nodeId, bool exclude);

    // Sets the exclude-from-compile-and-children flag of an existing node.
    // Returns false if the node id is not found.
    bool SetNodeExcludeFromCompileRecursive(const std::string& nodeId, bool exclude);

    // Replaces the zero-bound input list of an existing node (used by
    // emitters after pruning excluded nodes). Returns false if the node id
    // is not found.
    bool SetNodeZeroInputs(const std::string& nodeId, std::vector<std::string> zeroInputs);

    // The effective set of excluded nodes: every node flagged with either
    // exclusion flag, plus every node reachable downstream of a
    // recursive-flagged node. Maps each excluded node id to the
    // recursive-flagged ancestor that excludes it, or to itself for directly
    // flagged nodes (first ancestor wins when several overlap).
    std::map<std::string, std::string> ComputeExcludedNodes() const;

    // Connections.
    bool Connect(Connection connection);
    bool Disconnect(const std::string& connectionId);
    std::optional<Connection> FindConnection(const std::string& connectionId) const;
    const std::vector<Connection>& GetConnections() const { return connections_; }

    // Bridges (cross-domain links).
    bool AddBridge(Bridge bridge);
    bool RemoveBridge(const std::string& bridgeId);
    std::optional<Bridge> FindBridge(const std::string& bridgeId) const;
    const std::vector<Bridge>& GetBridges() const { return bridges_; }

    // Looks up the port definition from the node's type. Returns a copy so the
    // result is safe to keep across further graph operations.
    std::optional<Port> FindPort(const PortRef& ref) const;

    // True when the connection's endpoints exist, directions are compatible,
    // and the wire types match.
    bool TypeCheck(const Connection& connection) const;

    const std::string& GetName() const { return name_; }
    void SetName(std::string name) { name_ = std::move(name); }

private:
    bool NodeIdTaken(const std::string& nodeId) const;
    std::size_t CountInstances(const std::string& typeId) const;
    bool TypeIdTaken(const std::string& typeId) const;
    bool ConnectionIdTaken(const std::string& connectionId) const;
    bool BridgeIdTaken(const std::string& bridgeId) const;
    bool EndpointExists(const PortRef& ref, PortDirection expectedDirection) const;
    bool ConsumerHasConnection(const PortRef& ref) const;
    bool ConsumerHasBridge(const PortRef& ref) const;

    std::string name_;
    std::vector<NodeType> nodeTypes_;
    std::vector<Node> nodes_;
    std::vector<Connection> connections_;
    std::vector<Bridge> bridges_;
};

}  // namespace NodeAPI
