#pragma once

#include <NodeAPI/Graph.h>

#include <QtNodes/BasicGraphicsScene>
#include <QtNodes/DataFlowGraphModel>
#include <QtNodes/NodeDelegateModelRegistry>

#include <QString>
#include <map>
#include <memory>
#include <string>

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
    std::vector<std::string> TopologicalSort(const Adjacency& adj) const;
    std::map<std::string, int> ComputeLevels(const Adjacency& adj,
                                             const std::vector<std::string>& order) const;
    void RegisterNodeTypes();
    void CreateNodes();
    void CreateConnections();
    void CreateBridges();

    QtNodes::PortIndex FindPortIndex(const std::string& nodeId,
                                     const std::string& portName,
                                     QtNodes::PortType portType) const;

    NodeAPI::Graph graph_;
    std::shared_ptr<QtNodes::NodeDelegateModelRegistry> registry_;
    std::unique_ptr<QtNodes::DataFlowGraphModel> model_;
    std::unique_ptr<QtNodes::BasicGraphicsScene> scene_;

    // Map NodeAPI node id -> QtNodes NodeId.
    std::map<std::string, QtNodes::NodeId> nodeIdMap_;
};

}  // namespace NodeGUI
