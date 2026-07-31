#pragma once

#include <NodeAPI/Graph.h>

#include <QtNodes/BasicGraphicsScene>
#include <QtNodes/NodeDelegateModelRegistry>

#include "NodeGraphModel.h"

#include <QString>
#include <QSize>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class QColor;
class QGraphicsRectItem;
class QGraphicsTextItem;

namespace NodeGUI {

// Loads a NodeAPI Graph into a QtNodes scene for display.
class GraphScene {
public:
    GraphScene();
    ~GraphScene();

    // Disable copy.
    GraphScene(const GraphScene&) = delete;
    GraphScene& operator=(const GraphScene&) = delete;

    // Load node-type templates from disk, then load the graph JSON.
    // Returns an empty string on success, otherwise an error message.
    QString LoadGraph(const std::string& graphJsonPath,
                      const std::string& templatesDir);

    // Starts a new empty graph, keeping the previously loaded node-type
    // templates. Returns an empty string on success.
    QString NewGraph();

    // Loads only the node-type templates (no graph), so the palette and
    // node creation work before any graph is opened. Returns an empty
    // string on success.
    QString LoadTemplates(const std::string& templatesDir);

    QtNodes::BasicGraphicsScene* Scene() const { return scene_.get(); }

    NodeGraphModel* Model() const { return model_.get(); }

    const NodeAPI::Graph& Graph() const { return graph_; }

    // Re-position all nodes into a left-to-right layered layout based on the
    // graph topology. Nodes are spaced so they do not overlap.
    void AutoArrange();

    // Write the current graph (including any moved node positions) to disk.
    // Returns an empty string on success, otherwise an error message.
    QString SaveGraph(const std::string& path) const;

    // Serialize/restore the complete live graph for application-level
    // Undo/Redo. Snapshot synchronizes manual node movements first.
    std::string Snapshot();
    QString RestoreSnapshot(const std::string& json);

    // Invoked after a user-visible graph mutation has fully reached the
    // NodeAPI model.
    void SetChangeCallback(std::function<void()> callback);

    // Instantiates a node of the given type at the given scene position.
    // When requestedId is non-empty it is used as the instance id (must be
    // unique); otherwise an id is generated from the display name.
    // Returns an empty string on success, otherwise an error message.
    QString AddNodeAt(const std::string& typeId,
                      const QPointF& scenePos,
                      const std::string& requestedId = {});

    // Maps a QtNodes id back to the NodeAPI node id (empty when unknown).
    std::string NodeApiId(QtNodes::NodeId qtId) const;

    // Replaces a node's parameter map (same write path as the parameter
    // editor dialog). Returns an empty string on success.
    QString SetNodeParameters(QtNodes::NodeId qtId,
                              const std::map<std::string, std::string>& parameters);

    // Replaces a node's wireable parameter-input list. Returns an empty
    // string on success.
    QString SetNodeParameterInputs(QtNodes::NodeId qtId,
                                   const std::vector<std::string>& parameterInputs);

    // Changes a node's timing domain (empty = unassigned). Returns an empty
    // string on success, otherwise an error message. Types locked to a
    // specific domain reject changes.
    QString SetNodeDomain(QtNodes::NodeId qtId, const std::string& domain);

    // Renames a node instance, remapping its connections and bridges to the
    // new id. Returns an empty string on success, otherwise an error
    // message.
    QString RenameNode(QtNodes::NodeId qtId, const std::string& newId);

    // Copy the current scene positions back into the NodeAPI graph model.
    void SyncPositionsFromScene();

private:
    struct Adjacency {
        std::map<std::string, std::vector<std::string>> outgoing;
        std::map<std::string, std::vector<std::string>> incoming;
    };

    Adjacency BuildAdjacency() const;
    Adjacency BuildAdjacencyForNodes(const std::vector<std::string>& nodeIds) const;
    std::vector<std::string> TopologicalSort(const Adjacency& adj) const;
    std::map<std::string, int> ComputeLevels(const Adjacency& adj,
                                             const std::vector<std::string>& order) const;

    // Layout helpers that respect timing domains.
    std::map<std::string, std::vector<std::string>> GroupNodesByDomain() const;
    // Orders domain groups by dataflow: bridges form a domain-level DAG
    // (producer domain -> consumer domain) which is topologically sorted with
    // an alphabetical tie-break. Cyclic leftovers are appended alphabetically.
    std::vector<std::string> OrderDomainsByFlow(
        const std::map<std::string, std::vector<std::string>>& groups) const;
    void LayoutDomainNodes(const std::vector<std::string>& nodeIds,
                           const Adjacency& adj,
                           double originX,
                           double originY,
                           double& outWidth,
                           double& outHeight);

    // Actual rendered size of a node (caption + ports + parameter view).
    // Falls back to the scene geometry when the size cache has no entry.
    QSize NodeSize(const std::string& nodeId) const;

    void RegisterNodeTypes();
    void CreateNodes();
    // Creates the QtNodes graphics side for one graph node. Returns the
    // QtNodes id, or InvalidNodeId when the node's type is unknown.
    QtNodes::NodeId CreateNodeItem(const NodeAPI::Node& node);
    void CreateConnections();
    void CreateBridges();
    // Rebuilds model and scene from graph_ (after load/new).
    void RebuildScene();
    void NotifyChanged();

    // Visual domain grouping: colored outline + label per timing domain.
    void CreateDomainOutlines();
    void ClearDomainOutlines();
    QColor GetDomainColor(const std::string& domain) const;

public:
    // Public so the scene event filter can call it during node drags.
    void UpdateDomainOutlines();

    // Domain group interaction used by GraphView. A domain is activated by
    // double-clicking its outline/label or empty interior, then its background
    // can be dragged to move every member node together.
    bool SelectDomainAt(const QPointF& scenePos);
    bool BeginSelectedDomainDrag(const QPointF& scenePos);
    void MoveSelectedDomain(const QPointF& delta);
    void EndSelectedDomainDrag();
    void ClearDomainSelection();

    QtNodes::PortIndex FindPortIndex(const std::string& nodeId,
                                     const std::string& portName,
                                     QtNodes::PortType portType) const;

    NodeAPI::Graph graph_;
    std::shared_ptr<QtNodes::NodeDelegateModelRegistry> registry_;
    std::unique_ptr<NodeGraphModel> model_;
    std::unique_ptr<QtNodes::BasicGraphicsScene> scene_;
    std::string templatesDir_;

    // Map NodeAPI node id -> QtNodes NodeId.
    std::map<std::string, QtNodes::NodeId> nodeIdMap_;

    // Node instance currently being populated by CreateNodes(). Read by the
    // registry creators so the delegate model can embed the node's parameter
    // view before the scene constructs its graphics object.
    const NodeAPI::Node* pendingNode_ = nullptr;

    // Cached node geometry sizes so domain-outline updates don't re-query the
    // full geometry system on every mouse-move event.
    std::unordered_map<QtNodes::NodeId, QSize> nodeSizeCache_;

    // Event filter that keeps domain outlines synced while dragging nodes.
    std::unique_ptr<QObject> sceneEventFilter_;

    struct DomainVisuals {
        QGraphicsRectItem* outline = nullptr;
        QGraphicsTextItem* label = nullptr;
        QColor color;
    };
    std::map<std::string, DomainVisuals> domainVisuals_;
    std::string selectedDomain_;
    bool selectedDomainMoved_ = false;
    std::vector<QColor> domainColors_;
    std::function<void()> changeCallback_;

    bool PointHitsGraphContent(const QPointF& scenePos) const;
    void UpdateDomainSelectionStyle();
};

}  // namespace NodeGUI
