#pragma once

#include <NodeAPI/Graph.h>

#include "WireRoute.h"

#include <QtNodes/DataFlowGraphModel>
#include <QtNodes/Definitions>
#include <QtNodes/internal/ConnectionIdHash.hpp>

#include <QObject>
#include <QPointF>
#include <QPolygonF>
#include <QSize>
#include <QString>

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace NodeGUI {

// Data-flow graph model that enforces NodeAPI rules while the user creates or
// deletes connections in the QtNodes view.
class NodeGraphModel : public QtNodes::DataFlowGraphModel {
    Q_OBJECT

public:

    // Plant workflow (see GraphScene::SetPlantWorkflow): Validate skips the
    // controller timing validator's cycle rule and rejects any cross-domain
    // bridge outright (plant graphs are single-domain, no bridges).
    void SetPlantWorkflow(bool on) { plantWorkflow_ = on; }
    bool PlantWorkflow() const { return plantWorkflow_; }
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

    // The NodeAPI graph id of a live QtNodes connection (empty when unknown).
    std::string GraphConnectionId(QtNodes::ConnectionId const connectionId) const;

    // The QtNodes id of a live connection by its NodeAPI graph id
    // (QtNodes::InvalidConnectionId equivalent — outNodeId ==
    // QtNodes::InvalidNodeId — when unknown).
    QtNodes::ConnectionId QtConnectionIdForGraphId(const std::string& graphId) const;

    // Manual routing bends of a connection, in scene coordinates. Empty =
    // automatic routing. nullptr when the connection has no stored bends.
    const std::vector<QPointF>* ConnectionWaypoints(QtNodes::ConnectionId const connectionId) const;
    void SetConnectionWaypoints(QtNodes::ConnectionId const connectionId,
                                std::vector<QPointF> scenePoints);

    // One node's committed scene translation for the wire-follow rule.
    struct NodeTranslation {
        QtNodes::NodeId node;
        QPointF delta;
    };

    // Wire-follow on node-move settles (GraphScene calls this ONCE per
    // settle, covering every node whose scene origin ran ahead of the graph
    // position): every connection holding manual bends that touches a moved
    // node translates all its bends by that node's delta, so bends ride
    // along instead of spiking across the freed row. Labeled connections
    // skip (the renderer shows net-label stubs and ignores their bends;
    // data stays intact); connections without bends stay untouched (no
    // phantom entries are created). A connection shared by two moved nodes
    // translates ONCE: the first attached moved node (deterministic order:
    // ascending node id) shifts it and records the follow; the counterpart
    // endpoint moving by the SAME delta is consumed instead, so a selection
    // drag (or sequential same-delta moves of both anchors) rigidly moves
    // the whole routed shape — never double-shifted. Consumed pairs are
    // forgotten, so the next gesture follows again. Returns the ids of the
    // connections actually translated (for repaints); the caller drains the
    // single per-batch change notification as usual.
    std::vector<QtNodes::ConnectionId> FollowMovedNodes(
        const std::vector<NodeTranslation>& moved);

    // Optional display-only net label of a connection (nullptr when unset).
    // A labeled connection renders as two short stubs + name tags instead of
    // the long wire (KiCad-style net labels); NodeAPI/codegen never see it.
    const QString* ConnectionLabel(QtNodes::ConnectionId const connectionId) const;
    // Empty clears the label. Uniqueness of non-empty labels is vetted by
    // GraphScene::SetConnectionLabel before it reaches this setter.
    void SetConnectionLabel(QtNodes::ConnectionId const connectionId,
                            const QString& label);
    // The connection currently holding the given non-empty label
    // (QtNodes::InvalidConnectionId equivalent — outNodeId ==
    // QtNodes::InvalidNodeId — when the label is free).
    QtNodes::ConnectionId NetLabelOwner(const QString& label) const;

    // Optional display-only chip displacement of a labeled connection
    // (nullptr when unset): the tag chips render at stub-end + offset, so the
    // user can drag the chips out of dense areas (KiCad-style). Both chips of
    // the pair translate by the same scene-space vector; the stubs stay
    // anchored. nullptr when the connection rides at the computed anchor.
    const QPointF* ConnectionLabelOffset(QtNodes::ConnectionId const connectionId) const;
    // A null offset (0,0) clears the entry — an on-anchor chip has no state.
    void SetConnectionLabelOffset(QtNodes::ConnectionId const connectionId,
                                  const QPointF& offset);

    // Optional user-chosen node size (nullptr when unset). The gesture fully
    // bypasses QtNodes' widget-driven resize (typed nodes embed no widget, so
    // the corner pocket armed but did nothing): the override is the persisted
    // size the geometry clamps against the content minimum (see
    // ParameterNodeGeometry::recomputeSize); grid-multiple values only.
    const QSize* NodeSizeOverride(QtNodes::NodeId const nodeId) const;
    // A null/invalid size clears the entry. Emits nodeUpdated on change so
    // the scene item re-lays out + repaints (same repaint path as a port-set
    // or parameter change).
    void SetNodeSizeOverride(QtNodes::NodeId const nodeId, const QSize& size);

    // The mini-router's choice for a waypoint-free connection (scene
    // coordinates for the non-legacy line). nullptr when the cache has no
    // entry for the connection. GraphScene fills it per recompute pass.
    const WireRoute::AutoRouteChoice* AutoRoute(QtNodes::ConnectionId const connectionId) const;
    void SetAutoRoutes(std::unordered_map<QtNodes::ConnectionId,
                                          WireRoute::AutoRouteChoice> routes);
    void ClearAutoRoutes();

    // Returns true when either endpoint node of the connection is excluded
    // from compile (used to grey out the wire).
    bool IsExcluded(QtNodes::ConnectionId const connectionId) const;

    // Returns true when the node itself is excluded from compile.
    bool IsNodeExcluded(QtNodes::NodeId const nodeId) const;

    // Replaces the cached set of effectively excluded NodeAPI node ids
    // (direct flags plus downstream chains). Called by GraphScene whenever
    // the graph changes; the painters read the cache on every paint.
    void RefreshExcludedNodes(std::set<std::string> excludedNodeIds);

    // Retrieve the reason for the last rejected connection attempt and clear it.
    // Returns an empty string if nothing was rejected since the last clear.
    QString TakeLastRejectionReason();

    void ClearRejectionState();

    // Called after the NodeAPI graph has been changed by an interactive
    // connection or deletion. GraphScene uses this to record application-level
    // undo history instead of QtNodes' visual-only undo stack.
    std::function<void()> onGraphChanged;

Q_SIGNALS:
    // Emitted when a domainless node adopts its peer's domain on connect, so
    // the scene can refresh the node's caption and the domain outlines.
    void nodeDomainAssigned(QtNodes::NodeId qtId, const std::string& domain);

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

    // Recomputes which optional, parameter-backed input ports of a node are
    // connected and updates the delegate's visible ports.
    void RefreshOptionalPortVisibility(const std::string& nodeId);

    std::string GenerateId() const;
    QString Validate(QtNodes::ConnectionId const connectionId,
                     bool& asBridge) const;

    bool plantWorkflow_ = false;

    NodeAPI::Graph& graph_;
    std::unordered_map<QtNodes::NodeId, std::string> qtIdToNodeId_;
    mutable std::unordered_map<QtNodes::ConnectionId, GraphConnection> connectionMap_;
    std::unordered_map<QtNodes::ConnectionId, std::vector<QPointF>> connectionWaypoints_;
    // Pending wire-follow pairs: which endpoint translation already shifted
    // the connection's bends (set by FollowMovedNodes). A same-delta move of
    // the OTHER endpoint consumes the entry instead of double-shifting;
    // manual bend edits (SetConnectionWaypoints) and teardown clear it.
    std::unordered_map<QtNodes::ConnectionId, NodeTranslation> waypointFollows_;
    std::unordered_map<QtNodes::ConnectionId, QString> connectionLabels_;
    std::unordered_map<QtNodes::ConnectionId, QPointF> connectionLabelOffsets_;
    std::unordered_map<QtNodes::NodeId, QSize> nodeSizeOverrides_;
    std::unordered_map<QtNodes::ConnectionId, WireRoute::AutoRouteChoice> autoRoutes_;
    mutable std::size_t nextId_ = 0;
    mutable QString lastRejectionReason_;
    // Effective exclusion set (NodeAPI node ids), refreshed by GraphScene.
    std::set<std::string> excludedNodeIds_;
};

}  // namespace NodeGUI
