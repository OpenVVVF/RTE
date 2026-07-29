#pragma once

#include "TelemetryStore.h"

#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QStringList>

#include <vector>

class QColor;
class QOpenGLBuffer;
class QOpenGLShaderProgram;
class QOpenGLVertexArrayObject;

namespace NodeGUI::runtime {

// GPU-rendered scrolling line plot for live telemetry signals. Replaces the
// old ImPlot "PlotSet": one widget per plot, showing a sliding time window of
// the assigned signals pulled from the TelemetryStore.
//
// Rendering is plain OpenGL through QOpenGLWidget: a small shader program
// with an ortho projection mapping the visible window to clip space, grid
// lines plus one GL_LINE_STRIP per signal, and QPainter text overlays (title,
// legend, tick labels) on top.
class SignalPlotWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT

public:
    explicit SignalPlotWidget(QString title, QWidget* parent = nullptr);
    ~SignalPlotWidget() override;

    void SetStore(const TelemetryStore* store);  // call once
    void SetSignals(const QStringList& names);   // signals assigned to this plot
    QStringList Signals() const { return signals_; }
    void SetViewSeconds(double seconds);         // sliding X window, 0.5..60
    double ViewSeconds() const { return viewSeconds_; }

    // Accent color used for the title strip so stacked plots are easy to
    // tell apart at a glance.
    void SetAccentColor(const QColor& color) { accentColor_ = color; }

public slots:
    void Refresh();  // called ~30 Hz when store changed; triggers update()

protected:
    void initializeGL() override;
    void paintGL() override;

private:
    struct Series {
        QString name;
        std::vector<float> t;
        std::vector<float> y;
    };

    void DrawVertices(const std::vector<float>& xy, unsigned int mode, const QColor& color);
    QColor SignalColor(int index) const;
    // Unit label for the plot, or empty when the assigned signals do not all
    // resolve to the same unit.
    QString UnitLabel() const;

    QString title_;
    QColor accentColor_{150, 150, 150};
    const TelemetryStore* store_ = nullptr;
    QStringList signals_;
    double viewSeconds_ = 10.0;

    // History snapshots refreshed by Refresh() (GUI thread only).
    std::vector<Series> series_;

    QOpenGLShaderProgram* program_ = nullptr;
    QOpenGLBuffer* vbo_ = nullptr;
    QOpenGLVertexArrayObject* vao_ = nullptr;
};

}  // namespace NodeGUI::runtime
