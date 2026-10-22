#pragma once

#include "SimSession.h"

#include "runtime/TelemetryStore.h"

#include <QWidget>

#include <memory>
#include <string>
#include <vector>
#include <QPointF>
#include <QStringList>

class QComboBox;
class QDoubleSpinBox;
class QFrame;
class QLabel;
class QLineEdit;
class QSlider;
class QSplitter;
class QStackedWidget;
class QTableWidget;
class QTimer;

namespace NodeGUI::runtime {
class SignalPlotWidget;
}

namespace NodeGUI::sim {

// Telemetry region of the Simulate screen: signal list with a per-signal
// target-graph dropdown on the left, N runtime plot leaf widgets in a
// vertical splitter column center-right (reorder by title-strip drag,
// resize by the per-pane handles; default 3 like the historical layout),
// and the session summary line underneath.
//
// Data path: SimSession (v3 jsonl on disk) -> runtime::TelemetryStore ->
// runtime::SignalPlotWidget — the same contract the live Runtime tab uses,
// so sim and recorded sessions render identically. Mirrors the runtime
// TelemetryPanel + SignalTablePanel machinery.
//
// Layout persistence: graph count + per-graph signal sets + pane order +
// pane heights autosave per session class (file stem + channel-set hash)
// into QSettings, mirroring RuntimeTab's autosave shape; a loaded session
// with a stored entry restores it instead of falling back to the first-load
// favorites. Pane semantics: assignments are PER SLOT — a title-strip drag
// moves a pane (its set travels with it) and renames slots, the vertical
// splitter's handle drags set per-slot heights (px, non-collapsible); the
// stored `paneOrder` member is the permutation, absent legacy keys mean
// natural order.
//
// Playback cursor: the same surface-hub shape as TelemetryPanel — leaf
// gestures fan out to all plots, and the Value column ("Last" when no
// cursor) reads at the cursor time through the retained store
// (TelemetryStore::ValueAt), so a recorded session yields values anywhere
// along its full span. The view-seconds slider's maximum is sampled from
// the loaded session's duration (was a hard 60), so a long recording can
// fill one window; wheel-pan + cursor freeze run on the leaf.
//
// Recorded-session overlay (backlog P1-8): "Load overlay…" parses a v3 jsonl
// through SimSessionSeriesSource into the store's overlay set; the plot leaf
// then draws the reference trace for every assigned signal (per-signal
// opt-in = graph assignment), dashed/desaturated on the same time base. The
// offset row shifts the reference against the primary axis for phase-locked
// comparison, and with a cursor pinned the Value column reads
// "live (ref R, Δ ±D)" on channels the overlay carries. Like the cursor,
// overlay state is never persisted and survives a primary LoadSession (that
// is exactly the run-N-1-vs-run-N comparison flow).
class SimTelemetryView : public QWidget {
    Q_OBJECT

public:
    explicit SimTelemetryView(QWidget* parent = nullptr);
    ~SimTelemetryView() override;

    // Loads and renders a session file. On failure returns false and keeps
    // whatever was showing before; never throws — all-null channels list as
    // grayed rows, sparse channels just drop their null samples off-plots.
    bool LoadSession(const QString& path, QString& error);
    const SimSession* Session() const { return session_.get(); }
    NodeGUI::runtime::TelemetryStore& Store() { return store_; }
    // Playback-cursor state for readers that follow the Value column's
    // source (value-at-cursor while pinned, tail otherwise) — e.g. the
    // editor-canvas graph-value tags.
    bool HasCursor() const { return hasCursor_; }
    double CursorTsec() const { return cursorTsec_; }
    // The plot row's shared view window in data-time seconds (all plots run
    // the same window through the hub, so the first plot reads back as the
    // surface) — the window the editor-canvas sparklines draw. False when
    // no session is loaded or the window is degenerate.
    bool PlotWindowTsec(double& startTsec, double& endTsec) const;
    // Sparkline read side for the editor-canvas value tags: one channel's
    // retained history thinned to <= maxPoints within [startTsec,endTsec]
    // (the store's lock-guarded CopyHistoryInto — the same bounded read the
    // ~30 Hz plot refresh performs — then uniform-stride decimation, edge
    // samples kept). False for an unknown channel or empty window slice.
    bool SparklineSamples(const std::string& key,
                          double startTsec,
                          double endTsec,
                          int maxPoints,
                          std::vector<QPointF>& out) const;
    // Chip-click probe assign: puts the channel onto the sim plot grid in
    // first-free-graph order (the first empty graph, else graph 1) through
    // the same assignment machinery the signal-table dropdowns drive
    // (ApplyAssignments + table sync + layout autosave debounce). A channel
    // already assigned is a no-op success. False for a channel the session
    // does not carry (or an all-null one — the dropdowns disable those).
    bool AssignSignalFirstFreeGraph(const QString& name);

public slots:
    // Shared-cursor operations — the leaf gesture signals connect straight
    // here, and the headless smoke driver pokes the same entry points.
    void DragCursorTo(double tsec);
    void ClearCursor();
    void PanWindowBy(double deltaTsec);

    // Recorded-session overlay (see the class comment). LoadOverlay replaces
    // the overlay set with the file's finite channels; SetOverlayOffset
    // slides it against the primary time axis; ClearOverlay unloads. The
    // headless smoke driver pokes LoadOverlay / SetOverlayOffset by surface
    // name. LoadOverlay keeps the current primary session on failure.
    bool LoadOverlay(const QString& path);
    void SetOverlayOffset(double tsec);
    void ClearOverlay();

private slots:
    void AddGraph();
    void RemoveLastGraph();

public:
    // Pane gestures + dump — the leaf title-strip drag lands on the same
    // entries the smoke verbs poke (moveplot / setplotsizes).
    bool MovePane(int from, int to);
    bool SetPlotSizes(const QList<int>& px);
    QString DumpPlotPanes() const;

private:
    void RebuildSignalTable();
    void UpdateValueColumn();
    void ApplyAssignments();
    void ApplyDefaultAssignments();
    void UpdateStatusLine();
    void UpdateCursorInfo();
    void ApplyViewSeconds();
    void UpdateOverlayInfo();
    void ApplyOverlayOffsetFromSlider(int centiseconds);
    NodeGUI::runtime::SignalPlotWidget* MakePlot();
    void RetitlePlots();
    void OnPaneDragMoved(NodeGUI::runtime::SignalPlotWidget* pane, int globalY);
    void OnPaneDragEnded(NodeGUI::runtime::SignalPlotWidget* pane, int globalY);
    void UpdateGraphCount();
    int AssignedGraph(const QString& name) const;
    // Per-session-class layout persistence (see the class comment): restore
    // runs inside LoadSession ahead of the default-assignment fallback;
    // ScheduleLayoutSave debounces every user-side change (combo target
    // edit, graph add/remove, pane reorder, splitter movement) and
    // SaveLayout writes it through.
    bool TryRestoreLayout();
    void ScheduleLayoutSave();
    void SaveLayout();

    NodeGUI::runtime::TelemetryStore store_;
    std::unique_ptr<SimSession> session_;
    QString sessionPath_;  // full path of the loaded session (status line)

    QStackedWidget* stack_ = nullptr;  // empty-state page vs content page
    QLineEdit* filterEdit_ = nullptr;
    QTableWidget* signalTable_ = nullptr;
    QSlider* viewSlider_ = nullptr;
    QSplitter* plotsSplitter_ = nullptr;
    QFrame* dropIndicator_ = nullptr;
    QLabel* countLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* cursorInfo_ = nullptr;

    std::vector<NodeGUI::runtime::SignalPlotWidget*> plots_;
    std::vector<QStringList> graphSignals_{QStringList{}, QStringList{}, QStringList{}};
    // Pane ids in slot order (the persisted paneOrder member); stable ids
    // survive add/remove so the stored permutation is total. See
    // RuntimeTab's autosave for the same rank semantics.
    std::vector<int> paneOrder_;
    int nextPaneId_ = 0;
    bool rebuildingTable_ = false;
    bool hasCursor_ = false;
    double cursorTsec_ = 0.0;
    double viewCapTsec_ = 60.0;  // sampled up to the loaded session's span

    // Overlay row (see the class comment): the info label doubles as the
    // headless smoke read dump (simOverlayInfo); spin in seconds, slider in
    // centiseconds, sync-guarded against each other's feedback.
    QLabel* overlayInfo_ = nullptr;
    QDoubleSpinBox* overlayOffsetSpin_ = nullptr;
    QSlider* overlayOffsetSlider_ = nullptr;
    QString overlayName_;       // filename stem of the loaded overlay set
    bool overlaySyncGuard_ = false;

    QTimer* layoutSaveTimer_ = nullptr;
    QString layoutStem_;         // sanitized filename stem of the loaded session
    QString layoutChannelHash_;  // hash of its float channel names
};

}  // namespace NodeGUI::sim
