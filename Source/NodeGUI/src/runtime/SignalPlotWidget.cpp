#include "SignalPlotWidget.h"

#include <QColor>
#include <QMatrix4x4>
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
    viewSeconds_ = std::clamp(seconds, 0.5, 60.0);
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
            store_->CopyHistoryInto(s.name.toStdString(), s.t, s.y);
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
    glViewport(0, 0, static_cast<int>(width() * dpr), static_cast<int>(height() * dpr));
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
    const double x1 = newest;
    const double x0 = x1 - viewSeconds_;

    // Y autorange over the visible data with 5% padding.
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
        DrawVertices(grid, GL_LINES, QColor(75, 75, 80));

        if (haveData) {
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
        }

        vao_->release();
        program_->release();
    }

    // Text overlay (QPainter paints into the widget's FBO after the GL pass).
    QPainter painter(this);
    painter.setRenderHint(QPainter::TextAntialiasing);
    const int w = width();
    const int h = height();

    if (signals_.isEmpty()) {
        painter.setPen(QColor(150, 150, 150));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("No signals selected."));
        return;
    }
    if (!haveData) {
        painter.setPen(QColor(150, 150, 150));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("No history yet."));
    }

    // Title, top-left.
    painter.setPen(QColor(220, 220, 220));
    painter.drawText(QRect(8, 4, w - 16, 20), Qt::AlignLeft | Qt::AlignVCenter, title_);

    // Y-axis unit label under the title.
    painter.setPen(QColor(170, 170, 170));
    painter.drawText(QRect(8, 24, w - 16, 18), Qt::AlignLeft | Qt::AlignVCenter, GuessUnits());

    // Color-coded legend, top-right.
    const QFontMetrics fm = painter.fontMetrics();
    int legendY = 6;
    for (int i = 0; i < signals_.size(); ++i) {
        const QString& name = signals_[i];
        const int textWidth = fm.horizontalAdvance(name);
        painter.setPen(SignalColor(i));
        painter.drawText(w - 8 - textWidth, legendY + fm.ascent(), name);
        legendY += fm.height() + 2;
    }

    if (haveData) {
        // X ticks: seconds relative to the newest sample (right edge = 0.0).
        painter.setPen(QColor(170, 170, 170));
        for (int i = 0; i <= kGridDivisionsX; ++i) {
            const int px = i * w / kGridDivisionsX;
            const double rel = -viewSeconds_ + viewSeconds_ * i / kGridDivisionsX;
            painter.drawText(px + 3, h - 6, QString::number(rel, 'f', 1));
        }
        // Y ticks: value at each horizontal grid line.
        for (int j = 0; j <= kGridDivisionsY; ++j) {
            const int py = h - j * h / kGridDivisionsY;
            const double v = y0 + (y1 - y0) * j / kGridDivisionsY;
            painter.drawText(6, py - 4, QString::number(v, 'g', 4));
        }
    }
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

QString SignalPlotWidget::GuessUnits() const
{
    if (signals_.size() > 1) {
        return QStringLiteral("Mixed units");
    }
    for (const QString& name : signals_) {
        if (name.startsWith(QStringLiteral("V_"))) {
            return QStringLiteral("Volts (V)");
        }
    }
    for (const QString& name : signals_) {
        if (name.startsWith(QStringLiteral("I_"))) {
            return QStringLiteral("Amps (A)");
        }
    }
    for (const QString& name : signals_) {
        if (name.contains(QStringLiteral("ROTOR"))) {
            return QStringLiteral("Degrees (deg)");
        }
    }
    for (const QString& name : signals_) {
        if (name.contains(QStringLiteral("RATE"))) {
            return QStringLiteral("kHz");
        }
    }
    return QStringLiteral("Value");
}

}  // namespace NodeGUI::runtime
