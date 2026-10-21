#include "NodePalette.h"

#include <QKeyEvent>
#include <QMimeData>
#include <QTreeWidgetItem>

namespace NodeGUI {

NodePalette::NodePalette(QWidget* parent)
    : QTreeWidget(parent) {
    setHeaderHidden(true);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);

    // Activation beside drag: double-click a type entry to instantiate it.
    // Deliberately NOT itemActivated - on some platforms that fires on single
    // click, and a drag out of the palette would then add a stray node.
    // Carries the same type id the drag's mimeData does, so one host seam
    // (GraphView::onNodeTypeDropped) serves both gestures.
    connect(this, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item, int column) {
        (void)column;
        ActivateItem(item);
    });
}

QString NodePalette::CategoryOf(const std::string& typeId) {
    const auto dot = typeId.find('.');
    if (dot == std::string::npos || dot == 0) {
        return QStringLiteral("Other");
    }
    return QString::fromStdString(typeId.substr(0, dot));
}

void NodePalette::SetNodeTypes(const std::vector<NodeAPI::NodeType>& nodeTypes) {
    clear();

    std::map<QString, QTreeWidgetItem*> categories;
    for (const auto& nodeType : nodeTypes) {
        const QString category = CategoryOf(nodeType.id);
        auto& parent = categories[category];
        if (!parent) {
            parent = new QTreeWidgetItem(this, {category});
            // Categories are pure grouping headers: not selectable, not
            // draggable.
            parent->setFlags(Qt::ItemIsEnabled);
        }

        const QString display = nodeType.displayName.empty()
                                    ? QString::fromStdString(nodeType.id)
                                    : QString::fromStdString(nodeType.displayName);
        auto* item = new QTreeWidgetItem(parent, {display});
        item->setData(0, Qt::UserRole, QString::fromStdString(nodeType.id));
        QString toolTip =
            QStringLiteral("<b>%1</b>").arg(display.toHtmlEscaped());
        if (!nodeType.description.empty()) {
            toolTip += QStringLiteral("<br>%1")
                           .arg(QString::fromStdString(nodeType.description)
                                    .toHtmlEscaped());
        }
        toolTip += QStringLiteral("<br><span style=\"color:#999\">%1</span>")
                       .arg(QString::fromStdString(nodeType.id).toHtmlEscaped());
        item->setToolTip(0, toolTip);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
    }

    // Sort categories and the entries within each.
    sortItems(0, Qt::AscendingOrder);
    for (const auto& [name, parent] : categories) {
        parent->sortChildren(0, Qt::AscendingOrder);
    }
    expandAll();
}

QMimeData* NodePalette::mimeData(const QList<QTreeWidgetItem*>& items) const {
    auto* mime = new QMimeData;
    if (!items.isEmpty()) {
        mime->setData(kNodeTypeMimeFormat,
                      items.first()->data(0, Qt::UserRole).toString().toUtf8());
    }
    return mime;
}

void NodePalette::keyPressEvent(QKeyEvent* event) {
    // Enter/Return is the keyboard twin of the double-click: instantiate the
    // current type entry without reaching for the mouse.
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (QTreeWidgetItem* item = currentItem()) {
            ActivateItem(item);
            if (const QString typeId = item->data(0, Qt::UserRole).toString();
                !typeId.isEmpty()) {
                event->accept();
                return;
            }
        }
    }
    QTreeWidget::keyPressEvent(event);
}

void NodePalette::ActivateItem(const QTreeWidgetItem* item) {
    if (!item) {
        return;
    }
    const QString typeId = item->data(0, Qt::UserRole).toString();
    if (!typeId.isEmpty()) {
        Q_EMIT nodeTypeActivated(typeId);
    }
}

}  // namespace NodeGUI
