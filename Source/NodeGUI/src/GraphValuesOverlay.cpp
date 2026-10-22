#include "GraphValuesOverlay.h"

#include "GraphScene.h"

#include <QtNodes/BasicGraphicsScene>

#include <QApplication>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QPainter>
#include <QPalette>

#include <algorithm>
#include <set>

namespace NodeGUI {

namespace {

// Scene layer for the value tags: above unselected (0) and selected (1)
// nodes and every wire (-1)/domain-outline (-100) layer — the topmost
// content layer of the canvas.
constexpr double kTagZValue = 2.5;
constexpr double kGap = 6.0;    // socket edge -> pill edge (scene units)
constexpr double kPadX = 5.0;
constexpr double kPadY = 1.5;
constexpr double kRadius = 3.0;

// The mono/hud face used by the run-log and events fields.
QFont TagFont() {
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSizeF(8.5);
    return font;
}

}  // namespace

GraphValueTagItem::GraphValueTagItem(const QPointF& anchor, bool wireToRight)
    : wireRight_(wireToRight) {
    setPos(anchor);
    setZValue(kTagZValue);
    setFlag(QGraphicsItem::ItemIsSelectable, false);
    setFlag(QGraphicsItem::ItemIsMovable, false);
    setAcceptedMouseButtons(Qt::NoButton);
    pill_ = ComputePill();
}

void GraphValueTagItem::SetText(const QString& text) {
    if (text_ == text) {
        return;  // only dirty tags repaint
    }
    prepareGeometryChange();
    text_ = text;
    pill_ = ComputePill();
    update();
}

QRectF GraphValueTagItem::ComputePill() const {
    const QFontMetricsF metrics(TagFont());
    const double width = metrics.horizontalAdvance(text_) + 2.0 * kPadX;
    const double height = metrics.height() + 2.0 * kPadY;
    // Just outside the port's edge on the wire side: output sockets face
    // right (pill extends +x), input sockets face left (pill extends -x).
    const double x = wireRight_ ? kGap : -kGap - width;
    return QRectF(x, -height / 2.0, width, height);
}

QRectF GraphValueTagItem::boundingRect() const {
    return pill_.adjusted(-1.0, -1.0, 1.0, 1.0);
}

void GraphValueTagItem::paint(QPainter* painter,
                              const QStyleOptionGraphicsItem* /*option*/,
                              QWidget* /*widget*/) {
    const QPalette palette = QApplication::palette();
    QColor fill = palette.color(QPalette::Base);
    fill.setAlpha(232);
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(QPen(palette.color(QPalette::Mid), 0));
    painter->setBrush(fill);
    painter->drawRoundedRect(pill_, kRadius, kRadius);
    painter->setFont(TagFont());
    painter->setPen(palette.color(QPalette::Text));
    painter->drawText(pill_, Qt::AlignCenter, text_);
}

GraphValuesOverlay::GraphValuesOverlay(GraphScene* owner) : owner_(owner) {}

GraphValuesOverlay::~GraphValuesOverlay() {
    Clear();
}

QtNodes::BasicGraphicsScene* GraphValuesOverlay::LiveScene() {
    QtNodes::BasicGraphicsScene* live = owner_ ? owner_->Scene() : nullptr;
    if (live != lastScene_) {
        // The inner scene was rebuilt (document load/new/undo-restore):
        // tag items added to the retired scene died with it — drop the map
        // WITHOUT deleting, then attach fresh below.
        items_.clear();
        lastScene_ = live;
    }
    return live;
}

int GraphValuesOverlay::ApplyTags(const std::vector<Tag>& tags) {
    QtNodes::BasicGraphicsScene* scene = LiveScene();
    if (!scene) {
        return 0;
    }
    std::set<std::string> keep;
    for (const Tag& tag : tags) {
        keep.insert(tag.itemId);
        const auto it = items_.find(tag.itemId);
        if (it == items_.end()) {
            auto* item = new GraphValueTagItem(tag.anchor, tag.wireRight);
            item->setToolTip(QString::fromStdString(tag.key));
            item->SetText(tag.text);
            scene->addItem(item);
            items_.emplace(tag.itemId, item);
            continue;
        }
        GraphValueTagItem* item = it->second;
        if (item->pos() != tag.anchor) {
            item->setPos(tag.anchor);
        }
        const QString tooltip = QString::fromStdString(tag.key);
        if (item->toolTip() != tooltip) {
            item->setToolTip(tooltip);
        }
        item->SetText(tag.text);
    }
    for (auto it = items_.begin(); it != items_.end();) {
        if (keep.count(it->first)) {
            ++it;
            continue;
        }
        scene->removeItem(it->second);
        delete it->second;
        it = items_.erase(it);
    }
    return static_cast<int>(items_.size());
}

void GraphValuesOverlay::Clear() {
    QtNodes::BasicGraphicsScene* scene = LiveScene();
    for (auto& [itemId, item] : items_) {
        if (scene) {
            scene->removeItem(item);
        }
        delete item;
    }
    items_.clear();
}

std::vector<std::pair<std::string, QString>> GraphValuesOverlay::DumpPairs() const {
    std::vector<std::pair<std::string, QString>> pairs;
    pairs.reserve(items_.size());
    for (const auto& [itemId, item] : items_) {
        pairs.emplace_back(item->toolTip().toStdString(), item->Text());
    }
    std::sort(pairs.begin(), pairs.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return pairs;
}

}  // namespace NodeGUI
