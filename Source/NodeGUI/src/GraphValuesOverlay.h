#pragma once

#include <QColor>
#include <QGraphicsItem>
#include <QPointF>
#include <QRectF>
#include <QString>

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace QtNodes {
class BasicGraphicsScene;
}

namespace NodeGUI {

class GraphScene;

// LTspice-style sim probe chips on the node canvas (the "graph values" view
// mode). One GraphValueTagItem is a small rounded chip drawn just outside a
// port socket's edge on the wire side, carrying the telemetry channel bound
// to that socket: the channel name row, the latest value ('g',6, source =
// value-at-cursor while the playback cursor is pinned, the session-tail
// value otherwise), and a sparkline strip — the channel's waveform over the
// sim plot view window as a thin polyline in the wire-hue (port data-type
// color family) over a dark track, with a light cursor tick when a cursor
// is pinned. Clicking a chip assigns its channel onto the sim plot grid.
//
// Layering: GraphValuesOverlay owns one tag item per bound log node inside a
// QtNodes scene. Items sit at a single z-value above nodes/wires (the
// topmost content layer; nothing else paints above the tag layer except
// ephemeral drag feedback). Only tags whose payload actually changed repaint
// (SetText/SetName/SetSpark no-op on identical input), so a 4 Hz refresh
// costs nothing when the data holds still.
struct GraphValueSpark {
    double windowStart = 0.0;  // sim plot view window, data-time seconds
    double windowEnd = 0.0;
    std::vector<QPointF> points;  // (tsec, value), in-window, decimated
    double cursorFrac = -1.0;     // pinned cursor as window fraction, else -1
    QColor hue;                   // wire-hue ink (invalid -> style fallback)

    bool operator==(const GraphValueSpark& other) const {
        return windowStart == other.windowStart && windowEnd == other.windowEnd
               && points == other.points && cursorFrac == other.cursorFrac
               && hue == other.hue;
    }
    bool operator!=(const GraphValueSpark& other) const { return !(*this == other); }
};

class GraphValueTagItem : public QGraphicsItem {
public:
    GraphValueTagItem(const QPointF& anchor, bool wireToRight);

    // Dirty-gated updates: identical input leaves the item unrepainted.
    void SetName(const QString& name);
    void SetText(const QString& text);
    void SetSpark(const GraphValueSpark& spark);
    QString Name() const { return name_; }
    QString Text() const { return text_; }
    const GraphValueSpark& Spark() const { return spark_; }

    // "key:npoints,wmin,wmax[,cursor=frac]" — the sparkdump backing line.
    QString SparkDumpLine() const;

    // Fired on a single left click (chip probe-to-plot assign); the overlay
    // wires it at creation time.
    std::function<void(GraphValueTagItem*)> onClicked;

    QRectF boundingRect() const override;
    void paint(QPainter* painter,
               const QStyleOptionGraphicsItem* option,
               QWidget* widget) override;

protected:
    void mousePressEvent(QGraphicsSceneMouseEvent* event) override;

private:
    QRectF ComputePill() const;
    QColor ResolvedHue() const;

    QString name_;
    QString text_;
    GraphValueSpark spark_;
    bool wireRight_ = true;
    QRectF pill_;
};

// Owns and refills the tag items of ONE editor document's scene. Driven
// externally (the owner polls the sim cursor machinery); ApplyTags
// creates/removes items only when the binding set changes, and updates each
// item through the dirty gates.
//
// Scene lifetime: GraphScene::RebuildScene REPLACES the inner
// QtNodes::BasicGraphicsScene on every document load/new/undo-restore, so
// the overlay never caches the scene pointer — it resolves the live scene
// through the (stable) owning GraphScene on every access. Tag items added
// to a retired scene die with it; the overlay's item map is dropped
// (not deleted) when the swap is noticed.
class GraphValuesOverlay {
public:
    // Per-chip sparkline sample budget (decimated uniform-stride over the
    // sim plot view window).
    static constexpr int kMaxSparkPoints = 240;

    struct Tag {
        std::string itemId;  // stable id (log node id) — survives key edits
        std::string key;     // telemetry channel name (tooltip + dump)
        QPointF anchor;      // port socket, scene coordinates
        bool wireRight = true;  // chip extends this way off the socket edge
        QString text;        // formatted value
        // Sparkline payload: the anchor port's data-type id (resolved by the
        // overlay into the port-hue family color), the sim view window, the
        // decimated in-window samples, and the cursor fraction (-1 none).
        std::string portTypeId;
        double windowStart = 0.0;
        double windowEnd = 0.0;
        std::vector<QPointF> spark;
        double cursorFrac = -1.0;
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
    // One "key:npoints,wmin,wmax[,cursor=frac]" line per live tag, sorted by
    // key (smoke sparkdump backing).
    QString DumpSparkText() const;
    // Scene-space rect of one live tag item (bounding rect at its anchor);
    // invalid when the id has no item (smoke chip-click backing).
    QRectF ItemSceneRect(const std::string& itemId) const;

    // Single-click on a chip: its channel assigns onto the sim plot grid.
    // Wired by the owning MainWindow (the sim view lives out of scope here).
    std::function<void(const std::string& key)> onTagClicked;

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
