#pragma once

#include "TelemetryStore.h"

#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QStringList>

#include <deque>
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

public slots:
    void Refresh();  // called ~30 Hz when store changed; triggers update()

protected:
    void initializeGL() override;
    void paintGL() override;

private:
    struct Series {
        QString name;
        std::deque<float> t;
        std::deque<float> y;
    };

    void DrawVertices(const std::vector<float>& xy, unsigned int mode, const QColor& color);
    QColor SignalColor(int index) const;
    QString GuessUnits() const;

    QString title_;
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
