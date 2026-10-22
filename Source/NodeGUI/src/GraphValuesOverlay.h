#pragma once

#include <QGraphicsItem>
#include <QPointF>
#include <QRectF>
#include <QString>

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace QtNodes {
class BasicGraphicsScene;
}

namespace NodeGUI {

class GraphScene;

// Simulink-style sim value tags on the node canvas (the "graph values" view
// mode). One GraphValueTagItem is a small rounded pill drawn just outside a
// port socket's edge on the wire side, carrying the latest sim session value
// (value-at-cursor while the playback cursor is pinned, the session-tail
// value otherwise) of the telemetry channel bound to that socket. The pill
// is display-only: it accepts no mouse input and never touches the model.
//
// Layering: GraphValuesOverlay owns one tag item per bound log node inside a
// QtNodes scene. Items sit at a single z-value above nodes/wires (the
// topmost content layer; nothing else paints above the tag layer except
// ephemeral drag feedback). Only tags whose text actually changed repaint
// (SetText no-ops on identical text), so a 4 Hz refresh costs nothing when
// the values hold still.
class GraphValueTagItem : public QGraphicsItem {
public:
    GraphValueTagItem(const QPointF& anchor, bool wireToRight);

    // Dirty-gated text update: identical text leaves the item unrepainted.
    void SetText(const QString& text);
    QString Text() const { return text_; }

    QRectF boundingRect() const override;
    void paint(QPainter* painter,
               const QStyleOptionGraphicsItem* option,
               QWidget* widget) override;

private:
    QRectF ComputePill() const;

    QString text_;
    bool wireRight_ = true;
    QRectF pill_;
};

// Owns and refills the tag items of ONE editor document's scene. Driven
// externally (the owner polls the sim cursor machinery); ApplyTags
// creates/removes items only when the binding set changes, and updates each
// item through the dirty gate.
//
// Scene lifetime: GraphScene::RebuildScene REPLACES the inner
// QtNodes::BasicGraphicsScene on every document load/new/undo-restore, so
// the overlay never caches the scene pointer — it resolves the live scene
// through the (stable) owning GraphScene on every access. Tag items added
// to a retired scene die with it; the overlay's item map is dropped
// (not deleted) when the swap is noticed.
class GraphValuesOverlay {
public:
    struct Tag {
        std::string itemId;  // stable id (log node id) — survives key edits
        std::string key;     // telemetry channel name (tooltip + dump)
        QPointF anchor;      // port socket, scene coordinates
        bool wireRight = true;  // pill extends this way off the socket edge
        QString text;        // formatted value
    };

    explicit GraphValuesOverlay(GraphScene* owner);
    ~GraphValuesOverlay();

    GraphValuesOverlay(const GraphValuesOverlay&) = delete;
    GraphValuesOverlay& operator=(const GraphValuesOverlay&) = delete;

    // Replaces the tag set with the given one. Returns the live tag count.
    int ApplyTags(const std::vector<Tag>& tags);
    // Removes every tag item from the live scene.
    void Clear();
    int TagCount() const { return static_cast<int>(items_.size()); }

    // "key -> current text" pairs sorted by key (smoke dump backing).
    std::vector<std::pair<std::string, QString>> DumpPairs() const;

private:
    // The owner's current inner scene; on a swap (see the class comment)
    // the stale item map is dropped without deleting (their scene owns
    // them). Returns the live scene.
    QtNodes::BasicGraphicsScene* LiveScene();

    GraphScene* owner_;
    QtNodes::BasicGraphicsScene* lastScene_ = nullptr;
    std::map<std::string, GraphValueTagItem*> items_;  // itemId -> item
};

}  // namespace NodeGUI
