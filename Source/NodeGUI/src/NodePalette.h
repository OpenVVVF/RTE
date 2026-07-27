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

protected:
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override;

private:
    // Category prefix of a type id: text before the first dot, or "Other".
    static QString CategoryOf(const std::string& typeId);
};

}  // namespace NodeGUI
