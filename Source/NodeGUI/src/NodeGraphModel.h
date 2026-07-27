#pragma once

#include <NodeAPI/Graph.h>

#include <QtNodes/DataFlowGraphModel>
#include <QtNodes/Definitions>
#include <QtNodes/internal/ConnectionIdHash.hpp>

#include <QObject>
#include <QString>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>

namespace NodeGUI {

// Data-flow graph model that enforces NodeAPI rules while the user creates or
// deletes connections in the QtNodes view.
class NodeGraphModel : public QtNodes::DataFlowGraphModel {
    Q_OBJECT

public:
    NodeGraphModel(std::shared_ptr<QtNodes::NodeDelegateModelRegistry> registry,
                   NodeAPI::Graph& graph);

    // Rebuild the QtNodes -> NodeAPI node id map after the scene is populated.
    void BuildNodeIdMap(const std::map<std::string, QtNodes::NodeId>& nodeIdMap);

    // Register an existing connection/bridge that was loaded from disk so that
    // future delete operations know which NodeAPI id it maps to.
    void RegisterExistingConnection(QtNodes::ConnectionId qtId,
                                    const std::string& graphId,
                                    bool isBridge);

    bool connectionPossible(QtNodes::ConnectionId const connectionId) const override;
    void addConnection(QtNodes::ConnectionId const connectionId) override;
    bool deleteConnection(QtNodes::ConnectionId const connectionId) override;

    // Removes the node from the NodeAPI graph (which cascades to its
    // connections and bridges) before letting QtNodes tear down its side.
    bool deleteNode(QtNodes::NodeId const nodeId) override;

    // Returns true if the QtNodes connection id maps to a NodeAPI Bridge.
    bool IsBridge(QtNodes::ConnectionId const connectionId) const;

    // Retrieve the reason for the last rejected connection attempt and clear it.
    // Returns an empty string if nothing was rejected since the last clear.
    QString TakeLastRejectionReason();

    void ClearRejectionState();

private:
    struct GraphConnection {
        std::string graphId;
        bool isBridge = false;
    };

    std::string ProducerNodeId(QtNodes::NodeId qtId) const;
    std::string ConsumerNodeId(QtNodes::NodeId qtId) const;

    std::optional<NodeAPI::PortRef> MakePortRef(QtNodes::NodeId qtId,
                                                QtNodes::PortIndex index,
                                                QtNodes::PortType type) const;

    std::string GenerateId() const;
    QString Validate(QtNodes::ConnectionId const connectionId,
                     bool& asBridge) const;

    NodeAPI::Graph& graph_;
    std::unordered_map<QtNodes::NodeId, std::string> qtIdToNodeId_;
    mutable std::unordered_map<QtNodes::ConnectionId, GraphConnection> connectionMap_;
    mutable std::size_t nextId_ = 0;
    mutable QString lastRejectionReason_;
};

}  // namespace NodeGUI
