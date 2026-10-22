#pragma once

#include <QWidget>

#include <QList>
#include <QString>
#include <vector>

class QDoubleSpinBox;
class QFrame;
class QLabel;
class QSlider;
class QSplitter;

namespace NodeGUI::runtime {

class RuntimeController;
class SignalPlotWidget;

// Plot region of the Runtime telemetry view: N SignalPlotWidget leaf
// widgets in a vertical splitter with an add/remove toolbar. The default
// count of three matches the historical layout; assignments arrive from
// SignalTablePanel (single-target dropdown per signal).
//
// Pane order + heights: the split panes are draggable by their title strips
// (leaf paneDrag* signals -> MovePane); heights adjust with the splitter
// handles (children non-collapsible). Assignments are PER SLOT: a move
// carries both the pane and its signal set to the new slot together
// (RuntimeTab forwards the reorder to SignalTablePanel::PermuteGraphSignals,
// so the dropdown labels G1..Gn follow the new slot order and nothing is
// lost). Splitter px sizes and the pane-order permutation persist through
// the runtime autosave (RuntimeTab::Pane*Joined plumbing).
//
// Also the surface hub for the playback cursor: leaf cursor gestures
// (cursorDragged / cursorClearRequested / panRequested) land here, get
// fanned out to every plot, and are re-emitted as cursorChanged so the
// signal table's Value column can follow (RuntimeTab wires that). A pinned
// cursor holds the window span while the live stream keeps running behind
// it; clearing re-tails. The small cursorInfo_ label doubles as the
// headless-smoke read dump.
//
// Recorded-session overlay (backlog P1-8): same store overlay seam as the
// sim surface — "Load…" registers a recorded session via
// sim::SimSessionSeriesSource into the live store, and the leaf draws the
// reference dashed on the assigned signals with the offset row for
// phase-locked comparison. The Value-column delta stays sim-side (the
// runtime Value column lives in SignalTablePanel); this panel owns
// load/offset/clear plus the rtOverlayInfo smoke dump. Overlay state is
// never persisted and survives Clear Session (the reference is independent
// of the live ring).
class TelemetryPanel : public QWidget {
    Q_OBJECT

public:
    explicit TelemetryPanel(RuntimeController* controller, QWidget* parent = nullptr);

    int GraphCount() const { return static_cast<int>(plots_.size()); }
    void SetGraphSignals(const std::vector<QStringList>& sets);

    // Pane persistence plumbing (RuntimeTab autosave owns the QSettings
    // glue): the order member is the pane-id list in slot order; heights
    // are splitter px sizes in slot order. A missing/invalid order restores
    // naturally.
    std::vector<int> PaneOrder() const { return paneOrder_; }
    QList<int> PaneSizes() const;
    void RestorePaneState(const std::vector<int>& order, const QList<int>& heights);
    // Headless-smoke dump: "order=.. heights=.. titles=..".
    QString DumpPlotPanes() const;

public slots:
    void SetViewSeconds(double seconds);
    void AddGraph();
    void RemoveLastGraph();
    // Snap the visible graph count to a target (preset/autosave load path):
    // assignment drops for removed graphs are the caller's business.
    void SetGraphCount(int count);

    // Shared-cursor operations — the leaf gesture signals connect straight
    // here, and the headless smoke driver pokes the same entry points.
    void DragCursorTo(double tsec);
    void ClearCursor();
    void PanWindowBy(double deltaTsec);

    // Recorded-session overlay (see the class comment): same entry points
    // the headless smoke driver pokes by surface name.
    bool LoadOverlay(const QString& path);
    void SetOverlayOffset(double tsec);
    void ClearOverlay();

    // Pane gestures — the leaf drag lands here; the smoke verbs moveplot /
    // setplotsizes drive the same entries by surface name.
    bool MovePane(int from, int to);
    bool SetPlotSizes(const QList<int>& px);

signals:
    void graphCountChanged(int count);
    void cursorChanged(bool hasCursor, double tsec);
    // A pane moved or the splitter sizes changed (autosave debounce hook).
    void layoutChanged();
    // A reorder committed: the signal table must permute its per-slot sets
    // (RuntimeTab forwards to SignalTablePanel::PermuteGraphSignals).
    void panesReordered(int from, int to);

private slots:
    void OnStoreChanged();

private:
    SignalPlotWidget* MakePlot();
    void RetitlePlots();
    void OnPaneDragMoved(SignalPlotWidget* pane, int globalY);
    void OnPaneDragEnded(SignalPlotWidget* pane, int globalY);
    void UpdateCursorInfo();
    void UpdateOverlayInfo();
    void ApplyOverlayOffsetFromSlider(int centiseconds);

    RuntimeController* controller_;

    QSplitter* plotsSplitter_ = nullptr;
    QFrame* dropIndicator_ = nullptr;
    QLabel* countLabel_ = nullptr;
    QLabel* cursorInfo_ = nullptr;

    // Overlay row: spin in seconds, slider in centiseconds, sync-guarded.
    QLabel* overlayInfo_ = nullptr;
    QDoubleSpinBox* overlayOffsetSpin_ = nullptr;
    QSlider* overlayOffsetSlider_ = nullptr;
    QString overlayName_;
    bool overlaySyncGuard_ = false;

    std::vector<SignalPlotWidget*> plots_;
    // Pane ids in slot order (see PaneOrderJoined): stable ids assigned at
    // creation survive add/remove so the persisted permutation is total.
    std::vector<int> paneOrder_;
    int nextPaneId_ = 0;
    bool hasCursor_ = false;
    double cursorTsec_ = 0.0;
};

}  // namespace NodeGUI::runtime
