#include "SignalPlotWidget.h"

#include <QColor>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QOpenGLBuffer>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QPainter>
#include <QPen>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace NodeGUI::runtime {

namespace {

constexpr int kGridDivisionsX = 8;
constexpr int kGridDivisionsY = 6;

// Widget margins around the plot area: the title strip on top, Y tick labels
// on the left, X tick labels on the bottom.
constexpr int kMarginTop = 30;
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

// Unit guess for a single signal name (ported from the old app's GuessYLabel,
// with the substring checks first: "I_ROTOR_speed" is rotor speed, not amps).
QString UnitForSignal(const QString& name) {
    if (name.contains(QStringLiteral("ROTOR"))) {
        return QStringLiteral("Degrees (deg)");
    }
    if (name.contains(QStringLiteral("RATE"))) {
        return QStringLiteral("kHz");
    }
    if (name.startsWith(QStringLiteral("V_"))) {
        return QStringLiteral("Volts (V)");
    }
    if (name.startsWith(QStringLiteral("I_"))) {
        return QStringLiteral("Amps (A)");
    }
    return QStringLiteral("Value");
}

}  // namespace

SignalPlotWidget::SignalPlotWidget(QString title, QWidget* parent)
    : QOpenGLWidget(parent), title_(std::move(title))
{
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
    viewSeconds_ = std::clamp(seconds, 0.5, viewSecondsCap_);
    update();
}

void SignalPlotWidget::SetViewSecondsCap(double cap)
{
    viewSecondsCap_ = std::max(0.5, cap);
    viewSeconds_ = std::clamp(viewSeconds_, 0.5, viewSecondsCap_);
    update();
}

void SignalPlotWidget::SetCursor(bool hasCursor, double tsec)
{
    if (hasCursor && !hasCursor_ && followTail_) {
        // Live hold: freeze the window at its current end so the pinned
        // cursor stays put while the stream runs on behind it.
        anchoredEndTsec_ = NewestSampleTsec();
        followTail_ = false;
    }
    hasCursor_ = hasCursor;
    cursorTsec_ = tsec;
    update();
}

void SignalPlotWidget::FollowTail()
{
    followTail_ = true;
    update();
}

void SignalPlotWidget::PanWindowBy(double deltaTsec)
{
    double end = WindowEndTsec() + deltaTsec;
    // The right edge never steps before the first known sample; the left
    // side of the window may still outrun the data (renders as empty).
    end = std::max(end, OldestSampleTsec());
    if (!hasCursor_ && end >= NewestSampleTsec()) {
        // Ran back onto the newest edge: resume following automatically.
        followTail_ = true;
    } else {
        // A pinned cursor never re-follows: the hold is released only by
        // the surface (double-click clear / load), per the shared contract.
        anchoredEndTsec_ = hasCursor_ ? std::min(end, NewestSampleTsec()) : end;
        followTail_ = false;
    }
    update();
}

double SignalPlotWidget::WindowStartTsec() const
{
    return WindowEndTsec() - viewSeconds_;
}

double SignalPlotWidget::WindowEndTsec() const
{
    return followTail_ ? NewestSampleTsec() : anchoredEndTsec_;
}

double SignalPlotWidget::NewestSampleTsec() const
{
    double newest = 0.0;
    for (const Series& s : series_) {
        if (!s.t.empty()) {
            newest = std::max(newest, static_cast<double>(s.t.back()));
        }
    }
    return newest;
}

double SignalPlotWidget::OldestSampleTsec() const
{
    double oldest = 0.0;
    bool haveData = false;
    for (const Series& s : series_) {
        if (!s.t.empty()) {
            oldest = haveData ? std::min(oldest, static_cast<double>(s.t.front()))
                              : static_cast<double>(s.t.front());
            haveData = true;
        }
    }
    return oldest;
}

double SignalPlotWidget::TsecAtX(int x) const
{
    const int pw = std::max(1, width() - kMarginLeft - kMarginRight);
    const int px = std::clamp(x, kMarginLeft, kMarginLeft + pw);
    const double fx = static_cast<double>(px - kMarginLeft) / pw;
    return WindowStartTsec() + fx * viewSeconds_;
}

QRect SignalPlotWidget::PlotAreaRect() const
{
    const int pw = std::max(1, width() - kMarginLeft - kMarginRight);
    const int ph = std::max(1, height() - kMarginTop - kMarginBottom);
    return QRect(kMarginLeft, kMarginTop, pw, ph);
}

void SignalPlotWidget::mousePressEvent(QMouseEvent* event)
{
    const bool inPlot = event->position().x() >= kMarginLeft
                        && event->position().x() <= width() - kMarginRight
                        && event->position().y() >= kMarginTop
                        && event->position().y() <= height() - kMarginBottom;
    if (event->button() == Qt::LeftButton && inPlot) {
        dragging_ = true;
        emit cursorDragged(TsecAtX(static_cast<int>(event->position().x())));
        event->accept();
        return;
    }
    QOpenGLWidget::mousePressEvent(event);
}

void SignalPlotWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (dragging_) {
        emit cursorDragged(TsecAtX(static_cast<int>(event->position().x())));
        event->accept();
        return;
    }
    QOpenGLWidget::mouseMoveEvent(event);
}

void SignalPlotWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && dragging_) {
        // Release keeps the cursor pinned where the drag ended.
        dragging_ = false;
        event->accept();
        return;
    }
    QOpenGLWidget::mouseReleaseEvent(event);
}

void SignalPlotWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        dragging_ = false;
        emit cursorClearRequested();
        event->accept();
        return;
    }
    QOpenGLWidget::mouseDoubleClickEvent(event);
}

void SignalPlotWidget::wheelEvent(QWheelEvent* event)
{
    const double notches = event->angleDelta().y() / 120.0;
    if (notches != 0.0) {
        emit panRequested(notches * viewSeconds_ / 4.0);
        event->accept();
        return;
    }
    QOpenGLWidget::wheelEvent(event);
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
        overlaySeries_.resize(series_.size());
        for (std::size_t i = 0; i < series_.size(); ++i) {
            store_->CopyHistoryInto(series_[i].name.toStdString(),
                                    series_[i].t, series_[i].y);
            overlaySeries_[i].name = series_[i].name;
            if (!store_->CopyOverlayHistoryInto(series_[i].name.toStdString(),
                                                overlaySeries_[i].t,
                                                overlaySeries_[i].y)) {
                // No overlay channel under this name (or no overlay at all):
                // keep the slot aligned but empty.
                overlaySeries_[i].t.clear();
                overlaySeries_[i].y.clear();
            }
        }
    } else {
        overlaySeries_.clear();
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

    // Window end: the newest sample across assigned signals when following
    // the tail, else the anchored (panned / cursor-frozen) end.
    bool haveData = false;
    for (const Series& s : series_) {
        if (!s.t.empty()) {
            haveData = true;
            break;
        }
    }
    const double x1 = WindowEndTsec();
    const double x0 = x1 - viewSeconds_;

    // Y autorange over the visible data with 5% padding. Overlay snapshots
    // participate so the reference trace never silently clips out of frame.
    double y0 = 0.0;
    double y1 = 1.0;
    if (haveData) {
        double lo = std::numeric_limits<double>::max();
        double hi = std::numeric_limits<double>::lowest();
        for (const Series& s : series_) {
            for (std::size_t i = 0; i < s.t.size(); ++i) {
                if (s.t[i] < x0) {
                    continue;
                }
                lo = std::min(lo, static_cast<double>(s.y[i]));
                hi = std::max(hi, static_cast<double>(s.y[i]));
            }
        }
        for (const Series& s : overlaySeries_) {
            for (std::size_t i = 0; i < s.t.size(); ++i) {
                if (s.t[i] < x0) {
                    continue;
                }
                lo = std::min(lo, static_cast<double>(s.y[i]));
                hi = std::max(hi, static_cast<double>(s.y[i]));
            }
        }
        double pad = (hi - lo) * 0.05;
        if (pad <= 0.0) {  // flat signal: pad to a small non-zero span
            pad = (lo != 0.0) ? std::abs(lo) * 0.05 : 0.5;
        }
        y0 = lo - pad;
        y1 = hi + pad;
    }

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
                xy.reserve(s.t.size() * 2);
                for (std::size_t i = 0; i < s.t.size(); ++i) {
                    if (s.t[i] < x0) {
                        continue;
                    }
                    xy.push_back(s.t[i]);
                    xy.push_back(s.y[i]);
                }
                DrawVertices(xy, GL_LINE_STRIP, SignalColor(static_cast<int>(si)));
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

    // Title strip. The "Graph N" titles stay plain white; the strip's darker
    // background plus the neutral separator provide the contrast.
    painter.fillRect(QRectF(0, 0, w, kMarginTop - 4), QColor(48, 51, 56));
    painter.fillRect(QRectF(0, kMarginTop - 4, w, 1), QColor(86, 90, 96));

    const QFontMetrics fm = painter.fontMetrics();

    QFont bold = normalFont;
    bold.setBold(true);
    painter.setFont(bold);
    painter.setPen(QColor(235, 235, 235));
    painter.drawText(QRect(10, 0, w / 2, kMarginTop - 4),
                     Qt::AlignLeft | Qt::AlignVCenter, title_);
    painter.setFont(normalFont);

    // Right side of the strip: unit label (if any) then the color-coded
    // legend, fitted into the space left of the title. Fitting order: legend
    // names are elided per-item first, the unit label is dropped next, and
    // leftmost legend entries are dropped last — nothing ever overflows the
    // strip or covers the plot.
    const int titleWidth = fm.horizontalAdvance(title_);
    int available = w - 12 - (10 + titleWidth + 20);

    struct StripItem {
        QString text;
        QColor color;
        int width;
    };
    QList<StripItem> legend;
    int legendWidth = 0;
    for (int i = 0; i < signals_.size(); ++i) {
        const QString elided = fm.elidedText(signals_[i], Qt::ElideMiddle, 110);
        const int itemWidth = fm.horizontalAdvance(elided) + 14;
        legend.push_back({elided, SignalColor(i), itemWidth});
        legendWidth += itemWidth;
    }

    QString units = UnitLabel();
    int unitsWidth = units.isEmpty() ? 0 : fm.horizontalAdvance(units) + 14;
    if (legendWidth + unitsWidth > available) {
        units.clear();
        unitsWidth = 0;
    }
    while (legendWidth > available && !legend.isEmpty()) {
        legendWidth -= legend.first().width;
        legend.removeFirst();
    }

    int rightEdge = w - 12;
    for (int i = legend.size() - 1; i >= 0; --i) {
        const auto& item = legend[i];
        rightEdge -= item.width - 14;
        painter.setPen(item.color);
        painter.drawText(rightEdge, 0, item.width - 14 + 1, kMarginTop - 4,
                         Qt::AlignLeft | Qt::AlignVCenter, item.text);
        rightEdge -= 14;
    }
    if (!units.isEmpty()) {
        rightEdge -= unitsWidth - 14;
        painter.setPen(QColor(190, 190, 190));
        painter.drawText(rightEdge, 0, unitsWidth - 14 + 1, kMarginTop - 4,
                         Qt::AlignLeft | Qt::AlignVCenter, units);
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

    // Axes: bright left/bottom lines with outward tick marks.
    painter.setPen(QColor(160, 166, 173));
    painter.drawLine(plotRect.topLeft(), plotRect.bottomLeft());
    painter.drawLine(plotRect.bottomLeft(), plotRect.bottomRight());

    painter.setPen(QColor(215, 215, 215));
    for (int i = 0; i <= kGridDivisionsX; ++i) {
        const double fx = static_cast<double>(i) / kGridDivisionsX;
        const int px = kMarginLeft + static_cast<int>(fx * pw);
        painter.drawLine(px, kMarginTop + ph, px, kMarginTop + ph + 4);
        // The rightmost label would clip at the widget edge and collide
        // with the "Time (s)" caption, so it is skipped. Labels are
        // relative-to-tail while following; an anchored window shows
        // absolute session seconds instead (visiting the past is not a
        // countdown from "now").
        if (haveData && i < kGridDivisionsX) {
            const double t = followTail_ ? -viewSeconds_ + viewSeconds_ * fx
                                         : x0 + viewSeconds_ * fx;
            painter.drawText(QRect(px - 30, h - kMarginBottom + 5, 60, 16),
                             Qt::AlignHCenter, QString::number(t, 'f', 1));
        }
    }
    for (int j = 0; j <= kGridDivisionsY; ++j) {
        const double fy = static_cast<double>(j) / kGridDivisionsY;
        const int py = kMarginTop + ph - static_cast<int>(fy * ph);
        painter.drawLine(kMarginLeft - 4, py, kMarginLeft, py);
        if (haveData) {
            const double v = y0 + (y1 - y0) * fy;
            // Keep the label inside the plot area so it never pokes into the
            // title strip or the X-axis row.
            const int labelY = std::clamp(py - 8, kMarginTop, h - kMarginBottom - 16);
            painter.drawText(QRect(2, labelY, kMarginLeft - 10, 16),
                             Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(v, 'g', 3));
        }
    }

    // X axis caption.
    painter.setPen(QColor(190, 190, 190));
    painter.drawText(QRect(kMarginLeft + pw - 80, h - kMarginBottom + 5, 80, 16),
                     Qt::AlignRight, QStringLiteral("Time (s)"));

    // Recorded-session overlay: one dashed, thinner trace per assigned signal
    // whose name exists in the overlay set, desaturated against the same
    // palette color so the GL live trace stays primary. Shares the axis
    // mapping above with the live traces; clipped to the plot area.
    if (haveData && !overlaySeries_.empty()) {
        painter.save();
        painter.setClipRect(plotRect);
        for (std::size_t si = 0;
             si < overlaySeries_.size() && si < series_.size(); ++si) {
            const Series& s = overlaySeries_[si];
            if (s.t.empty()) {
                continue;
            }
            QColor overlayColor = SignalColor(static_cast<int>(si)).darker(135);
            overlayColor.setAlpha(190);
            painter.setPen(QPen(overlayColor, 1.0, Qt::DashLine));
            QPointF previous;
            bool havePrevious = false;
            for (std::size_t i = 0; i < s.t.size(); ++i) {
                if (s.t[i] < x0) {
                    continue;
                }
                const double fx = (static_cast<double>(s.t[i]) - x0) / viewSeconds_;
                const double fy = (static_cast<double>(s.y[i]) - y0) / (y1 - y0);
                const QPointF point(
                    kMarginLeft + fx * pw,
                    kMarginTop + ph - fy * ph);
                if (havePrevious) {
                    painter.drawLine(previous, point);
                }
                previous = point;
                havePrevious = true;
            }
        }
        painter.restore();
    }

    // Playback cursor: one pinned vertical line at the shared cursor time
    // (drawn only while inside the visible window) plus its absolute-time
    // tag tucked under the title strip.
    if (hasCursor_ && haveData && cursorTsec_ >= x0 && cursorTsec_ <= x1) {
        const int cx = kMarginLeft
                       + static_cast<int>(
                             (cursorTsec_ - x0) / viewSeconds_ * pw + 0.5);
        painter.setPen(QColor(245, 245, 250));
        painter.drawLine(cx, kMarginTop, cx, kMarginTop + ph);
        painter.setClipRect(plotRect);
        painter.drawText(cx + 4, kMarginTop + 14, QString::number(cursorTsec_, 'f', 3));
        painter.setClipping(false);
    }

    // Frame around the plot area.
    painter.setPen(QColor(86, 90, 96));
    painter.drawRect(plotRect);
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
