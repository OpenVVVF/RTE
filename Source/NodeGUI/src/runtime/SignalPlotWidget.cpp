#include "SignalPlotWidget.h"

#include "SignalUnits.h"

#include <QColor>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <limits>

namespace NodeGUI::runtime {

namespace {

constexpr int kGridDivisionsX = 8;
constexpr int kGridDivisionsY = 6;

// Widget margins around the plot area: the title strip on top, Y tick labels
// on the left, X tick labels on the bottom. The strip carries two rows — name
// and time stamp above, legend below — so the legend no longer paints over the
// traces it describes.
constexpr int kTitleRowHeight = 20;
constexpr int kLegendRowHeight = 18;
constexpr int kMarginTop = kTitleRowHeight + kLegendRowHeight + 4;
constexpr int kMarginLeft = 64;
constexpr int kMarginRight = 12;
constexpr int kMarginBottom = 26;

// GLSL 130 works on every default-constructed Qt GL context (no core profile
// is requested app-wide).
const char* kVertexShader = R"(#version 130
in vec2 vertexPos;
uniform mat4 mvp;
void main()
{
    gl_Position = mvp * vec4(vertexPos, 0.0, 1.0);
}
)";

const char* kFragmentShader = R"(#version 130
uniform vec4 lineColor;
out vec4 fragColor;
void main()
{
    fragColor = lineColor;
}
)";

bool IsStepSignal(const QString& name) {
    return name.startsWith(QStringLiteral("pwm_gate_"));
}

void BuildStepVertices(const std::vector<float>& t,
                       const std::vector<float>& y,
                       double x0,
                       double x1,
                       std::vector<float>& xy) {
    if (t.empty() || y.empty() || t.size() != y.size()) {
        return;
    }

    std::size_t i0 = 0;
    while (i0 < t.size() && static_cast<double>(t[i0]) < x0) {
        ++i0;
    }

    float y_level = (i0 > 0) ? y[i0 - 1] : y[i0];
    xy.push_back(static_cast<float>(x0));
    xy.push_back(y_level);

    for (std::size_t i = i0; i < t.size(); ++i) {
        const double ti = static_cast<double>(t[i]);
        if (ti > x1) {
            break;
        }

        const float yi = y[i];
        if (yi != y_level) {
            xy.push_back(static_cast<float>(ti));
            xy.push_back(y_level);
            xy.push_back(static_cast<float>(ti));
            xy.push_back(yi);
            y_level = yi;
        }

        const double t_next =
            (i + 1 < t.size()) ? std::min(static_cast<double>(t[i + 1]), x1) : x1;
        if (t_next > ti) {
            xy.push_back(static_cast<float>(ti));
            xy.push_back(y_level);
            xy.push_back(static_cast<float>(t_next));
            xy.push_back(y_level);
        }
    }

    if (xy.size() >= 2) {
        const float last_x = xy[xy.size() - 2];
        if (static_cast<double>(last_x) < x1) {
            xy.push_back(static_cast<float>(x1));
            xy.push_back(y_level);
        }
    }
}

int TimeAxisDecimals(double viewSeconds) {
    if (viewSeconds < 0.05) {
        return 3;
    }
    if (viewSeconds < 0.5) {
        return 2;
    }
    if (viewSeconds < 2.0) {
        return 2;
    }
    if (viewSeconds < 15.0) {
        return 1;
    }
    return 0;
}

}  // namespace

SignalPlotWidget::SignalPlotWidget(QString title, QWidget* parent)
    : QOpenGLWidget(parent), title_(std::move(title))
{
    setMouseTracking(true);  // cursor readout without holding a button
    setCursor(Qt::CrossCursor);
    setToolTip(QStringLiteral(
        "Drag horizontally to set the time window, vertically to lock the Y range.\n"
        "Double-click to release the Y lock."));
}

QRect SignalPlotWidget::PlotRect() const
{
    const int pw = std::max(1, width() - kMarginLeft - kMarginRight);
    const int ph = std::max(1, height() - kMarginTop - kMarginBottom);
    return QRect(kMarginLeft, kMarginTop, pw, ph);
}

SignalPlotWidget::~SignalPlotWidget()
{
    makeCurrent();
    delete program_;
    delete vbo_;
    delete vao_;
    doneCurrent();
}

void SignalPlotWidget::SetStore(const TelemetryStore* store)
{
    store_ = store;
    series_.clear();
    update();
}

void SignalPlotWidget::SetSignals(const QStringList& names)
{
    signals_ = names;
    series_.clear();
    update();
}

void SignalPlotWidget::SetViewSeconds(double seconds)
{
    viewSeconds_ = std::clamp(seconds, 0.01, 60.0);
    update();
}

void SignalPlotWidget::SetTimeFrozen(bool frozen, double anchorSimSec)
{
    timeFrozen_ = frozen;
    freezeAnchorT_ = anchorSimSec;
    update();
}

void SignalPlotWidget::Refresh()
{
    // Rebuild the series list only when the assignment changed; otherwise
    // refill the existing vectors in place so repeated refreshes do not
    // reallocate.
    QStringList have;
    have.reserve(static_cast<qsizetype>(series_.size()));
    for (const Series& s : series_) {
        have.push_back(s.name);
    }
    if (have != signals_) {
        series_.clear();
        series_.reserve(static_cast<std::size_t>(signals_.size()));
        for (const QString& name : signals_) {
            Series s;
            s.name = name;
            series_.push_back(std::move(s));
        }
    }
    if (store_) {
        for (Series& s : series_) {
            const bool isGate = IsStepSignal(s.name);
            const bool highRate = isGate || s.name.startsWith(QStringLiteral("pwm_v_"));
            const double hz = isGate ? 8000.0 : (highRate ? 2000.0 : 500.0);
            const std::size_t maxPts = static_cast<std::size_t>(
                std::min(isGate ? 8000.0 : 4000.0, std::max(300.0, viewSeconds_ * hz)));
            store_->CopyHistoryInto(s.name.toStdString(), s.t, s.y, maxPts);
        }
    }
    update();
}

void SignalPlotWidget::initializeGL()
{
    initializeOpenGLFunctions();

    program_ = new QOpenGLShaderProgram;
    program_->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader);
    program_->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader);
    program_->link();

    vao_ = new QOpenGLVertexArrayObject;
    vao_->create();

    vbo_ = new QOpenGLBuffer(QOpenGLBuffer::VertexBuffer);
    vbo_->create();
    vbo_->setUsagePattern(QOpenGLBuffer::DynamicDraw);
}

void SignalPlotWidget::paintGL()
{
    const qreal dpr = devicePixelRatioF();
    const int w = width();
    const int h = height();

    // Plot area inside the margins.
    const int pw = std::max(1, w - kMarginLeft - kMarginRight);
    const int ph = std::max(1, h - kMarginTop - kMarginBottom);
    const QRectF plotRect(kMarginLeft, kMarginTop, pw, ph);

    // GL renders only into the plot area (GL origin is bottom-left).
    glViewport(static_cast<int>(kMarginLeft * dpr),
               static_cast<int>(kMarginBottom * dpr),
               static_cast<int>(pw * dpr),
               static_cast<int>(ph * dpr));
    glClearColor(45.0f / 255.0f, 45.0f / 255.0f, 48.0f / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Newest timestamp across all assigned signals anchors the sliding window.
    double newest = 0.0;
    bool haveData = false;
    for (const Series& s : series_) {
        if (!s.t.empty()) {
            newest = haveData ? std::max(newest, static_cast<double>(s.t.back()))
                              : static_cast<double>(s.t.back());
            haveData = true;
        }
    }
    const double x1 = timeFrozen_ ? freezeAnchorT_ : newest;
    const double x0 = x1 - viewSeconds_;

    // Y autorange over the visible data with 5% padding.
    double y0 = 0.0;
    double y1 = 1.0;
    bool allGateSignals =
        !signals_.isEmpty() &&
        std::all_of(signals_.begin(), signals_.end(), [](const QString& name) {
            return IsStepSignal(name);
        });
    if (yLocked_) {
        y0 = yLockLow_;
        y1 = yLockHigh_;
    } else if (allGateSignals) {
        y0 = -0.05;
        y1 = 1.05;
    } else if (haveData) {
        double lo = std::numeric_limits<double>::max();
        double hi = std::numeric_limits<double>::lowest();
        for (const Series& s : series_) {
            for (std::size_t i = 0; i < s.t.size(); ++i) {
                if (s.t[i] < x0) {
                    continue;
                }
                const double val = static_cast<double>(s.y[i]);
                if (std::isfinite(val)) {
                    lo = std::min(lo, val);
                    hi = std::max(hi, val);
                }
            }
        }
        double pad = (hi - lo) * 0.05;
        if (pad <= 0.0) {  // flat signal: pad to a small non-zero span
            pad = (lo != 0.0) ? std::abs(lo) * 0.05 : 0.5;
        }
        y0 = lo - pad;
        y1 = hi + pad;
    }
    lastY0_ = y0;
    lastY1_ = y1;

    if (program_ && program_->isLinked()) {
        QMatrix4x4 mvp;
        mvp.ortho(static_cast<float>(x0), static_cast<float>(x1),
                  static_cast<float>(y0), static_cast<float>(y1), -1.0f, 1.0f);

        program_->bind();
        program_->setUniformValue("mvp", mvp);
        vao_->bind();

        // Grid: kGridDivisionsX columns, kGridDivisionsY rows.
        std::vector<float> grid;
        grid.reserve((kGridDivisionsX + 1 + kGridDivisionsY + 1) * 4);
        for (int i = 0; i <= kGridDivisionsX; ++i) {
            const float x = static_cast<float>(x0 + (x1 - x0) * i / kGridDivisionsX);
            grid.insert(grid.end(), {x, static_cast<float>(y0), x, static_cast<float>(y1)});
        }
        for (int j = 0; j <= kGridDivisionsY; ++j) {
            const float y = static_cast<float>(y0 + (y1 - y0) * j / kGridDivisionsY);
            grid.insert(grid.end(), {static_cast<float>(x0), y, static_cast<float>(x1), y});
        }
        glLineWidth(1.0f);
        DrawVertices(grid, GL_LINES, QColor(88, 93, 100));

        if (haveData) {
            glLineWidth(1.75f);
            for (std::size_t si = 0; si < series_.size(); ++si) {
                const Series& s = series_[si];
                if (s.t.empty()) {
                    continue;
                }
                std::vector<float> xy;
                if (IsStepSignal(s.name)) {
                    BuildStepVertices(s.t, s.y, x0, x1, xy);
                    glLineWidth(2.0f);
                } else {
                    xy.reserve(s.t.size() * 2);
                    for (std::size_t i = 0; i < s.t.size(); ++i) {
                        if (s.t[i] < x0) {
                            continue;
                        }
                        if (static_cast<double>(s.t[i]) > x1) {
                            break;
                        }
                        xy.push_back(s.t[i]);
                        xy.push_back(s.y[i]);
                    }
                }
                DrawVertices(xy, GL_LINE_STRIP, SignalColor(static_cast<int>(si)));
                if (IsStepSignal(s.name)) {
                    glLineWidth(1.75f);
                }
            }
            glLineWidth(1.0f);
        }

        vao_->release();
        program_->release();
    }

    // Everything outside the GL pass: background, title strip, axes, labels.
    // The margins are filled around the plot area — painting over it would
    // erase the GL traces.
    QPainter painter(this);
    painter.setRenderHint(QPainter::TextAntialiasing);
    const QFont normalFont = painter.font();

    const QColor marginColor(38, 38, 41);
    painter.fillRect(QRectF(0, 0, w, kMarginTop), marginColor);
    painter.fillRect(QRectF(0, h - kMarginBottom, w, kMarginBottom), marginColor);
    painter.fillRect(QRectF(0, kMarginTop, kMarginLeft, ph), marginColor);
    painter.fillRect(QRectF(w - kMarginRight, kMarginTop, kMarginRight, ph), marginColor);

    // Title strip with a per-graph accent line underneath.
    painter.fillRect(QRectF(0, 0, w, kMarginTop - 4), QColor(48, 51, 56));
    painter.fillRect(QRectF(0, kMarginTop - 4, w, 2), accentColor_);

    QFont bold = normalFont;
    bold.setBold(true);
    painter.setFont(bold);
    painter.setPen(QColor(235, 235, 235));
    painter.drawText(QRect(10, 0, w / 3, kTitleRowHeight),
                     Qt::AlignLeft | Qt::AlignVCenter, title_);
    painter.setFont(normalFont);

    // Center: absolute sim time at the right edge plus the window width, so
    // the negative tick labels below have an anchor to be relative to.
    if (haveData) {
        const QString stamp =
            QStringLiteral("%1t = %2 s   last %3 s")
                .arg(timeFrozen_ ? QStringLiteral("PAUSED  ") : QString())
                .arg(x1, 0, 'f', 3)
                .arg(viewSeconds_, 0, 'f', TimeAxisDecimals(viewSeconds_));
        painter.setPen(timeFrozen_ ? QColor(255, 190, 90) : QColor(170, 176, 184));
        painter.drawText(QRect(w / 3, 0, w / 3, kTitleRowHeight),
                         Qt::AlignHCenter | Qt::AlignVCenter, stamp);
    }

    // Unit label, only when every assigned signal resolves to the same unit.
    QString units = UnitLabel();
    if (yLocked_) {
        units = units.isEmpty() ? QStringLiteral("Y LOCK")
                                : units + QStringLiteral("  ·  Y LOCK");
    }
    if (!units.isEmpty()) {
        painter.setPen(yLocked_ ? QColor(255, 190, 90) : QColor(190, 190, 190));
        painter.drawText(QRect(w - w / 3 - 10, 0, w / 3, kTitleRowHeight),
                         Qt::AlignRight | Qt::AlignVCenter, units);
    }

    if (signals_.isEmpty()) {
        painter.setPen(QColor(150, 150, 150));
        painter.drawText(plotRect, Qt::AlignCenter, QStringLiteral("No signals selected."));
        return;
    }
    if (!haveData) {
        painter.setPen(QColor(150, 150, 150));
        painter.drawText(plotRect, Qt::AlignCenter, QStringLiteral("No history yet."));
    }

    // Color-coded legend on the strip's second row, laid out horizontally with
    // a swatch per signal. When the cursor is over the plot each entry also
    // carries the sampled value at that instant.
    const QFontMetrics fm = painter.fontMetrics();
    const int legendBaseline = kTitleRowHeight + (kLegendRowHeight + fm.ascent()) / 2 - 1;
    int legendX = 10;
    for (int i = 0; i < signals_.size(); ++i) {
        QString entry = signals_[i];
        if (hovering_ && haveData && i < static_cast<int>(series_.size())) {
            const Series& s = series_[i];
            if (!s.t.empty()) {
                const double frac =
                    static_cast<double>(hoverPos_.x() - kMarginLeft) / pw;
                const double tCursor = x0 + (x1 - x0) * std::clamp(frac, 0.0, 1.0);
                const auto it = std::lower_bound(s.t.begin(), s.t.end(),
                                                 static_cast<float>(tCursor));
                std::size_t idx = static_cast<std::size_t>(it - s.t.begin());
                if (idx >= s.y.size()) {
                    idx = s.y.size() - 1;
                }
                entry += QStringLiteral(" = ") + FormatSignalValue(s.y[idx]);
            }
        }
        const int textWidth = fm.horizontalAdvance(entry);
        if (legendX + textWidth + 16 > w - kMarginRight) {
            break;
        }
        painter.fillRect(QRect(legendX, legendBaseline - fm.ascent() + 3, 8, 8),
                         SignalColor(i));
        painter.setPen(SignalColor(i));
        painter.drawText(legendX + 12, legendBaseline, entry);
        legendX += textWidth + 24;
    }

    // Axes: bright left/bottom lines with outward tick marks.
    painter.setPen(QColor(160, 166, 173));
    painter.drawLine(plotRect.topLeft(), plotRect.bottomLeft());
    painter.drawLine(plotRect.bottomLeft(), plotRect.bottomRight());

    painter.setPen(QColor(215, 215, 215));
    const int timeDecimals = TimeAxisDecimals(viewSeconds_);
    QString lastTimeLabel;
    for (int i = 0; i <= kGridDivisionsX; ++i) {
        const double fx = static_cast<double>(i) / kGridDivisionsX;
        const int px = kMarginLeft + static_cast<int>(fx * pw);
        painter.drawLine(px, kMarginTop + ph, px, kMarginTop + ph + 4);
        if (!haveData) {
            continue;
        }
        const double rel = -viewSeconds_ + viewSeconds_ * fx;
        // The right edge is "now": label it 0 rather than a rounded -0.00.
        const bool isNow = (i == kGridDivisionsX);
        const QString label =
            isNow ? QStringLiteral("0") : QString::number(rel, 'f', timeDecimals);
        if (label == lastTimeLabel) {
            continue;
        }
        lastTimeLabel = label;
        // The last label would overflow the widget if centered on its tick.
        const QRect box = isNow ? QRect(px - 56, h - kMarginBottom + 5, 56, 16)
                                : QRect(px - 30, h - kMarginBottom + 5, 60, 16);
        painter.drawText(box, isNow ? Qt::AlignRight : Qt::AlignHCenter, label);
    }
    for (int j = 0; j <= kGridDivisionsY; ++j) {
        const double fy = static_cast<double>(j) / kGridDivisionsY;
        const int py = kMarginTop + ph - static_cast<int>(fy * ph);
        painter.drawLine(kMarginLeft - 4, py, kMarginLeft, py);
        if (haveData) {
            const double v = y0 + (y1 - y0) * fy;
            painter.drawText(QRect(2, py - 8, kMarginLeft - 10, 16),
                             Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(v, 'g', 3));
        }
    }

    // Cursor crosshair with the time it points at.
    if (hovering_ && haveData && !dragging_) {
        const double frac = static_cast<double>(hoverPos_.x() - kMarginLeft) / pw;
        const double tCursor = x0 + (x1 - x0) * std::clamp(frac, 0.0, 1.0);
        painter.setPen(QPen(QColor(220, 220, 220, 130), 1, Qt::DashLine));
        painter.drawLine(hoverPos_.x(), kMarginTop, hoverPos_.x(), kMarginTop + ph);
        painter.setPen(QColor(230, 230, 230));
        painter.drawText(QRect(hoverPos_.x() - 60, kMarginTop + ph - 18, 56, 16),
                         Qt::AlignRight,
                         QStringLiteral("%1 s").arg(tCursor, 0, 'f', 3));
    }

    // Rubber band: horizontal extent picks the new time window, vertical
    // extent locks the Y range.
    if (dragging_) {
        const QRect band = QRect(dragStart_, dragCurrent_).normalized() & plotRect.toRect();
        painter.setPen(QPen(QColor(255, 220, 120), 1, Qt::DashLine));
        painter.setBrush(QColor(255, 220, 120, 40));
        painter.drawRect(band);
        painter.setBrush(Qt::NoBrush);
    }

    // Frame around the plot area.
    painter.setPen(QColor(86, 90, 96));
    painter.drawRect(plotRect);

    if (timeFrozen_) {
        // A frozen trace is otherwise indistinguishable from a dead link.
        QFont badgeFont = normalFont;
        badgeFont.setBold(true);
        painter.setFont(badgeFont);
        const QString badge = QStringLiteral("PAUSED");
        const QFontMetrics badgeFm(badgeFont);
        const QRect textRect = badgeFm.boundingRect(badge);
        const QRectF box(plotRect.left() + 8, plotRect.top() + 8,
                         textRect.width() + 18, textRect.height() + 8);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(183, 121, 31, 210));
        painter.drawRoundedRect(box, 4, 4);
        painter.setPen(QColor(20, 18, 12));
        painter.drawText(box, Qt::AlignCenter, badge);
        painter.setFont(normalFont);
    }
}

void SignalPlotWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && PlotRect().contains(event->pos())) {
        dragging_ = true;
        dragStart_ = event->pos();
        dragCurrent_ = event->pos();
        update();
    }
    QOpenGLWidget::mousePressEvent(event);
}

void SignalPlotWidget::mouseMoveEvent(QMouseEvent* event)
{
    hovering_ = PlotRect().contains(event->pos());
    hoverPos_ = event->pos();
    if (dragging_) {
        dragCurrent_ = event->pos();
    }
    update();
    QOpenGLWidget::mouseMoveEvent(event);
}

void SignalPlotWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (dragging_ && event->button() == Qt::LeftButton) {
        dragging_ = false;
        const QRect plotRect = PlotRect();
        const QRect band = QRect(dragStart_, event->pos()).normalized() & plotRect;

        // A drag has to clear a few pixels on an axis to count, so a stray
        // click does not collapse the window or pin the Y range.
        constexpr int kMinDragPx = 8;

        if (band.height() >= kMinDragPx) {
            // Screen Y grows downward while the data axis grows upward.
            const double top = static_cast<double>(band.top() - plotRect.top()) /
                               plotRect.height();
            const double bottom = static_cast<double>(band.bottom() - plotRect.top()) /
                                  plotRect.height();
            const double span = lastY1_ - lastY0_;
            yLockLow_ = lastY0_ + (1.0 - bottom) * span;
            yLockHigh_ = lastY0_ + (1.0 - top) * span;
            yLocked_ = yLockHigh_ > yLockLow_;
        }
        if (band.width() >= kMinDragPx) {
            const double seconds =
                viewSeconds_ * static_cast<double>(band.width()) / plotRect.width();
            emit viewSecondsRequested(std::clamp(seconds, 0.01, 60.0));
        }
        update();
    }
    QOpenGLWidget::mouseReleaseEvent(event);
}

void SignalPlotWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        yLocked_ = false;
        update();
    }
    QOpenGLWidget::mouseDoubleClickEvent(event);
}

void SignalPlotWidget::leaveEvent(QEvent* event)
{
    hovering_ = false;
    update();
    QOpenGLWidget::leaveEvent(event);
}

void SignalPlotWidget::DrawVertices(const std::vector<float>& xy, unsigned int mode, const QColor& color)
{
    if (xy.empty()) {
        return;
    }
    vbo_->bind();
    vbo_->allocate(xy.data(), static_cast<int>(xy.size() * sizeof(float)));
    program_->setAttributeBuffer("vertexPos", GL_FLOAT, 0, 2);
    program_->enableAttributeArray("vertexPos");
    program_->setUniformValue("lineColor", color);
    glDrawArrays(mode, 0, static_cast<int>(xy.size() / 2));
}

QColor SignalPlotWidget::SignalColor(int index) const
{
    // Stable, distinct bright colors on the dark background; cycles past 8.
    static const QColor colors[] = {
        QColor(102, 204, 255),  // cyan
        QColor(255, 140, 60),   // orange
        QColor(140, 235, 120),  // green
        QColor(255, 110, 130),  // red/pink
        QColor(255, 225, 90),   // yellow
        QColor(190, 150, 255),  // purple
        QColor(90, 230, 200),   // teal
        QColor(255, 170, 220),  // pink
    };
    return colors[index % 8];
}

QString SignalPlotWidget::UnitLabel() const
{
    // Only meaningful when every assigned signal resolves to the same unit;
    // otherwise the label would be wrong, so show nothing.
    if (signals_.isEmpty()) {
        return QString();
    }
    const QString first = UnitForSignal(signals_.front());
    for (const QString& name : signals_) {
        if (UnitForSignal(name) != first) {
            return QString();
        }
    }
    return first;
}

}  // namespace NodeGUI::runtime
