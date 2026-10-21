#pragma once

#include <NodeAPI/NodeType.h>

#include <QTreeWidget>

#include <map>
#include <vector>

namespace NodeGUI {

// Mime format carried while dragging a node type out of the palette.
inline constexpr char kNodeTypeMimeFormat[] = "application/x-nodegui-nodetype";

// Side-panel tree of available node types, grouped by the category prefix of
// the type id (e.g. "hw", "math", "config"). Entries can be dragged onto the
// graph view to instantiate the type.
class NodePalette : public QTreeWidget {
    Q_OBJECT

public:
    explicit NodePalette(QWidget* parent = nullptr);

    // Rebuilds the tree from the graph's node-type database.
    void SetNodeTypes(const std::vector<NodeAPI::NodeType>& nodeTypes);

    // Substring filter (case-insensitive, display + id): leaves not matching
    // hide along with any category that has no visible children left.
    // Empty filter restores everything. The host provides the edit widget.
    void SetFilter(const QString& substring);

    // Category prefix of a type id: text before the first dot, or "Other".
    // Also used by GraphView's empty-canvas "Add node here" menu so the menu
    // grouping always matches the palette's family taxonomy.
    static QString CategoryOf(const std::string& typeId);

Q_SIGNALS:
    // Double-click (or Enter) on a type entry asks the host to instantiate
    // it - the keyboard/pointer activation path beside drag & drop. Carries
    // the same type id the drag mimeData does (kNodeTypeMimeFormat payload).
    void nodeTypeActivated(const QString& typeId);

protected:
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    // Emits nodeTypeActivated for an activated type entry (leaf item with a
    // UserRole type id); category headers carry no id and are ignored.
    void ActivateItem(const QTreeWidgetItem* item);

    QString filterText_;
};

// Host widget pairing a filter line edit (top) with a NodePalette (bottom);
// the palette docks receive this wrapper, and the palette reference stays
// addressable for SetNodeTypes / activation connects.
class NodePaletteHost : public QWidget {
public:
    explicit NodePaletteHost(NodePalette* palette, QWidget* parent);
    NodePalette* Palette() const { return palette_; }

private:
    NodePalette* palette_ = nullptr;
};

}  // namespace NodeGUI
