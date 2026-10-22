#pragma once

#include <NodeAPI/Graph.h>

#include <QtNodes/BasicGraphicsScene>
#include <QtNodes/NodeDelegateModelRegistry>

#include <nlohmann/json.hpp>

#include "NodeGraphModel.h"

#include <QString>
#include <QSize>

#include <functional>
#include <map>
#include <memory>
#include <set>
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
    // An optional default domain is assigned by AddNodeAt to freshly dropped
    // nodes that would otherwise start unassigned (used by the Plant Editor,
    // whose graphs run the single implicit domain 'plant'; the Node Editor
    // passes nothing and keeps the unassigned default).
    explicit GraphScene(QString defaultDomain = {});
    ~GraphScene();

    // Plant workflow: this document follows the plant pipeline's rules
    // (single implicit domain, no bridges, cycle-feedback legalized by the
    // plant scheduler). The Node Editor leaves this off.
    void SetPlantWorkflow(bool on);

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

    // Non-fatal schema-version warnings from the most recent LoadGraph /
    // LoadTemplates call (empty when everything matched this build).
    QString LoadWarning() const { return loadWarning_; }

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

    // ---- Manual wire routing (optional per-connection "waypoints") ----

    // QtNodes id of a live connection by its NodeAPI graph id (invalid when
    // unknown).
    QtNodes::ConnectionId FindQtConnectionId(const std::string& graphId) const;

    // Currently selected connections (both editors' wire gestures and the
    // route.* actions operate on exactly one selected wire).
    std::vector<QtNodes::ConnectionId> SelectedConnections() const;

    // Selects the connection with the given NodeAPI graph id (clears the rest
    // of the selection first). Returns false when it does not exist.
    bool SelectConnection(const std::string& graphId);

    // Copies of the connection's manual bends in scene coordinates.
    std::vector<QPointF> ConnectionWaypoints(QtNodes::ConnectionId qtId) const;

    // Replaces the connection's manual bends (empty = automatic routing),
    // repaints the wire, and records the change for undo. Returns an empty
    // string on success.
    QString SetConnectionWaypoints(QtNodes::ConnectionId qtId,
                                   const std::vector<QPointF>& scenePoints);

    // Action verbs behind the route.addPoint / route.removePoint verbs:
    // insert a bend beside the visually widest gap of the wire's current
    // route, or drop the connection's last manual bend. Error when the
    // connection is unknown; RemoveLastWireBend is a no-op on a wire without
    // bends.
    QString AddWireBend(QtNodes::ConnectionId qtId);
    QString RemoveLastWireBend(QtNodes::ConnectionId qtId);

    // ---- Display-only net labels (KiCad-style named wire stubs) ----

    // Copy of the connection's net label (empty when unset). The label never
    // leaves the GUI layer: NodeAPI/codegen/simulation see the plain
    // pairwise connection; a labeled wire just renders as two stubs + tag
    // chips instead of the long route.
    QString ConnectionLabel(QtNodes::ConnectionId qtId) const;

    // Sets/replaces (trimmed; empty clears) the connection's net label and
    // books the change for undo (same plumbing as waypoints). NON-EMPTY
    // labels are unique across ALL connections and bridges — a connection IS
    // the net, so two distinct connections may not claim one name. Refuses
    // with an error message when the name is taken or the connection is
    // unknown; empty string on success.
    QString SetConnectionLabel(QtNodes::ConnectionId qtId, const QString& label);

    // Smoke/checker read-out for one connection:
    // "label=<text|—>;layer=<z>;wp=[x,y;...]" (or an error line when
    // unknown; wp lists the manual bends in scene coordinates, "[]" when
    // the wire is auto-routed).
    QString DumpConnectionInfo(const std::string& graphId) const;

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

    // Sets a node's exclude-from-compile flag and greys/restores the node's
    // rendering to match. Returns an empty string on success.
    QString SetNodeExcluded(QtNodes::NodeId qtId, bool exclude);

    // Sets a node's exclude-from-compile-and-children flag: the node and its
    // whole downstream chain are excluded. Returns an empty string on success.
    QString SetNodeExcludedRecursive(QtNodes::NodeId qtId, bool exclude);

    // If the node is excluded because a recursive-flagged ancestor excludes
    // it, returns that ancestor's id; empty string otherwise.
    std::string ExclusionParent(const std::string& nodeId) const;

    // Copy the current scene positions back into the NodeAPI graph model.
    void SyncPositionsFromScene();

    // Grid-snap preference: node origins and wire bends snap to
    // WireRoute::kGridPitch multiples. Default on.
    void SetGridSnap(bool on);
    bool GridSnap() const { return gridSnap_; }

    // Programmatic translation that ends in the same completion path as a
    // user drag (snap + outlines + position sync + change notification).
    // Used by the offscreen smoke driver. Returns false for unknown nodes.
    bool TranslateNode(const std::string& nodeId, const QPointF& delta);
    // Smoke: emulate a REAL-mouse drag (move the graphics object directly,
    // bypassing the model exactly like a manual drag does, then the
    // release-side nodeClicked). Verifies the 2026-10-21 nodeClicked snap
    // hook through the chain that previously swallowed the completion.
    bool DragNodeAsUser(const std::string& nodeId, const QPointF& delta);
    // Real-drag evidence (fed by GraphView::onNodeDragProven at release):
    // the nodeClicked handler completes drags only for nodes marked proven,
    // so plain clicks don't snap off-grid nodes nor book spurious history
    // (coldboot dossier 2026-10-21: one plain click cost a move + 2 undos).
    void ReportDragProven(QtNodes::NodeId qtId);
    // Smoke-only: emit a plain-click release for a node (drag-gate check).
    bool PlainClickNode(const std::string& nodeId);

    // ---- Sim value tags ("graph values" view mode) -----------------------
    // One row per Debug.TelemetryLog node: the telemetry channel it publishes
    // (its Key parameter) and the scene-space anchor socket the channel value
    // ties to — the upstream producer's OUTPUT socket when the log input is
    // wired (the value flows on that wire), else the log node's own INPUT
    // port. Read-only geometry for the GraphValuesOverlay layer; nodes whose
    // anchor cannot be resolved (hidden/unknown port) are skipped rather
    // than guessed.
    struct TelemetryTagBinding {
        std::string nodeId;
        std::string key;
        QPointF scenePos;
        bool anchorIsOutput = false;
    };
    std::vector<TelemetryTagBinding> TelemetryTagBindings() const;

    // "<id>=x,y;<id>.<port>=x,y;..." — node origin and port socket scene
    // positions, rounded to 0.01; serves grid-compliance smoke assertions.
    QString DumpNodeGeometry(const std::string& nodeId) const;
    QPointF NodeCenterForSmoke(const std::string& nodeId) const;

private:
    // Applies the greyed (excluded) or normal style to a node.
    void ApplyNodeExcludedVisual(QtNodes::NodeId qtId, bool exclude);

    // Recomputes the effective exclusion set (direct flags + downstream
    // chains), refreshes node visuals for nodes whose state changed, and
    // updates the model's painter cache.
    void RefreshExclusionVisuals();

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

    // NodeAPI::SaveToJson output with the GUI-layer extras (non-schema top
    // keys, per-connection "waypoints") merged back in.
    std::string SerializeWithExtras() const;
    // Parses the optional per-connection "waypoints" arrays from a loaded
    // graph document into pendingWaypoints_ (tolerant: malformed entries are
    // silently skipped).
    void ParseWaypointsFromJson(const nlohmann::json& graphJson);
    // Hands pendingWaypoints_ to the model after a rebuild. Must run after
    // CreateConnections/CreateBridges (the qt ids exist only then).
    void ApplyPendingWaypoints();
    // Same staged parse/apply for the optional per-connection "netLabel"
    // string (tolerant: non-string entries are silently skipped).
    void ParseNetLabelsFromJson(const nlohmann::json& graphJson);
    void ApplyPendingNetLabels();
    // Repaints a wire after its waypoint list changed.
    void RepaintConnection(QtNodes::ConnectionId qtId);

    // The drag-complete path (shared by the QtNodes nodeMoved signal and the
    // programmatic smoke move): snap, outlines, sync, notifications.
    void OnNodeMoved(QtNodes::NodeId qtId);
    // Wire-follow settle: collects every node whose scene origin ran ahead
    // of the graph position (drags move graphics objects only; a selection
    // drag surfaces all its members even though just the grabbed node emits
    // the release signal) and translates the manual bends of the touched
    // connections through NodeGraphModel::FollowMovedNodes, repainting each
    // affected wire. Must run BEFORE SyncPositionsFromScene, which re-anchors
    // the graph positions to the scene. Drains no notification of its own —
    // the caller's single per-batch NotifyChanged covers positions + bends.
    void FollowWiresAfterMoves();
    void SnapNodeToGrid(QtNodes::NodeId qtId);
    void SnapAllNodesToGrid();
    // Refills the mini-router's per-connection route table (obstacle mask
    // from live node bounds, wire crossings counted in deterministic order).
    // Runs from NotifyChanged, i.e. after every drained graph mutation.
    void RecomputeAutoRoutes();

    NodeAPI::Graph graph_;
    std::shared_ptr<QtNodes::NodeDelegateModelRegistry> registry_;
    std::unique_ptr<NodeGraphModel> model_;
    std::unique_ptr<QtNodes::BasicGraphicsScene> scene_;
    std::string templatesDir_;
    QString loadWarning_;

    // Map NodeAPI node id -> QtNodes NodeId.
    std::map<std::string, QtNodes::NodeId> nodeIdMap_;

    // Node instance currently being populated by CreateNodes(). Read by the
    // registry creators so the delegate model can embed the node's parameter
    // view before the scene constructs its graphics object.
    const NodeAPI::Node* pendingNode_ = nullptr;

    // Cached node geometry sizes so domain-outline updates don't re-query the
    // full geometry system on every mouse-move event.
    std::unordered_map<QtNodes::NodeId, QSize> nodeSizeCache_;

    // Effective exclusion state from the last RefreshExclusionVisuals() pass.
    std::set<std::string> effectiveExcluded_;
    std::map<std::string, std::string> exclusionParents_;

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

    // Optional default domain assigned by AddNodeAt (see the constructor).
    QString defaultDomain_;

    bool gridSnap_ = true;

    // Nodes proved as drags at release (see ReportDragProven's comment);
    // the scene node's release handler consumes the mark per node.
    std::set<QtNodes::NodeId> dragProven_;

    // Non-schema top-level keys of the loaded source document, stashed at
    // LoadGraph for the save-time merge (description etc.; the NodeAPI JSON
    // surface does not model them).
    std::unordered_map<std::string, nlohmann::json> originalTopKeys_;

    // Manual wire bends parsed from the loaded document, keyed by NodeAPI
    // connection id; staged until the scene exists (ApplyPendingWaypoints).
    std::unordered_map<std::string, std::vector<QPointF>> pendingWaypoints_;

    // Net labels parsed from the loaded document (same staging as bends,
    // ApplyPendingNetLabels).
    std::unordered_map<std::string, QString> pendingNetLabels_;

    bool PointHitsGraphContent(const QPointF& scenePos) const;
    void UpdateDomainSelectionStyle();
};

}  // namespace NodeGUI
