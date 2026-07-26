#include "ParameterBlock.h"

#include <QFont>
#include <QFontMetricsF>
#include <QPainter>

namespace NodeGUI {

namespace {

// Matches the look the embedded QLabel panel had: tinted rounded background,
// amber italic monospace text.
constexpr double kPaddingX = 6.0;
constexpr double kPaddingY = 3.0;
constexpr double kRowSpacing = 1.0;
constexpr double kRadius = 4.0;

const QFont& ParameterFont() {
    static const QFont font = [] {
        QFont f(QStringLiteral("monospace"));
        f.setStyleHint(QFont::TypeWriter);
        f.setItalic(true);
        return f;
    }();
    return font;
}

const QFontMetricsF& ParameterMetrics() {
    static const QFontMetricsF metrics(ParameterFont());
    return metrics;
}

double RowHeight() {
    return ParameterMetrics().height() + kRowSpacing;
}

}  // namespace

ParameterBlockData PrepareParameterBlock(const ParameterMap& parameters) {
    ParameterBlockData block;
    if (parameters.empty()) {
        return block;
    }

    double textWidth = 0.0;
    block.rows.reserve(parameters.size());
    for (const auto& [name, value] : parameters) {
        QStaticText row(QStringLiteral("%1: %2")
                            .arg(QString::fromStdString(name),
                                 QString::fromStdString(value)));
        // Lock the layout to the panel font so drawing never re-lays out.
        row.prepare(QTransform(), ParameterFont());
        textWidth = std::max(textWidth, row.size().width());
        block.rows.push_back(std::move(row));
    }

    const double height = static_cast<double>(block.rows.size()) * RowHeight()
                          - kRowSpacing + 2.0 * kPaddingY;
    block.size = QSizeF(textWidth + 2.0 * kPaddingX, height);
    return block;
}

QRectF ParameterBlockRect(const ParameterBlockData& block, const QSize& nodeSize) {
    if (block.size.isNull()) {
        return QRectF();
    }

    const double width = std::min(block.size.width(),
                                  static_cast<double>(nodeSize.width())
                                      - 2.0 * kParameterBlockMarginX);
    return QRectF(kParameterBlockMarginX,
                  nodeSize.height() - block.size.height() - kParameterBlockMarginBottom,
                  width,
                  block.size.height());
}

void PaintParameterBlock(QPainter* painter,
                         const ParameterBlockData& block,
                         const QSize& nodeSize) {
    const QRectF rect = ParameterBlockRect(block, nodeSize);
    if (rect.isNull()) {
        return;
    }

    painter->save();

    painter->setPen(QColor(255, 255, 255, 45));
    painter->setBrush(QColor(255, 255, 255, 14));
    painter->drawRoundedRect(rect, kRadius, kRadius);

    painter->setFont(ParameterFont());
    painter->setPen(QColor(0xe8, 0xc0, 0x7a));

    double y = rect.top() + kPaddingY;
    for (const QStaticText& row : block.rows) {
        painter->drawStaticText(QPointF(rect.left() + kPaddingX, y), row);
        y += RowHeight();
    }

    painter->restore();
}

}  // namespace NodeGUI
