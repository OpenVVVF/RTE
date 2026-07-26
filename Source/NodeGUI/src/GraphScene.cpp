#include "GraphScene.h"

#include "BridgeConnectionPainter.h"
#include "NodeDataModel.h"
#include "ParameterNodeGeometry.h"
#include "TypedNodePainter.h"

#include <NodeAPI/NodeTemplates.h>
#include <NodeAPI/Serialization.h>

#include <QtNodes/Definitions>
#include <QtNodes/internal/NodeGraphicsObject.hpp>

#include <QBrush>
#include <QColor>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFile>
#include <QFont>
#include <QFormLayout>
#include <QGraphicsRectItem>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsTextItem>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QObject>
#include <QPen>
#include <QPointF>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace NodeGUI {

namespace {

// Filters QGraphicsScene events so domain outlines can follow nodes while they
// are being dragged. QtNodes only emits nodeMoved on mouse release, so we poll
// the scene's mouse grabber during move events. Also opens the parameter
// editor when a node is double-clicked.
class SceneEventFilter : public QObject {
public:
    explicit SceneEventFilter(GraphScene& scene, QObject* parent = nullptr)
        : QObject(parent)
        , scene_(scene) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        (void)watched;
        if (event->type() == QEvent::GraphicsSceneMouseMove) {
            if (auto* scene = scene_.Scene()) {
                if (qgraphicsitem_cast<QtNodes::NodeGraphicsObject*>(scene->mouseGrabberItem())) {
                    scene_.UpdateDomainOutlines();
                }
            }
        } else if (event->type() == QEvent::GraphicsSceneMouseDoubleClick) {
            auto* mouseEvent = static_cast<QGraphicsSceneMouseEvent*>(event);
            auto* scene = scene_.Scene();
            if (!scene) {
                return false;
            }

            // Walk up from the hit item so clicks on the embedded parameter
            // panel (a child proxy widget) also resolve to the node.
            QGraphicsItem* item = scene->itemAt(mouseEvent->scenePos(), QTransform());
            while (item) {
                if (auto* ngo = qgraphicsitem_cast<QtNodes::NodeGraphicsObject*>(item)) {
                    scene_.EditNodeParameters(ngo->nodeId());
                    return false;
                }
                item = item->parentItem();
            }
        }
        return false;
    }

private:
    GraphScene& scene_;
};

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
    , scene_{std::make_unique<QtNodes::BasicGraphicsScene>(*model_)} {
    // Distinct colors for timing domains. The palette cycles if there are more
    // domains than colors.
    domainColors_ = {
        QColor(100, 149, 237),   // cornflower blue
        QColor(60, 179, 113),    // medium sea green
        QColor(255, 165, 0),     // orange
        QColor(221, 160, 221),   // plum
        QColor(255, 99, 71),     // tomato
        QColor(70, 130, 180),    // steel blue
        QColor(218, 165, 32),    // goldenrod
        QColor(147, 112, 219),   // medium purple
    };
}

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
    scene_->setNodeGeometry(std::make_unique<ParameterNodeGeometry>(*model_));
    scene_->setConnectionPainter(std::make_unique<BridgeConnectionPainter>());
    scene_->setNodePainter(std::make_unique<TypedNodePainter>());
    nodeSizeCache_.clear();

    RegisterNodeTypes();
    CreateNodes();
    model_->BuildNodeIdMap(nodeIdMap_);
    CreateConnections();
    CreateBridges();
    CreateDomainOutlines();

    sceneEventFilter_ = std::make_unique<SceneEventFilter>(*this, scene_.get());
    scene_->installEventFilter(sceneEventFilter_.get());

    QObject::connect(scene_.get(),
                     &QtNodes::BasicGraphicsScene::nodeMoved,
                     [this](QtNodes::NodeId) { UpdateDomainOutlines(); });

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

GraphScene::Adjacency GraphScene::BuildAdjacencyForNodes(const std::vector<std::string>& nodeIds) const {
    Adjacency adj;

    const std::unordered_set<std::string> nodeSet(nodeIds.begin(), nodeIds.end());
    for (const auto& id : nodeIds) {
        adj.outgoing[id];
        adj.incoming[id];
    }

    auto addEdge = [&](const std::string& from, const std::string& to) {
        if (from == to) {
            return;
        }
        if (!nodeSet.count(from) || !nodeSet.count(to)) {
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
    for (const auto& [id, neighbors] : adj.incoming) {
        inDegree[id] = neighbors.size();
    }

    std::queue<std::string> queue;
    for (const auto& [id, degree] : inDegree) {
        if (degree == 0) {
            queue.push(id);
        }
    }

    std::vector<std::string> order;
    order.reserve(adj.incoming.size());

    while (!queue.empty()) {
        const auto current = queue.front();
        queue.pop();
        order.push_back(current);

        const auto it = adj.outgoing.find(current);
        if (it == adj.outgoing.end()) {
            continue;
        }
        for (const auto& next : it->second) {
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

std::map<std::string, std::vector<std::string>> GraphScene::GroupNodesByDomain() const {
    std::map<std::string, std::vector<std::string>> groups;
    for (const auto& node : graph_.GetNodes()) {
        groups[node.domain.empty() ? "(no domain)" : node.domain].push_back(node.id);
    }
    return groups;
}

QSize GraphScene::NodeSize(const std::string& nodeId) const {
    const auto it = nodeIdMap_.find(nodeId);
    if (it == nodeIdMap_.end()) {
        return QSize(200, 120);
    }
    const auto cached = nodeSizeCache_.find(it->second);
    if (cached != nodeSizeCache_.end()) {
        return cached->second;
    }
    return scene_->nodeGeometry().size(it->second);
}

void GraphScene::LayoutDomainNodes(const std::vector<std::string>& nodeIds,
                                   const Adjacency& adj,
                                   double originX,
                                   double originY,
                                   double& outWidth,
                                   double& outHeight) {
    outWidth = 0.0;
    outHeight = 0.0;

    if (nodeIds.empty()) {
        return;
    }

    const std::vector<std::string> order = TopologicalSort(adj);

    // Gaps between nodes; column widths and row heights come from the actual
    // node sizes so embedded parameter views are not overlapped.
    constexpr double horizontalGap = 100.0;
    constexpr double verticalGap = 40.0;

    // Fallback grid for cyclic subgraphs.
    if (order.size() != nodeIds.size()) {
        constexpr std::size_t columns = 3;
        const std::size_t rows = (nodeIds.size() + columns - 1) / columns;

        std::vector<double> colWidths(columns, 0.0);
        std::vector<double> rowHeights(rows, 0.0);
        for (std::size_t i = 0; i < nodeIds.size(); ++i) {
            const QSize size = NodeSize(nodeIds[i]);
            colWidths[i % columns] = std::max(colWidths[i % columns],
                                              static_cast<double>(size.width()));
            rowHeights[i / columns] = std::max(rowHeights[i / columns],
                                               static_cast<double>(size.height()));
        }

        double y = originY;
        for (std::size_t r = 0; r < rows; ++r) {
            double x = originX;
            for (std::size_t c = 0; c < columns; ++c) {
                const std::size_t i = r * columns + c;
                if (i < nodeIds.size()) {
                    const auto it = nodeIdMap_.find(nodeIds[i]);
                    if (it != nodeIdMap_.end()) {
                        model_->setNodeData(it->second,
                                            QtNodes::NodeRole::Position,
                                            QVariant::fromValue(QPointF(x, y)));
                    }
                }
                x += colWidths[c] + horizontalGap;
            }
            outWidth = std::max(outWidth, x - originX - horizontalGap);
            y += rowHeights[r] + verticalGap;
        }
        outHeight = y - originY - verticalGap;
        return;
    }

    const std::map<std::string, int> levels = ComputeLevels(adj, order);

    std::map<int, std::vector<std::string>> nodesByLevel;
    for (const auto& id : nodeIds) {
        nodesByLevel[levels.at(id)].push_back(id);
    }

    // Each level's column is as wide as its widest node.
    std::map<int, double> levelX;
    double xCursor = originX;
    for (const auto& [level, ids] : nodesByLevel) {
        double width = 0.0;
        for (const auto& id : ids) {
            width = std::max(width, static_cast<double>(NodeSize(id).width()));
        }
        levelX[level] = xCursor;
        xCursor += width + horizontalGap;
    }
    outWidth = xCursor - originX - horizontalGap;

    std::map<std::string, double> yPositions;

    for (auto& [level, levelNodeIds] : nodesByLevel) {
        std::sort(levelNodeIds.begin(), levelNodeIds.end(), [&](const std::string& a, const std::string& b) {
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

        const double x = levelX[level];
        double y = originY;
        for (const auto& id : levelNodeIds) {
            yPositions[id] = y;

            const auto it = nodeIdMap_.find(id);
            if (it != nodeIdMap_.end()) {
                model_->setNodeData(it->second,
                                    QtNodes::NodeRole::Position,
                                    QVariant::fromValue(QPointF(x, y)));
            }

            y += static_cast<double>(NodeSize(id).height()) + verticalGap;
        }
        outHeight = std::max(outHeight, y - originY - verticalGap);
    }
}

void GraphScene::AutoArrange() {
    if (graph_.GetNodes().empty()) {
        return;
    }

    // Global fallback for cyclic graphs: plain grid, sized from the actual
    // node sizes.
    const Adjacency globalAdj = BuildAdjacency();
    const std::vector<std::string> globalOrder = TopologicalSort(globalAdj);
    if (globalOrder.size() != graph_.GetNodes().size()) {
        constexpr std::size_t columns = 5;
        constexpr double horizontalGap = 60.0;
        constexpr double verticalGap = 40.0;

        std::vector<std::pair<std::string, QtNodes::NodeId>> nodes(nodeIdMap_.begin(),
                                                                   nodeIdMap_.end());
        const std::size_t rows = (nodes.size() + columns - 1) / columns;

        std::vector<double> colWidths(columns, 0.0);
        std::vector<double> rowHeights(rows, 0.0);
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const QSize size = NodeSize(nodes[i].first);
            colWidths[i % columns] = std::max(colWidths[i % columns],
                                              static_cast<double>(size.width()));
            rowHeights[i / columns] = std::max(rowHeights[i / columns],
                                               static_cast<double>(size.height()));
        }

        double y = 0.0;
        for (std::size_t r = 0; r < rows; ++r) {
            double x = 0.0;
            for (std::size_t c = 0; c < columns; ++c) {
                const std::size_t i = r * columns + c;
                if (i < nodes.size()) {
                    model_->setNodeData(nodes[i].second,
                                        QtNodes::NodeRole::Position,
                                        QVariant::fromValue(QPointF(x, y)));
                }
                x += colWidths[c] + horizontalGap;
            }
            y += rowHeights[r] + verticalGap;
        }
        UpdateDomainOutlines();
        SyncPositionsFromScene();
        return;
    }

    // Layout each timing domain as its own subgraph, then place the domains
    // side-by-side so cross-domain bridges run horizontally between groups.
    const auto domainGroups = GroupNodesByDomain();

    constexpr double domainHorizontalSpacing = 150.0;
    constexpr double domainVerticalSpacing = 80.0;
    double currentX = domainHorizontalSpacing;
    double currentY = domainVerticalSpacing;
    double rowMaxHeight = 0.0;

    std::size_t domainIndex = 0;
    for (const auto& [domain, nodeIds] : domainGroups) {
        const Adjacency adj = BuildAdjacencyForNodes(nodeIds);

        double width = 0.0;
        double height = 0.0;
        LayoutDomainNodes(nodeIds, adj, currentX, currentY, width, height);

        currentX += width + domainHorizontalSpacing;
        rowMaxHeight = std::max(rowMaxHeight, height);

        // Wrap to a new row every two domains to keep the canvas reasonable.
        if (++domainIndex % 2 == 0 && domainIndex < domainGroups.size()) {
            currentX = domainHorizontalSpacing;
            currentY += rowMaxHeight + domainVerticalSpacing + 40.0;
            rowMaxHeight = 0.0;
        }
    }

    UpdateDomainOutlines();
    SyncPositionsFromScene();
}

void GraphScene::EditNodeParameters(QtNodes::NodeId qtId) {
    std::string nodeId;
    for (const auto& [id, qt] : nodeIdMap_) {
        if (qt == qtId) {
            nodeId = id;
            break;
        }
    }

    const auto node = nodeId.empty() ? std::nullopt : graph_.FindNode(nodeId);
    if (!node) {
        return;
    }
    const auto nodeType = graph_.FindNodeType(node->type);

    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("Parameters: %1").arg(QString::fromStdString(nodeId)));

    auto* form = new QFormLayout(&dialog);
    std::vector<std::pair<std::string, QLineEdit*>> editors;
    for (const auto& [name, value] : node->parameters) {
        auto* edit = new QLineEdit(QString::fromStdString(value), &dialog);
        if (nodeType) {
            if (const auto type = nodeType->FindParameterType(name)) {
                const std::string typeText = NodeAPI::ToString(type->quantity) + "."
                                             + NodeAPI::ToString(type->frame) + "."
                                             + NodeAPI::ToString(type->dtype);
                edit->setToolTip(QString::fromStdString(typeText));
            }
        }
        form->addRow(QString::fromStdString(name), edit);
        editors.emplace_back(name, edit);
    }

    if (editors.empty()) {
        form->addRow(new QLabel(QStringLiteral("This node has no parameters."), &dialog));
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);

    if (editors.empty() || dialog.exec() != QDialog::Accepted) {
        return;
    }

    std::map<std::string, std::string> updated;
    for (const auto& [name, edit] : editors) {
        updated[name] = edit->text().toStdString();
    }
    if (!graph_.SetNodeParameters(nodeId, updated)) {
        return;
    }

    // Refresh the painted parameter block and the cached geometry. The
    // delegate's requestNodeUpdate signal makes the scene recompute the node
    // size and repaint before we read the new size here.
    if (auto* delegate = model_->delegateModel<NodeInstanceModel>(qtId)) {
        delegate->SetParameters(updated);
    }
    nodeSizeCache_[qtId] = scene_->nodeGeometry().size(qtId);
    UpdateDomainOutlines();
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

QColor GraphScene::GetDomainColor(const std::string& domain) const {
    if (domain.empty() || domain == "(no domain)") {
        return QColor(160, 160, 160);
    }

    // Assign colors by the order domains first appear in the graph.
    std::vector<std::string> seen;
    for (const auto& node : graph_.GetNodes()) {
        const std::string nodeDomain = node.domain.empty() ? "(no domain)" : node.domain;
        if (std::find(seen.begin(), seen.end(), nodeDomain) == seen.end()) {
            seen.push_back(nodeDomain);
        }
    }

    const auto it = std::find(seen.begin(), seen.end(), domain);
    if (it == seen.end()) {
        return QColor(160, 160, 160);
    }

    const std::size_t index = static_cast<std::size_t>(std::distance(seen.begin(), it));
    return domainColors_[index % domainColors_.size()];
}

void GraphScene::ClearDomainOutlines() {
    for (auto& [domain, visuals] : domainVisuals_) {
        if (visuals.outline) {
            scene_->removeItem(visuals.outline);
            delete visuals.outline;
            visuals.outline = nullptr;
        }
        if (visuals.label) {
            scene_->removeItem(visuals.label);
            delete visuals.label;
            visuals.label = nullptr;
        }
    }
    domainVisuals_.clear();
}

void GraphScene::CreateDomainOutlines() {
    ClearDomainOutlines();

    const auto groups = GroupNodesByDomain();
    for (const auto& [domain, nodeIds] : groups) {
        if (nodeIds.empty()) {
            continue;
        }

        const QColor color = GetDomainColor(domain);
        const QString labelText = QString::fromStdString(domain);

        auto* outline = new QGraphicsRectItem();
        outline->setPen(QPen(color, 2.0));

        QColor fillColor = color;
        fillColor.setAlpha(30);
        outline->setBrush(QBrush(fillColor, Qt::SolidPattern));
        outline->setZValue(-100.0);
        scene_->addItem(outline);

        auto* label = new QGraphicsTextItem(labelText);
        QFont font = label->font();
        font.setBold(true);
        font.setPointSize(10);
        label->setFont(font);
        label->setDefaultTextColor(color.darker(120));
        label->setZValue(-99.0);
        scene_->addItem(label);

        domainVisuals_[domain] = DomainVisuals{outline, label, color};
    }

    UpdateDomainOutlines();
}

void GraphScene::UpdateDomainOutlines() {
    constexpr double padding = 40.0;
    constexpr double labelOffset = 6.0;

    const auto groups = GroupNodesByDomain();
    for (const auto& [domain, nodeIds] : groups) {
        auto it = domainVisuals_.find(domain);
        if (it == domainVisuals_.end()) {
            continue;
        }

        double minX = std::numeric_limits<double>::max();
        double minY = std::numeric_limits<double>::max();
        double maxX = std::numeric_limits<double>::lowest();
        double maxY = std::numeric_limits<double>::lowest();

        for (const auto& nodeId : nodeIds) {
            const auto nodeIt = nodeIdMap_.find(nodeId);
            if (nodeIt == nodeIdMap_.end()) {
                continue;
            }

            QPointF pos;
            if (auto* graphicsObject = scene_->nodeGraphicsObject(nodeIt->second)) {
                pos = graphicsObject->pos();
            } else {
                const QVariant posVar = model_->nodeData(nodeIt->second, QtNodes::NodeRole::Position);
                pos = posVar.value<QPointF>();
            }

            minX = std::min(minX, pos.x());
            minY = std::min(minY, pos.y());
            maxX = std::max(maxX, pos.x());
            maxY = std::max(maxY, pos.y());
        }

        if (minX == std::numeric_limits<double>::max()) {
            continue;
        }

        // Use the largest cached node size in the group so the outline does not
        // clip captions or ports. Sizes are populated once when nodes are created.
        double nodeWidth = 200.0;
        double nodeHeight = 120.0;
        for (const auto& nodeId : nodeIds) {
            const auto nodeIt = nodeIdMap_.find(nodeId);
            if (nodeIt == nodeIdMap_.end()) {
                continue;
            }

            QSize size;
            const auto sizeIt = nodeSizeCache_.find(nodeIt->second);
            if (sizeIt != nodeSizeCache_.end()) {
                size = sizeIt->second;
            } else {
                size = scene_->nodeGeometry().size(nodeIt->second);
                nodeSizeCache_[nodeIt->second] = size;
            }

            nodeWidth = std::max(nodeWidth, static_cast<double>(size.width()));
            nodeHeight = std::max(nodeHeight, static_cast<double>(size.height()));
        }

        const QRectF bounds(minX - padding,
                            minY - padding,
                            maxX - minX + nodeWidth + padding * 2.0,
                            maxY - minY + nodeHeight + padding * 2.0);

        it->second.outline->setRect(bounds);
        it->second.label->setPos(bounds.left(), bounds.top() - it->second.label->boundingRect().height() - labelOffset);
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
        // the correct ports. If a node instance is currently being populated
        // (pendingNode_), its parameters are embedded into the delegate right
        // away so the scene picks up the parameter view when the node's
        // graphics object is constructed.
        registry_->registerModel([this, nodeType]() -> std::unique_ptr<QtNodes::NodeDelegateModel> {
            NodeAPI::Node dummy;
            dummy.id = nodeType.id;
            dummy.type = nodeType.id;
            auto model = std::make_unique<NodeInstanceModel>(std::move(dummy), nodeType);
            if (pendingNode_ && pendingNode_->type == nodeType.id) {
                model->SetParameters(pendingNode_->parameters);
            }
            return model;
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
        pendingNode_ = &node;
        const QtNodes::NodeId qtId = model_->addNode(modelName);
        pendingNode_ = nullptr;
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

        nodeSizeCache_[qtId] = scene_->nodeGeometry().size(qtId);

        // Cache the rendered node as a pixmap: panning and dragging then blit
        // the cache instead of re-running the (relatively expensive) node
        // painter for every node on every frame. The cache invalidates itself
        // on geometry/content changes and on zoom.
        if (auto* ngo = scene_->nodeGraphicsObject(qtId)) {
            ngo->setCacheMode(QGraphicsItem::DeviceCoordinateCache);
        }
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
