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

    QtNodes::BasicGraphicsScene* Scene() const { return scene_.get(); }

    NodeGraphModel* Model() const { return model_.get(); }

    const NodeAPI::Graph& Graph() const { return graph_; }

    // Re-position all nodes into a left-to-right layered layout based on the
    // graph topology. Nodes are spaced so they do not overlap.
    void AutoArrange();

    // Write the current graph (including any moved node positions) to disk.
    // Returns an empty string on success, otherwise an error message.
    QString SaveGraph(const std::string& path) const;

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
    void CreateConnections();
    void CreateBridges();

    // Visual domain grouping: colored outline + label per timing domain.
    void CreateDomainOutlines();
    void ClearDomainOutlines();
    QColor GetDomainColor(const std::string& domain) const;

public:
    // Public so the scene event filter can call it during node drags.
    void UpdateDomainOutlines();

    QtNodes::PortIndex FindPortIndex(const std::string& nodeId,
                                     const std::string& portName,
                                     QtNodes::PortType portType) const;

    NodeAPI::Graph graph_;
    std::shared_ptr<QtNodes::NodeDelegateModelRegistry> registry_;
    std::unique_ptr<NodeGraphModel> model_;
    std::unique_ptr<QtNodes::BasicGraphicsScene> scene_;

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
    std::vector<QColor> domainColors_;
};

}  // namespace NodeGUI
