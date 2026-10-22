#include "GraphValuesOverlay.h"

#include "GraphScene.h"
#include "PortStyle.h"

#include <QtNodes/BasicGraphicsScene>
#include <QtNodes/StyleCollection>

#include <QApplication>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGraphicsSceneMouseEvent>
#include <QPainter>
#include <QPalette>

#include <algorithm>
#include <cmath>
#include <set>

namespace NodeGUI {

namespace {

// Scene layer for the value tags: above unselected (0) and selected (1)
// nodes and every wire (-1)/domain-outline (-100) layer — the topmost
// content layer of the canvas.
constexpr double kTagZValue = 2.5;
constexpr double kGap = 6.0;    // socket edge -> chip edge (scene units)
constexpr double kPadX = 5.0;
constexpr double kPadY = 1.5;
constexpr double kRadius = 3.0;
constexpr double kRowGap = 2.0;
constexpr double kSparkW = 64.0;   // minimum strip width (scene units)
constexpr double kSparkH = 16.0;   // strip height
constexpr double kSparkPadY = 2.0; // waveform inset inside the strip

// The mono/hud face used by the run-log and events fields.
QFont TagFont() {
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSizeF(8.5);
    return font;
}

// The wire-hue family for a port data type — the same pair of lookups
// BridgeConnectionPainter::ConnectionColor chains through (port data-type
// color, then the connection style's normal color).
QColor HueForPortType(const std::string& typeId) {
    if (!typeId.empty()) {
        if (const auto color = ParsePortColor(QString::fromStdString(typeId))) {
            return *color;
        }
    }
    return QtNodes::StyleCollection::connectionStyle().normalColor();
}

}  // namespace

GraphValueTagItem::GraphValueTagItem(const QPointF& anchor, bool wireToRight)
    : wireRight_(wireToRight) {
    setPos(anchor);
    setZValue(kTagZValue);
    setFlag(QGraphicsItem::ItemIsSelectable, false);
    setFlag(QGraphicsItem::ItemIsMovable, false);
    // Left button only: single click probes the channel onto the sim plot
    // grid. Everything else (context/drag gestures) falls through.
    setAcceptedMouseButtons(Qt::LeftButton);
    pill_ = ComputePill();
}

void GraphValueTagItem::SetName(const QString& name) {
    if (name_ == name) {
        return;
    }
    prepareGeometryChange();
    name_ = name;
    pill_ = ComputePill();
    update();
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

void GraphValueTagItem::SetSpark(const GraphValueSpark& spark) {
    if (spark_ == spark) {
        return;  // the strip carries its own dirty gate
    }
    spark_ = spark;
    update();
}

QString GraphValueTagItem::SparkDumpLine() const {
    QString line = QStringLiteral("%1:%2,%3,%4")
                       .arg(name_)
                       .arg(spark_.points.size())
                       .arg(spark_.windowStart, 0, 'f', 4)
                       .arg(spark_.windowEnd, 0, 'f', 4);
    if (spark_.cursorFrac >= 0.0) {
        line += QStringLiteral(",cursor=%1").arg(spark_.cursorFrac, 0, 'f', 4);
    }
    return line;
}

QRectF GraphValueTagItem::ComputePill() const {
    const QFontMetricsF metrics(TagFont());
    const double contentWidth =
        std::max({metrics.horizontalAdvance(name_),
                  metrics.horizontalAdvance(text_), kSparkW});
    const double contentHeight =
        2.0 * metrics.height() + 2.0 * kRowGap + kSparkH;
    const double width = contentWidth + 2.0 * kPadX;
    const double height = contentHeight + 2.0 * kPadY;
    // Just outside the port's edge on the wire side: output sockets face
    // right (chip extends +x), input sockets face left (chip extends -x).
    const double x = wireRight_ ? kGap : -kGap - width;
    return QRectF(x, -height / 2.0, width, height);
}

QRectF GraphValueTagItem::boundingRect() const {
    return pill_.adjusted(-1.0, -1.0, 1.0, 1.0);
}

QColor GraphValueTagItem::ResolvedHue() const {
    if (spark_.hue.isValid()) {
        return spark_.hue;
    }
    return QtNodes::StyleCollection::connectionStyle().normalColor();
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
    const QFontMetricsF metrics(painter->font());
    const double rowH = metrics.height();
    const double innerX = pill_.left() + kPadX;
    const double innerW = pill_.width() - 2.0 * kPadX;
    const QRectF nameRect(innerX, pill_.top() + kPadY, innerW, rowH);
    const QRectF valueRect(innerX, nameRect.bottom() + kRowGap, innerW, rowH);
    // Channel name in the probe hue (LTspice paints the probe label in the
    // trace's color), the numeric value in the plain text color.
    painter->setPen(ResolvedHue());
    painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, name_);
    painter->setPen(palette.color(QPalette::Text));
    painter->drawText(valueRect, Qt::AlignLeft | Qt::AlignVCenter, text_);

    // Sparkline strip: palette-aware dark track (deepen a dark base, pull a
    // light one down) under the wire-hue polyline.
    const QRectF strip(innerX, valueRect.bottom() + kRowGap, innerW, kSparkH);
    QColor track = palette.color(QPalette::Base).darker(
        palette.color(QPalette::Base).lightness() > 120 ? 140 : 60);
    track.setAlpha(255);
    painter->setPen(QPen(palette.color(QPalette::Dark), 0));
    painter->setBrush(track);
    painter->drawRoundedRect(strip, 1.5, 1.5);

    if (spark_.points.size() >= 2 && spark_.windowEnd > spark_.windowStart) {
        const auto yBounds = std::minmax_element(
            spark_.points.begin(), spark_.points.end(),
            [](const QPointF& a, const QPointF& b) { return a.y() < b.y(); });
        double yMin = yBounds.first->y();
        double yMax = yBounds.second->y();
        if (!(yMax > yMin)) {
            // A flat channel still gets a readable centered line.
            const double pad = std::max(1.0, std::abs(yMin) * 0.5);
            yMin -= pad;
            yMax += pad;
        }
        const double spanX = spark_.windowEnd - spark_.windowStart;
        const double innerH = strip.height() - 2.0 * kSparkPadY;
        QPolygonF wave;
        wave.reserve(static_cast<int>(spark_.points.size()));
        for (const QPointF& point : spark_.points) {
            const double fx =
                std::clamp((point.x() - spark_.windowStart) / spanX, 0.0, 1.0);
            const double fy = (point.y() - yMin) / (yMax - yMin);
            wave << QPointF(strip.left() + fx * strip.width(),
                            strip.top() + kSparkPadY + (1.0 - fy) * innerH);
        }
        painter->setPen(QPen(ResolvedHue(), 0));
        painter->drawPolyline(wave);
    }

    // Pinned cursor: a light tick at the same window fraction as the sim
    // plots' shared cursor.
    if (spark_.cursorFrac >= 0.0 && spark_.cursorFrac <= 1.0) {
        const double x = strip.left() + spark_.cursorFrac * strip.width();
        const QColor tick = palette.color(QPalette::Light);
        painter->setPen(QPen(tick.isValid() ? tick : ResolvedHue().lighter(150), 0));
        painter->drawLine(QPointF(x, strip.top()), QPointF(x, strip.bottom()));
    }
}

void GraphValueTagItem::mousePressEvent(QGraphicsSceneMouseEvent* event) {
    if (event->button() == Qt::LeftButton && onClicked) {
        onClicked(this);
        event->accept();
        return;
    }
    QGraphicsItem::mousePressEvent(event);
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
        GraphValueSpark spark;
        spark.windowStart = tag.windowStart;
        spark.windowEnd = tag.windowEnd;
        spark.points = tag.spark;
        spark.cursorFrac = tag.cursorFrac;
        spark.hue = HueForPortType(tag.portTypeId);
        const auto it = items_.find(tag.itemId);
        if (it == items_.end()) {
            auto* item = new GraphValueTagItem(tag.anchor, tag.wireRight);
            item->onClicked = [this](GraphValueTagItem* clicked) {
                if (onTagClicked) {
                    onTagClicked(clicked->Name().toStdString());
                }
            };
            item->SetName(QString::fromStdString(tag.key));
            item->setToolTip(QString::fromStdString(tag.key));
            item->SetText(tag.text);
            item->SetSpark(spark);
            scene->addItem(item);
            items_.emplace(tag.itemId, item);
            continue;
        }
        GraphValueTagItem* item = it->second;
        if (item->pos() != tag.anchor) {
            item->setPos(tag.anchor);
        }
        const QString key = QString::fromStdString(tag.key);
        item->SetName(key);
        if (item->toolTip() != key) {
            item->setToolTip(key);
        }
        item->SetText(tag.text);
        item->SetSpark(spark);
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

QString GraphValuesOverlay::DumpSparkText() const {
    std::vector<QString> lines;
    lines.reserve(items_.size());
    for (const auto& [itemId, item] : items_) {
        lines.push_back(item->SparkDumpLine());
    }
    // The line starts with the channel key, so a plain sort is a key sort.
    std::sort(lines.begin(), lines.end());
    return QStringList(lines.begin(), lines.end()).join(QStringLiteral("\n"));
}

QRectF GraphValuesOverlay::ItemSceneRect(const std::string& itemId) const {
    const auto it = items_.find(itemId);
    if (it == items_.end()) {
        return {};
    }
    return it->second->boundingRect().translated(it->second->pos());
}

}  // namespace NodeGUI
