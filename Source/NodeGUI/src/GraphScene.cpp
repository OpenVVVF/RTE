#include "GraphScene.h"

#include "NodeDataModel.h"

#include <NodeAPI/NodeTemplates.h>
#include <NodeAPI/Serialization.h>

#include <QtNodes/Definitions>
#include <QtNodes/internal/NodeGraphicsObject.hpp>

#include <QFile>
#include <QJsonObject>
#include <QObject>
#include <QPointF>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace NodeGUI {

namespace {

std::optional<std::string> ReadTextFile(const std::string& path) {
    std::ifstream stream(path, std::ios::in | std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }
    return std::string{std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>()};
}

QString MakeNodeCaption(const NodeAPI::Node& node, const NodeAPI::NodeType& nodeType) {
    QString caption = QString::fromStdString(node.displayName.empty() ? node.id : node.displayName);
    caption += QStringLiteral("\n");
    caption += QString::fromStdString(nodeType.id);
    if (!node.domain.empty()) {
        caption += QStringLiteral("\n[");
        caption += QString::fromStdString(node.domain);
        caption += QStringLiteral("]");
    }
    return caption;
}

}  // namespace

GraphScene::GraphScene()
    : registry_{std::make_shared<QtNodes::NodeDelegateModelRegistry>()}
    , model_{std::make_unique<NodeGraphModel>(registry_, graph_)}
    , scene_{std::make_unique<QtNodes::BasicGraphicsScene>(*model_)} {}

GraphScene::~GraphScene() = default;

QString GraphScene::LoadGraph(const std::string& graphJsonPath,
                              const std::string& templatesDir) {
    graph_ = NodeAPI::Graph{};
    nodeIdMap_.clear();

    const auto templatesResult = NodeAPI::LoadNodeTypesFromDirectory(graph_, templatesDir);
    if (!templatesResult.ok && templatesResult.typesLoaded == 0) {
        std::ostringstream message;
        message << "Failed to load node templates from '" << templatesDir << "':";
        for (const auto& error : templatesResult.errors) {
            message << "\n  " << error;
        }
        return QString::fromStdString(message.str());
    }

    const auto graphText = ReadTextFile(graphJsonPath);
    if (!graphText) {
        return QStringLiteral("Failed to read graph file: %1").arg(QString::fromStdString(graphJsonPath));
    }

    try {
        NodeAPI::LoadIntoGraph(graph_, *graphText);
    } catch (const std::exception& e) {
        return QStringLiteral("Failed to parse graph JSON: %1").arg(QString::fromStdString(e.what()));
    }

    // Reset model/scene so each load starts fresh.
    model_ = std::make_unique<NodeGraphModel>(registry_, graph_);
    scene_ = std::make_unique<QtNodes::BasicGraphicsScene>(*model_);

    RegisterNodeTypes();
    CreateNodes();
    model_->BuildNodeIdMap(nodeIdMap_);
    CreateConnections();
    CreateBridges();

    return QString{};
}

GraphScene::Adjacency GraphScene::BuildAdjacency() const {
    Adjacency adj;

    for (const auto& node : graph_.GetNodes()) {
        adj.outgoing[node.id];
        adj.incoming[node.id];
    }

    auto addEdge = [&](const std::string& from, const std::string& to) {
        if (from == to) {
            return;
        }
        adj.outgoing[from].push_back(to);
        adj.incoming[to].push_back(from);
    };

    for (const auto& connection : graph_.GetConnections()) {
        addEdge(connection.from.nodeId, connection.to.nodeId);
    }
    for (const auto& bridge : graph_.GetBridges()) {
        addEdge(bridge.producer.nodeId, bridge.consumer.nodeId);
    }

    // Remove duplicate edges between the same nodes.
    for (auto& [id, neighbors] : adj.outgoing) {
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
    }
    for (auto& [id, neighbors] : adj.incoming) {
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
    }

    return adj;
}

std::vector<std::string> GraphScene::TopologicalSort(const Adjacency& adj) const {
    std::unordered_map<std::string, std::size_t> inDegree;
    for (const auto& node : graph_.GetNodes()) {
        inDegree[node.id] = 0;
    }
    for (const auto& [to, fromNodes] : adj.incoming) {
        inDegree[to] = fromNodes.size();
    }

    std::queue<std::string> queue;
    for (const auto& node : graph_.GetNodes()) {
        if (inDegree[node.id] == 0) {
            queue.push(node.id);
        }
    }

    std::vector<std::string> order;
    order.reserve(graph_.GetNodes().size());

    while (!queue.empty()) {
        const auto current = queue.front();
        queue.pop();
        order.push_back(current);

        for (const auto& next : adj.outgoing.at(current)) {
            if (--inDegree[next] == 0) {
                queue.push(next);
            }
        }
    }

    return order;
}

std::map<std::string, int> GraphScene::ComputeLevels(const Adjacency& adj,
                                                     const std::vector<std::string>& order) const {
    std::map<std::string, int> levels;
    for (const auto& id : order) {
        levels[id] = 0;
    }

    for (const auto& id : order) {
        for (const auto& next : adj.outgoing.at(id)) {
            levels[next] = std::max(levels[next], levels[id] + 1);
        }
    }

    return levels;
}

void GraphScene::AutoArrange() {
    if (graph_.GetNodes().empty()) {
        return;
    }

    const Adjacency adj = BuildAdjacency();
    const std::vector<std::string> order = TopologicalSort(adj);

    // If the graph has a cycle, fall back to a simple grid instead of a
    // topology-driven layout.
    if (order.size() != graph_.GetNodes().size()) {
        constexpr double spacing = 250.0;
        std::size_t index = 0;
        for (const auto& [nodeId, qtId] : nodeIdMap_) {
            const double x = static_cast<double>(index % 5) * spacing;
            const double y = static_cast<double>(index / 5) * spacing;
            model_->setNodeData(qtId,
                                QtNodes::NodeRole::Position,
                                QVariant::fromValue(QPointF(x, y)));
            ++index;
        }
        return;
    }

    const std::map<std::string, int> levels = ComputeLevels(adj, order);

    // Group nodes by level.
    std::map<int, std::vector<std::string>> nodesByLevel;
    for (const auto& [id, level] : levels) {
        nodesByLevel[level].push_back(id);
    }

    constexpr double horizontalSpacing = 300.0;
    constexpr double verticalSpacing = 150.0;

    // Track assigned y positions so we can sort later levels by predecessor
    // median y.
    std::map<std::string, double> yPositions;

    for (auto& [level, nodeIds] : nodesByLevel) {
        // Sort nodes within the level by the average y of their predecessors.
        std::sort(nodeIds.begin(), nodeIds.end(), [&](const std::string& a, const std::string& b) {
            const auto& predsA = adj.incoming.at(a);
            const auto& predsB = adj.incoming.at(b);

            auto avgY = [&](const std::vector<std::string>& preds) -> double {
                if (preds.empty()) {
                    return 0.0;
                }
                double sum = 0.0;
                for (const auto& p : preds) {
                    sum += yPositions.count(p) ? yPositions[p] : 0.0;
                }
                return sum / static_cast<double>(preds.size());
            };

            const double avgA = avgY(predsA);
            const double avgB = avgY(predsB);
            if (std::abs(avgA - avgB) > 1.0) {
                return avgA < avgB;
            }
            return a < b;
        });

        const double x = static_cast<double>(level) * horizontalSpacing;
        for (std::size_t i = 0; i < nodeIds.size(); ++i) {
            const double y = static_cast<double>(i) * verticalSpacing;
            yPositions[nodeIds[i]] = y;

            const auto it = nodeIdMap_.find(nodeIds[i]);
            if (it != nodeIdMap_.end()) {
                model_->setNodeData(it->second,
                                    QtNodes::NodeRole::Position,
                                    QVariant::fromValue(QPointF(x, y)));
            }
        }
    }

    // Isolated nodes have no edges. Place them in a column to the right.
    std::vector<std::string> isolated;
    for (const auto& node : graph_.GetNodes()) {
        if (adj.incoming.at(node.id).empty() && adj.outgoing.at(node.id).empty()) {
            isolated.push_back(node.id);
        }
    }

    if (!isolated.empty()) {
        double maxX = 0.0;
        for (const auto& [id, qtId] : nodeIdMap_) {
            const QVariant posVar = model_->nodeData(qtId, QtNodes::NodeRole::Position);
            const QPointF pos = posVar.value<QPointF>();
            maxX = std::max(maxX, pos.x());
        }

        const double isolatedX = maxX + horizontalSpacing + 50.0;
        for (std::size_t i = 0; i < isolated.size(); ++i) {
            const auto it = nodeIdMap_.find(isolated[i]);
            if (it != nodeIdMap_.end()) {
                const double y = static_cast<double>(i) * verticalSpacing;
                model_->setNodeData(it->second,
                                    QtNodes::NodeRole::Position,
                                    QVariant::fromValue(QPointF(isolatedX, y)));
            }
        }
    }

    SyncPositionsFromScene();
}

void GraphScene::SyncPositionsFromScene() {
    for (const auto& [nodeId, qtId] : nodeIdMap_) {
        QPointF pos;
        if (auto* graphicsObject = scene_->nodeGraphicsObject(qtId)) {
            // The graphics object holds the real on-screen position, including
            // any manual drags that QtNodes does not push back into the model.
            pos = graphicsObject->pos();
        } else {
            const QVariant posVar = model_->nodeData(qtId, QtNodes::NodeRole::Position);
            pos = posVar.value<QPointF>();
        }
        graph_.SetNodePosition(nodeId, NodeAPI::Position{pos.x(), pos.y()});
    }
}

QString GraphScene::SaveGraph(const std::string& path) const {
    std::ofstream stream(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!stream) {
        return QStringLiteral("Failed to open file for writing: %1").arg(QString::fromStdString(path));
    }

    const std::string json = NodeAPI::SaveToJson(graph_);
    stream << json;
    if (!stream) {
        return QStringLiteral("Failed to write graph to: %1").arg(QString::fromStdString(path));
    }

    return QString{};
}

void GraphScene::RegisterNodeTypes() {
    for (const auto& nodeType : graph_.GetNodeTypes()) {
        // Each creator captures a copy of its node type so the model can expose
        // the correct ports.
        registry_->registerModel([nodeType]() -> std::unique_ptr<QtNodes::NodeDelegateModel> {
            NodeAPI::Node dummy;
            dummy.id = nodeType.id;
            dummy.type = nodeType.id;
            return std::make_unique<NodeInstanceModel>(std::move(dummy), nodeType);
        });
    }
}

void GraphScene::CreateNodes() {
    for (const auto& node : graph_.GetNodes()) {
        const auto nodeType = graph_.FindNodeType(node.type);
        if (!nodeType) {
            continue;
        }

        const QString modelName = QString::fromStdString(node.type);
        const QtNodes::NodeId qtId = model_->addNode(modelName);
        nodeIdMap_[node.id] = qtId;

        model_->setNodeData(qtId,
                            QtNodes::NodeRole::Position,
                            QVariant::fromValue(QPointF(node.position.x, node.position.y)));
        model_->setNodeData(qtId,
                            QtNodes::NodeRole::Caption,
                            QVariant::fromValue(MakeNodeCaption(node, *nodeType)));
        model_->setNodeData(qtId,
                            QtNodes::NodeRole::Label,
                            QVariant::fromValue(QString::fromStdString(node.id)));
    }
}

QtNodes::PortIndex GraphScene::FindPortIndex(const std::string& nodeId,
                                             const std::string& portName,
                                             QtNodes::PortType portType) const {
    const auto node = graph_.FindNode(nodeId);
    if (!node) {
        return QtNodes::InvalidPortIndex;
    }

    const auto nodeType = graph_.FindNodeType(node->type);
    if (!nodeType) {
        return QtNodes::InvalidPortIndex;
    }

    const auto& ports = (portType == QtNodes::PortType::In) ? nodeType->inputPorts
                                                            : nodeType->outputPorts;
    for (std::size_t i = 0; i < ports.size(); ++i) {
        if (ports[i].name == portName) {
            return static_cast<QtNodes::PortIndex>(i);
        }
    }
    return QtNodes::InvalidPortIndex;
}

void GraphScene::CreateConnections() {
    for (const auto& connection : graph_.GetConnections()) {
        const auto fromIt = nodeIdMap_.find(connection.from.nodeId);
        const auto toIt = nodeIdMap_.find(connection.to.nodeId);
        if (fromIt == nodeIdMap_.end() || toIt == nodeIdMap_.end()) {
            continue;
        }

        const QtNodes::PortIndex outPort = FindPortIndex(connection.from.nodeId,
                                                         connection.from.portName,
                                                         QtNodes::PortType::Out);
        const QtNodes::PortIndex inPort = FindPortIndex(connection.to.nodeId,
                                                        connection.to.portName,
                                                        QtNodes::PortType::In);
        if (outPort == QtNodes::InvalidPortIndex || inPort == QtNodes::InvalidPortIndex) {
            continue;
        }

        const QtNodes::ConnectionId qtId{fromIt->second, outPort, toIt->second, inPort};
        // Use the base implementation so we don't double-add to the NodeAPI graph.
        model_->DataFlowGraphModel::addConnection(qtId);
        model_->RegisterExistingConnection(qtId, connection.id, false);
    }
}

void GraphScene::CreateBridges() {
    for (const auto& bridge : graph_.GetBridges()) {
        const auto producerIt = nodeIdMap_.find(bridge.producer.nodeId);
        const auto consumerIt = nodeIdMap_.find(bridge.consumer.nodeId);
        if (producerIt == nodeIdMap_.end() || consumerIt == nodeIdMap_.end()) {
            continue;
        }

        const QtNodes::PortIndex outPort = FindPortIndex(bridge.producer.nodeId,
                                                         bridge.producer.portName,
                                                         QtNodes::PortType::Out);
        const QtNodes::PortIndex inPort = FindPortIndex(bridge.consumer.nodeId,
                                                         bridge.consumer.portName,
                                                        QtNodes::PortType::In);
        if (outPort == QtNodes::InvalidPortIndex || inPort == QtNodes::InvalidPortIndex) {
            continue;
        }

        const QtNodes::ConnectionId qtId{producerIt->second, outPort, consumerIt->second, inPort};
        model_->DataFlowGraphModel::addConnection(qtId);
        model_->RegisterExistingConnection(qtId, bridge.id, true);
    }
}

}  // namespace NodeGUI
