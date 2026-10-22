#pragma once

#include "TelemetryStore.h"

#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QStringList>

#include <QRect>

#include <vector>

class QColor;
class QOpenGLBuffer;
class QOpenGLShaderProgram;
class QOpenGLVertexArrayObject;
class QTimer;

namespace NodeGUI::runtime {

// GPU-rendered scrolling line plot for live telemetry signals. Replaces the
// old ImPlot "PlotSet": one widget per plot, showing a sliding time window of
// the assigned signals pulled from the TelemetryStore.
//
// Rendering is plain OpenGL through QOpenGLWidget: a small shader program
// with an ortho projection mapping the visible window to clip space, grid
// lines plus one GL_LINE_STRIP per signal, and QPainter text overlays (title,
// legend, tick labels) on top.
//
// Playback cursor + pan: the widget only REPORTS gestures (cursorDragged,
// cursorClearRequested, panRequested) and renders whatever the owning
// surface pushes down (SetCursor); the surface fans one shared cursor out to
// every plot so a drag on one draws at the same time in all. Setting a
// cursor freezes the window at its current span (a live stream keeps
// running behind the hold); clearing the cursor plus FollowTail returns the
// window to the newest edge. Wheel pans the window off the tail; wheeling
// back onto the newest edge re-engages following (unless a cursor holds).
//
// Recorded-session overlay: when the store carries an overlay set, Refresh()
// additionally snapshots the overlay history of each assigned signal (the
// store returns it offset-mapped into the live time axis), so the reference
// trace for exactly the user's picked signals rides along in the same QPainter
// pass as the cursor — dashed, thinner, and desaturated against the same
// palette color. The leaf keeps no overlay state of its own beyond those
// snapshots; load/clear/offset all flow through the store + a Refresh.
class SignalPlotWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT

public:
    explicit SignalPlotWidget(QString title, QWidget* parent = nullptr);
    ~SignalPlotWidget() override;

    void SetStore(const TelemetryStore* store);  // call once
    void SetSignals(const QStringList& names);   // signals assigned to this plot
    QStringList Signals() const { return signals_; }
    void SetViewSeconds(double seconds);  // sliding X window, 0.5..cap
    double ViewSeconds() const { return viewSeconds_; }
    // Upper clamp for SetViewSeconds (default 60): the sim surface samples it
    // up to a long session's span so recorded data can fill one window.
    void SetViewSecondsCap(double cap);

    // Surface-pushed shared cursor (data-time seconds). Setting one while
    // following freezes the window at its current end; clearing does NOT
    // re-tail by itself — the surface pairs it with FollowTail on the
    // double-click / load paths.
    void SetCursor(bool hasCursor, double tsec);
    bool HasCursor() const { return hasCursor_; }
    double CursorTsec() const { return cursorTsec_; }

    // Window positioning: followTail_ means the right edge tracks the newest
    // sample (default); otherwise it sits at anchoredEndTsec_.
    void FollowTail();
    bool FollowingTail() const { return followTail_; }
    void PanWindowBy(double deltaTsec);
    double WindowStartTsec() const;
    double WindowEndTsec() const;
    // Plot-area rectangle in widget coords — doubles as the headless smoke
    // driver's target for synthesized gestures at a known data position.
    QRect PlotAreaRect() const;

public slots:
    void Refresh();  // called ~30 Hz when store changed; triggers update()

signals:
    void cursorDragged(double tsec);     // left press/move inside the plot area
    void cursorClearRequested();         // left double-click
    void panRequested(double deltaTsec); // wheel notches, + toward newer

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

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

    double NewestSampleTsec() const;  // 0 when nothing has data
    double OldestSampleTsec() const;
    // Data time under a widget X pixel, using the current window.
    double TsecAtX(int x) const;

    QString title_;
    const TelemetryStore* store_ = nullptr;
    QStringList signals_;
    double viewSeconds_ = 10.0;
    double viewSecondsCap_ = 60.0;

    // History snapshots refreshed by Refresh() (GUI thread only).
    std::vector<Series> series_;
    // Parallel to series_: the overlay (recorded reference) snapshot of the
    // same signal, times already shifted by the overlay offset. Empty when
    // the store has no overlay channel under that name. View-only: rebuilt
    // wholesale on every Refresh().
    std::vector<Series> overlaySeries_;

    bool hasCursor_ = false;
    double cursorTsec_ = 0.0;
    bool dragging_ = false;
    bool followTail_ = true;
    double anchoredEndTsec_ = 0.0;

    // Resize-burst coalescer (see resizeEvent): one-shot throttle window and
    // the "an intermediate resize was swallowed" flag it commits on timeout.
    QTimer* resizeCoalesceTimer_ = nullptr;
    bool commitPending_ = false;

    QOpenGLShaderProgram* program_ = nullptr;
    QOpenGLBuffer* vbo_ = nullptr;
    QOpenGLVertexArrayObject* vao_ = nullptr;
};

}  // namespace NodeGUI::runtime
