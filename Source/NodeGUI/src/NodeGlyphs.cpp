#include "NodeGlyphs.h"

#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QRectF>

#include <cmath>
#include <functional>
#include <unordered_map>
#include <vector>

namespace NodeGUI {

namespace {

// All glyphs draw inside the square `r` in proportional coordinates; the
// stroke color always comes from the node style (theme) via GlyphColors.

QPen GlyphPen(const GlyphColors& c, double width) {
    QPen pen(c.line);
    pen.setWidthF(width);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    return pen;
}

QFont GlyphFont(double pixels, bool bold = false) {
    QFont font;
    font.setPixelSize(std::max(6.0, pixels));
    font.setBold(bold);
    return font;
}

// Fractional point inside rect r (0..1, 0..1).
QPointF At(const QRectF& r, double fx, double fy) {
    return QPointF(r.left() + fx * r.width(), r.top() + fy * r.height());
}

QRectF Sub(const QRectF& r, double fx, double fy, double fw, double fh) {
    return QRectF(r.left() + fx * r.width(),
                  r.top() + fy * r.height(),
                  fw * r.width(),
                  fh * r.height());
}

void DrawText(QPainter* p, const QRectF& r, const QString& text, double size) {
    p->save();
    p->setFont(GlyphFont(r.height() * size, true));
    p->drawText(r, Qt::AlignCenter, text);
    p->restore();
}

// PMSM-style machine: circle with "M" + "3~", three phase terminals on the
// left, a shaft stub with coupling on the right.
void DrawMotorGlyph(QPainter* p, const QRectF& r, const GlyphColors& c) {
    p->setPen(GlyphPen(c, 1.6));
    p->setBrush(c.fill);
    p->drawEllipse(Sub(r, 0.16, 0.12, 0.68, 0.68));

    // three phase terminals entering from the left
    for (const double fy : {0.30, 0.46, 0.62}) {
        p->drawLine(At(r, 0.0, fy), At(r, 0.16, fy));
    }
    // shaft stub + coupling
    p->drawLine(At(r, 0.84, 0.46), At(r, 1.0, 0.46));
    p->setBrush(QBrush(c.line));
    p->drawRect(Sub(r, 0.88, 0.40, 0.10, 0.12));

    DrawText(p, Sub(r, 0.16, 0.16, 0.68, 0.38), QStringLiteral("M"), 0.34);
    DrawText(p, Sub(r, 0.16, 0.50, 0.68, 0.24), QStringLiteral("3~"), 0.20);
}

// 3-phase inverter bridge: dc rails +/-, three switch legs, phase taps.
void DrawBridgeGlyph(QPainter* p, const QRectF& r, const GlyphColors& c) {
    p->setPen(GlyphPen(c, 1.6));
    p->setBrush(Qt::NoBrush);
    p->drawLine(At(r, 0.05, 0.18), At(r, 0.82, 0.18));  // dc+ rail
    p->drawLine(At(r, 0.05, 0.80), At(r, 0.82, 0.80));  // dc- rail

    DrawText(p, Sub(r, 0.0, 0.0, 0.16, 0.14), QStringLiteral("+"), 0.20);
    DrawText(p, Sub(r, 0.0, 0.84, 0.16, 0.14), QStringLiteral("-"), 0.20);

    for (const double fx : {0.26, 0.47, 0.68}) {
        p->drawLine(At(r, fx, 0.18), At(r, fx, 0.80));  // leg
        // two switches per leg
        p->setBrush(c.fill);
        p->drawRect(Sub(r, fx - 0.045, 0.28, 0.09, 0.09));
        p->drawRect(Sub(r, fx - 0.045, 0.61, 0.09, 0.09));
        p->setBrush(Qt::NoBrush);
        // phase tap from the leg midpoint to the right
        p->drawLine(At(r, fx, 0.49), At(r, 0.82, 0.49));
    }
    p->drawLine(At(r, 0.82, 0.18), At(r, 0.82, 0.80));
    p->drawLine(At(r, 0.82, 0.49), At(r, 1.0, 0.49));
}

// DC link supply: double rail with polarity marks, ground symbol below.
void DrawDcBusGlyph(QPainter* p, const QRectF& r, const GlyphColors& c) {
    p->setPen(GlyphPen(c, 2.0));
    p->setBrush(Qt::NoBrush);
    p->drawLine(At(r, 0.08, 0.30), At(r, 0.92, 0.30));
    p->drawLine(At(r, 0.08, 0.62), At(r, 0.92, 0.62));

    DrawText(p, Sub(r, 0.0, 0.08, 0.18, 0.18), QStringLiteral("+"), 0.22);
    DrawText(p, Sub(r, 0.0, 0.66, 0.18, 0.18),
             QStringLiteral("\u2212"),  // minus sign
             0.22);

    // ground symbol hanging off the lower rail
    p->setPen(GlyphPen(c, 1.5));
    p->drawLine(At(r, 0.50, 0.62), At(r, 0.50, 0.76));
    p->drawLine(At(r, 0.38, 0.76), At(r, 0.62, 0.76));
    p->drawLine(At(r, 0.42, 0.83), At(r, 0.58, 0.83));
    p->drawLine(At(r, 0.46, 0.90), At(r, 0.54, 0.90));
}

// Thermal block: heatsink outline (finned plate).
void DrawHeatsinkGlyph(QPainter* p, const QRectF& r, const GlyphColors& c) {
    p->setPen(GlyphPen(c, 1.6));
    p->setBrush(c.fill);
    p->drawRoundedRect(Sub(r, 0.08, 0.66, 0.84, 0.22), 3.0, 3.0);

    p->setBrush(Qt::NoBrush);
    for (const double fx : {0.16, 0.31, 0.46, 0.61, 0.76, 0.91}) {
        p->drawLine(At(r, fx, 0.66), At(r, fx, 0.18));
    }
}

// Hall current sensor: toroid ring with a gap, conductor + arrow through it.
void DrawHallGlyph(QPainter* p, const QRectF& r, const GlyphColors& c) {
    p->setPen(GlyphPen(c, 1.6));
    p->setBrush(Qt::NoBrush);
    const QRectF ring = Sub(r, 0.14, 0.12, 0.68, 0.68);
    p->drawArc(ring, 50 * 16, 260 * 16);  // gap at the lower right

    // conductor through the ring, arrowhead up-right
    p->drawLine(At(r, 0.26, 0.80), At(r, 0.74, 0.16));
    p->drawLine(At(r, 0.74, 0.16), At(r, 0.56, 0.20));
    p->drawLine(At(r, 0.74, 0.16), At(r, 0.70, 0.36));

    // sensor die at the ring gap
    p->setBrush(QBrush(c.line));
    p->drawEllipse(Sub(r, 0.70, 0.64, 0.12, 0.12));
}

// Shaft block (dyno / free inertia): housing on the shaft, bearing pedestals.
void DrawShaftGlyph(QPainter* p, const QRectF& r, const GlyphColors& c) {
    p->setPen(GlyphPen(c, 1.8));
    p->setBrush(Qt::NoBrush);
    p->drawLine(At(r, 0.0, 0.48), At(r, 1.0, 0.48));  // shaft

    p->setPen(GlyphPen(c, 1.5));
    p->setBrush(c.fill);
    p->drawRect(Sub(r, 0.28, 0.30, 0.44, 0.36));  // dyno housing

    // bearing pedestals left/right of the housing
    p->setBrush(Qt::NoBrush);
    for (const double fx : {0.14, 0.86}) {
        p->drawLine(At(r, fx, 0.48), At(r, fx, 0.74));
        p->drawLine(At(r, fx - 0.08, 0.74), At(r, fx + 0.08, 0.74));
    }
}

// SinCos encoder: disc with radial ticks and a center hub.
void DrawEncoderGlyph(QPainter* p, const QRectF& r, const GlyphColors& c) {
    p->setPen(GlyphPen(c, 1.6));
    p->setBrush(c.fill);
    p->drawEllipse(Sub(r, 0.12, 0.10, 0.76, 0.76));

    const QPointF center = At(r, 0.50, 0.48);
    p->setBrush(Qt::NoBrush);
    for (int k = 0; k < 12; ++k) {
        const double angle = k * M_PI / 6.0;
        const double rx = r.width() * 0.5;
        const QPointF dir(std::cos(angle), std::sin(angle));
        p->drawLine(center + dir * (0.66 * rx), center + dir * (0.78 * rx));
    }
    p->setBrush(QBrush(c.line));
    p->drawEllipse(center, r.width() * 0.05, r.width() * 0.05);
}

// Voltage sense: meter circle with "V" and through-leads.
void DrawVoltMeterGlyph(QPainter* p, const QRectF& r, const GlyphColors& c) {
    p->setPen(GlyphPen(c, 1.6));
    p->setBrush(Qt::NoBrush);
    p->drawLine(At(r, 0.0, 0.46), At(r, 0.22, 0.46));
    p->drawLine(At(r, 0.78, 0.46), At(r, 1.0, 0.46));
    p->setBrush(c.fill);
    p->drawEllipse(Sub(r, 0.22, 0.14, 0.56, 0.64));
    DrawText(p, Sub(r, 0.22, 0.14, 0.56, 0.64), QStringLiteral("V"), 0.40);
}

// DcDc converter family: coupled windings with a core (transformer glyph).
void DrawTransformerGlyph(QPainter* p, const QRectF& r, const GlyphColors& c) {
    p->setPen(GlyphPen(c, 1.5));
    p->setBrush(Qt::NoBrush);

    // primary / secondary coil humps
    for (const double fy : {0.22, 0.40, 0.58}) {
        p->drawArc(Sub(r, 0.24, fy, 0.16, 0.16), -90 * 16, 180 * 16);
        p->drawArc(Sub(r, 0.60, fy, 0.16, 0.16), 90 * 16, 180 * 16);
    }
    // core bars
    p->drawLine(At(r, 0.47, 0.16), At(r, 0.47, 0.82));
    p->drawLine(At(r, 0.53, 0.16), At(r, 0.53, 0.82));

    // leads
    p->drawLine(At(r, 0.0, 0.30), At(r, 0.24, 0.30));
    p->drawLine(At(r, 0.0, 0.66), At(r, 0.24, 0.66));
    p->drawLine(At(r, 0.76, 0.30), At(r, 1.0, 0.30));
    p->drawLine(At(r, 0.76, 0.66), At(r, 1.0, 0.66));
}

// PI(P) controller: error junction feeding a controller block.
void DrawPiGlyph(QPainter* p, const QRectF& r, const GlyphColors& c) {
    p->setPen(GlyphPen(c, 1.5));
    p->setBrush(Qt::NoBrush);

    // summing junction
    p->setBrush(c.fill);
    p->drawEllipse(Sub(r, 0.06, 0.40, 0.16, 0.16));
    p->setBrush(Qt::NoBrush);
    p->drawLine(At(r, 0.0, 0.48), At(r, 0.06, 0.48));    // setpoint lead
    p->drawLine(At(r, 0.14, 0.60), At(r, 0.14, 0.56));   // feedback lead (from below)
    DrawText(p, Sub(r, 0.0, 0.24, 0.12, 0.12), QStringLiteral("+"), 0.16);
    DrawText(p, Sub(r, 0.18, 0.58, 0.12, 0.12), QStringLiteral("-"), 0.16);
    p->drawLine(At(r, 0.22, 0.48), At(r, 0.36, 0.48));   // error into block

    // controller block
    p->setBrush(c.fill);
    p->drawRect(Sub(r, 0.36, 0.26, 0.50, 0.44));
    DrawText(p, Sub(r, 0.36, 0.26, 0.50, 0.44), QStringLiteral("PI"), 0.30);
    p->setBrush(Qt::NoBrush);
    p->drawLine(At(r, 0.86, 0.48), At(r, 1.0, 0.48));    // output lead
}

// SVPWM: triangle carrier crossing a sine reference (classic PWM sigil).
void DrawSvpwmGlyph(QPainter* p, const QRectF& r, const GlyphColors& c) {
    p->setPen(GlyphPen(c, 1.4));
    p->setBrush(Qt::NoBrush);

    QPolygonF carrier;
    for (const double fx : {0.06, 0.185, 0.31, 0.435, 0.56, 0.685, 0.81, 0.935}) {
        carrier << At(r, fx, carrier.isEmpty() || carrier.constLast().y() > At(r, 0.0, 0.31).y()
                               ? 0.16
                               : 0.46);
    }
    p->drawPolyline(carrier);

    QPolygonF sine;
    for (int k = 0; k <= 24; ++k) {
        const double t = static_cast<double>(k) / 24.0;
        const double fy = 0.70 - 0.14 * std::sin(2.0 * M_PI * t);
        sine << At(r, 0.06 + 0.875 * t, fy);
    }
    p->setPen(GlyphPen(c, 1.6));
    p->drawPolyline(sine);
}

using GlyphFn = std::function<void(QPainter*, const QRectF&, const GlyphColors&)>;

// Exact type-id registrations win over prefix registrations.
const GlyphFn& ExactGlyph(const QString& typeId) {
    static const std::unordered_map<QString, GlyphFn> kExact = {
        {QStringLiteral("Plant.DcBus"), DrawDcBusGlyph},
        {QStringLiteral("Plant.Sensor.Hall"), DrawHallGlyph},
        {QStringLiteral("Plant.Sensor.SinCosEncoder"), DrawEncoderGlyph},
        {QStringLiteral("Plant.Sensor.VoltageSense"), DrawVoltMeterGlyph},
        {QStringLiteral("Control.Pi"), DrawPiGlyph},
        {QStringLiteral("Transforms.Svpwm"), DrawSvpwmGlyph},
    };
    static const GlyphFn kEmpty;
    const auto it = kExact.find(typeId);
    return it != kExact.end() ? it->second : kEmpty;
}

GlyphFn PrefixGlyph(const QString& typeId) {
    static const std::vector<std::pair<QString, GlyphFn>> kPrefixes = {
        {QStringLiteral("Plant.Machine."), DrawMotorGlyph},
        {QStringLiteral("Plant.Inverter."), DrawBridgeGlyph},
        {QStringLiteral("Plant.Thermal."), DrawHeatsinkGlyph},
        {QStringLiteral("Plant.Shaft."), DrawShaftGlyph},
        {QStringLiteral("Plant.DcDc."), DrawTransformerGlyph},
    };
    for (const auto& [prefix, fn] : kPrefixes) {
        if (typeId.startsWith(prefix)) {
            return fn;
        }
    }
    return nullptr;
}

GlyphFn FindGlyph(const QString& typeId) {
    if (const GlyphFn& exact = ExactGlyph(typeId)) {
        return exact;
    }
    return PrefixGlyph(typeId);
}

// Fit thresholds shared between the area negotiation (GlyphArea /
// GlyphAreaPadding) and PaintTypeGlyph's own minimum.
constexpr double kColumnMinWidth = 26.0;
constexpr double kColumnMinHeight = 20.0;
constexpr double kStripMinWidth = 26.0;
constexpr double kStripMinHeight = 24.0;

// The full-width strip below the port rows (candidate area).
QRectF StripArea(const GlyphAreaInput& input) {
    const double rowsExtent =
        std::max(input.inRows, input.outRows) *
        (input.rowHeight + input.spacing);
    const double top = input.areaTop + rowsExtent + 4.0;
    return QRectF(8.0, top, input.nodeWidth - 16.0, input.areaBottom - top);
}

// The center column between the port text columns (candidate area).
QRectF ColumnArea(const GlyphAreaInput& input) {
    const double left = input.spacing + input.inColumnWidth + 8.0;
    const double right =
        input.nodeWidth - input.outColumnWidth - input.spacing - 8.0;
    return QRectF(left, input.areaTop, right - left, input.areaBottom - input.areaTop);
}

}  // namespace

bool HasTypeGlyph(const QString& typeId) {
    return FindGlyph(typeId) != nullptr;
}

QRectF GlyphArea(const GlyphAreaInput& input) {
    const QRectF column = ColumnArea(input);
    const bool columnFits = column.width() >= kColumnMinWidth &&
                            column.height() >= kColumnMinHeight;

    const QRectF strip = StripArea(input);
    const bool stripFits = strip.width() >= kStripMinWidth &&
                           strip.height() >= kStripMinHeight;

    // Prefer the larger usable square; the column wins ties because dense
    // nodes look better with the icon between the port columns.
    if (columnFits && stripFits) {
        return std::min(column.width(), column.height()) >=
                       std::min(strip.width(), strip.height())
                   ? column
                   : strip;
    }
    if (columnFits) {
        return column;
    }
    if (stripFits) {
        return strip;
    }
    return {};
}

double GlyphAreaPadding(const GlyphAreaInput& input) {
    const QRectF column = ColumnArea(input);
    if (column.width() >= kColumnMinWidth && column.height() >= kColumnMinHeight) {
        return 0.0;
    }
    // All nodes are far wider than the strip minimum; any shortfall is
    // vertical.
    const QRectF strip = StripArea(input);
    if (strip.width() < kStripMinWidth) {
        return 0.0;
    }
    return std::max(0.0, kStripMinHeight + 8.0 - strip.height());
}

bool PaintTypeGlyph(QPainter* painter,
                    const QString& typeId,
                    const QRectF& area,
                    const GlyphColors& colors) {
    GlyphFn glyph = FindGlyph(typeId);
    if (!glyph) {
        return false;
    }

    // Center a capped square inside the offered area; too-small nodes keep
    // their text-only look.
    constexpr double kMaxSide = 52.0;
    constexpr double kMinSide = 24.0;
    const double side = std::min({area.width(), area.height(), kMaxSide});
    if (side < kMinSide) {
        return false;
    }
    const QRectF square(area.center().x() - side / 2.0,
                        area.center().y() - side / 2.0,
                        side,
                        side);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    glyph(painter, square, colors);
    painter->restore();
    return true;
}

}  // namespace NodeGUI
