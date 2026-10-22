#include "SimTelemetryView.h"

#include "SimSessionSeriesSource.h"
#include "runtime/SignalPlotWidget.h"

#include <QColor>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace NodeGUI::sim {

namespace {

constexpr int kSignalColumn = 0;
constexpr int kTargetColumn = 1;
constexpr int kValueColumn = 2;

// First-load favorites, checked in order: G1 currents, G2 DC link, G3
// speed (additional graphs start empty). Applied only when neither a
// persisted layout nor the in-memory carry-over covers the new session.
const std::vector<QStringList> kDefaultSignals = {
    QStringList{QStringLiteral("cg_id_a"), QStringLiteral("cg_iq_a")},
    QStringList{QStringLiteral("vdc_v")},
    QStringList{QStringLiteral("Mech_RPM"), QStringLiteral("Elec_RPM")}};

// Corrupt-settings guard for the restored graph count (the runtime autosave
// caps under the same constant).
constexpr int kMaxPersistedGraphs = 12;

// Lower clamp for persisted pane heights (splitter px sizes).
constexpr int kMinPaneHeightPx = 60;

// Same factory idiom as SimulateScreen::MakeSimSettings /
// RuntimeTab::MakeSettings (per-TU copies of the 3-liner).
QSettings MakeSimSettings() {
    return QSettings(QStringLiteral("RTE"), QStringLiteral("RTEStudio"));
}

// QSettings group components nest on '/', so the session-file stem is
// reduced to a filename-ish alphabet first (OnSavePreset does the same to
// layout names with name.replace('/', '_')).
QString SanitizeLayoutStem(const QString& stem) {
    QString clean = stem;
    for (QChar& ch : clean) {
        if (!ch.isLetterOrNumber() && ch != u'-' && ch != u'_' && ch != u'.') {
            ch = u'_';
        }
    }
    return clean.isEmpty() ? QStringLiteral("session") : clean;
}

// Session-class discriminator: two files that share a stem but carry
// different channel sets (a foc_demo run vs a DSR replay) keep separate
// layouts. SHA-1 because qHash is salted per process and therefore useless
// across restarts; 12 hex chars is ample here.
QString ChannelSetHash(const QStringList& sortedNames) {
    QCryptographicHash digest(QCryptographicHash::Sha1);
    for (const QString& name : sortedNames) {
        digest.addData(name.toUtf8() + '\n');
    }
    return QString::fromLatin1(digest.result().toHex().left(12));
}

QString LayoutGroupFor(const QString& sanitizedStem, const QString& channelHash) {
    return QStringLiteral("sim/layouts/") + sanitizedStem + u'/' + channelHash;
}

QString LayoutStemHashKey(const QString& channelHash) {
    return QStringLiteral("sim/layoutsByHash/") + channelHash;
}

// Pane-order rank semantics (same convention as RuntimeTab's autosave): the
// order member is the pane-id list in SLOT order and the graph{i} sets are
// keyed NATURALLY (id rank), so a reorder only rewrites the order member.
// Files from before pane persistence carry no order/heights keys: natural
// arrangement and default heights.
QString JoinIntList(const std::vector<int>& values) {
    QStringList parts;
    for (int value : values) {
        parts.push_back(QString::number(value));
    }
    return parts.join(QStringLiteral(","));
}

// Parses the comma-joined order member; empty on absent/malformed (the
// caller falls back to natural order for anything invalid).
std::vector<int> ParsePaneOrder(const QString& joined, int count) {
    std::vector<int> ids;
    if (joined.isEmpty()) {
        return ids;
    }
    for (const QString& part : joined.split(u',')) {
        bool ok = false;
        const int id = part.toInt(&ok);
        if (!ok) {
            return {};
        }
        ids.push_back(id);
    }
    if (static_cast<int>(ids.size()) != count) {
        return {};
    }
    std::vector<int> sorted = ids;
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
        return {};
    }
    return ids;
}

QList<int> ParsePaneHeights(const QString& joined) {
    QList<int> heights;
    if (joined.isEmpty()) {
        return heights;
    }
    for (const QString& part : joined.split(u',')) {
        heights.push_back(part.toInt());
    }
    return heights;
}

}  // namespace

SimTelemetryView::SimTelemetryView(QWidget* parent)
    : QWidget(parent) {
    stack_ = new QStackedWidget(this);

    auto* emptyLabel = new QLabel(
        QStringLiteral("No session yet — run a scenario or load a session file."),
        this);
    emptyLabel->setAlignment(Qt::AlignCenter);
    stack_->addWidget(emptyLabel);

    auto* content = new QWidget(this);
    stack_->addWidget(content);

    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);

    auto* splitter = new QSplitter(Qt::Horizontal, content);

    auto* leftPanel = new QWidget(splitter);
    auto* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    filterEdit_ = new QLineEdit(leftPanel);
    filterEdit_->setObjectName(QStringLiteral("simSignalFilter"));
    filterEdit_->setPlaceholderText(QStringLiteral("Filter"));
    connect(filterEdit_, &QLineEdit::textChanged, this,
            [this] { RebuildSignalTable(); });
    leftLayout->addWidget(filterEdit_);

    signalTable_ = new QTableWidget(0, 3, leftPanel);
    signalTable_->setObjectName(QStringLiteral("simSignalTable"));
    // "Last": the column shows the session-final value until a playback
    // cursor exists (the runtime live table's "Value" is live-latest); a
    // pinned cursor flips the header to "Cursor" (UpdateValueColumn).
    signalTable_->setHorizontalHeaderLabels(
        {QStringLiteral("Signal"), QStringLiteral("Graph"), QStringLiteral("Last")});
    signalTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    signalTable_->horizontalHeader()->setStretchLastSection(true);
    signalTable_->setColumnWidth(kSignalColumn, 170);
    signalTable_->setColumnWidth(kTargetColumn, 110);
    signalTable_->setColumnWidth(kValueColumn, 90);
    signalTable_->verticalHeader()->setVisible(false);
    signalTable_->setSelectionMode(QAbstractItemView::NoSelection);
    leftLayout->addWidget(signalTable_, 1);
    splitter->addWidget(leftPanel);

    auto* rightPanel = new QWidget(splitter);
    auto* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);

    auto* topRow = new QHBoxLayout;
    topRow->setContentsMargins(4, 2, 4, 2);
    topRow->addWidget(new QLabel(QStringLiteral("Graphs"), rightPanel));
    countLabel_ = new QLabel(rightPanel);
    countLabel_->setObjectName(QStringLiteral("simGraphCount"));
    topRow->addWidget(countLabel_);
    topRow->addStretch(1);
    topRow->addWidget(new QLabel(QStringLiteral("Plot view (sec)"), rightPanel));
    // Same idiom as the runtime SignalTablePanel slider: 0.5..60.0 s in
    // 0.1 s steps, driving every plot's SetViewSeconds.
    viewSlider_ = new QSlider(Qt::Horizontal, rightPanel);
    viewSlider_->setObjectName(QStringLiteral("simViewSeconds"));
    viewSlider_->setRange(5, 600);
    viewSlider_->setValue(100);  // 10 s, the SignalPlotWidget default
    connect(viewSlider_, &QSlider::valueChanged, this,
            [this](int) { ApplyViewSeconds(); });
    topRow->addWidget(viewSlider_, 1);
    auto* addButton = new QPushButton(QStringLiteral("+ Graph"), rightPanel);
    addButton->setObjectName(QStringLiteral("simAddGraph"));
    addButton->setToolTip(QStringLiteral("Add another telemetry graph"));
    connect(addButton, &QPushButton::clicked, this, &SimTelemetryView::AddGraph);
    topRow->addWidget(addButton);
    auto* removeButton = new QPushButton(QStringLiteral("- Graph"), rightPanel);
    removeButton->setObjectName(QStringLiteral("simRemoveGraph"));
    removeButton->setToolTip(QStringLiteral(
        "Remove the last graph (its signals unassign)"));
    connect(removeButton, &QPushButton::clicked, this,
            &SimTelemetryView::RemoveLastGraph);
    topRow->addWidget(removeButton);
    rightLayout->addLayout(topRow);

    // Recorded-session overlay row (backlog P1-8): load/clear the reference
    // set + the per-overlay time offset (spin in seconds, slider in
    // centiseconds) + the info label that doubles as the headless smoke
    // dump. Nothing here is ever persisted — same convention as the cursor.
    auto* overlayRow = new QHBoxLayout;
    overlayRow->setContentsMargins(4, 0, 4, 2);
    overlayRow->addWidget(new QLabel(QStringLiteral("Overlay"), rightPanel));
    auto* overlayLoadButton =
        new QPushButton(QStringLiteral("Load\u2026"), rightPanel);
    overlayLoadButton->setObjectName(QStringLiteral("simOverlayLoad"));
    overlayLoadButton->setToolTip(QStringLiteral(
        "Load a recorded session as an overlay reference (dashed traces)"));
    connect(overlayLoadButton, &QPushButton::clicked, this, [this] {
        const QString startDir = MakeSimSettings()
                                     .value(QStringLiteral("sim/lastSessionDir"))
                                     .toString();
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Load Overlay Session"), startDir,
            QStringLiteral("RTE Runtime Session (*.jsonl);;All Files (*)"));
        if (!path.isEmpty()) {
            LoadOverlay(path);
        }
    });
    overlayRow->addWidget(overlayLoadButton);
    auto* overlayClearButton =
        new QPushButton(QStringLiteral("Clear"), rightPanel);
    overlayClearButton->setObjectName(QStringLiteral("simOverlayClear"));
    overlayClearButton->setToolTip(QStringLiteral("Unload the overlay set"));
    connect(overlayClearButton, &QPushButton::clicked, this,
            &SimTelemetryView::ClearOverlay);
    overlayRow->addWidget(overlayClearButton);
    overlayRow->addWidget(new QLabel(QStringLiteral("offset (s)"), rightPanel));
    overlayOffsetSpin_ = new QDoubleSpinBox(rightPanel);
    overlayOffsetSpin_->setObjectName(QStringLiteral("simOverlayOffsetSpin"));
    overlayOffsetSpin_->setRange(-100000.0, 100000.0);
    overlayOffsetSpin_->setDecimals(3);
    overlayOffsetSpin_->setSingleStep(0.1);
    overlayOffsetSpin_->setToolTip(QStringLiteral(
        "Shift the overlay reference against the primary time axis"));
    connect(overlayOffsetSpin_,
            qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this](double value) {
                if (!overlaySyncGuard_) {
                    SetOverlayOffset(value);
                }
            });
    overlayRow->addWidget(overlayOffsetSpin_);
    overlayOffsetSlider_ = new QSlider(Qt::Horizontal, rightPanel);
    overlayOffsetSlider_->setObjectName(QStringLiteral("simOverlayOffsetSlider"));
    overlayOffsetSlider_->setRange(-600, 600);  // ±6 s in centiseconds; widened on load
    overlayOffsetSlider_->setValue(0);
    connect(overlayOffsetSlider_, &QSlider::valueChanged, this,
            &SimTelemetryView::ApplyOverlayOffsetFromSlider);
    overlayRow->addWidget(overlayOffsetSlider_, 1);
    overlayInfo_ = new QLabel(rightPanel);
    overlayInfo_->setObjectName(QStringLiteral("simOverlayInfo"));
    overlayInfo_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    overlayRow->addWidget(overlayInfo_);
    rightLayout->addLayout(overlayRow);
    UpdateOverlayInfo();

    // Plot pane stack: a vertical splitter (non-collapsible panes whose px
    // sizes persist as paneHeights); panes reorder by title-strip drag.
    plotsSplitter_ = new QSplitter(Qt::Vertical, rightPanel);
    plotsSplitter_->setObjectName(QStringLiteral("simPlotSplitter"));
    plotsSplitter_->setChildrenCollapsible(false);
    // Drop strip drawn at the insertion boundary during a reorder drag
    // (overlaid on this widget, never a splitter child — that would make IT
    // a pane).
    dropIndicator_ = new QFrame(this);
    dropIndicator_->setFixedHeight(3);
    dropIndicator_->setAutoFillBackground(true);
    dropIndicator_->setStyleSheet(QStringLiteral("background-color: #e8963c;"));
    dropIndicator_->hide();
    rightLayout->addWidget(plotsSplitter_, 1);
    splitter->addWidget(rightPanel);

    for (int i = 0; i < 3; ++i) {
        plots_.push_back(MakePlot());
    }
    RetitlePlots();
    UpdateGraphCount();

    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({370, 620});

    // Splitter-handle drags change the persisted pane heights.
    connect(plotsSplitter_, &QSplitter::splitterMoved,
            this, [this] { ScheduleLayoutSave(); });

    contentLayout->addWidget(splitter, 1);

    statusLabel_ = new QLabel(content);
    statusLabel_->setObjectName(QStringLiteral("simSessionInfo"));
    statusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    contentLayout->addWidget(statusLabel_);

    // Cursor/window breadcrumb; doubles as the headless smoke read dump.
    cursorInfo_ = new QLabel(content);
    cursorInfo_->setObjectName(QStringLiteral("simCursorInfo"));
    contentLayout->addWidget(cursorInfo_);
    UpdateCursorInfo();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(stack_);

    // Write-through debounce for the per-session-class layout autosave
    // (combo edits, graph add/remove); ~500 ms quiet window, then save.
    layoutSaveTimer_ = new QTimer(this);
    layoutSaveTimer_->setSingleShot(true);
    layoutSaveTimer_->setInterval(500);
    connect(layoutSaveTimer_, &QTimer::timeout, this, &SimTelemetryView::SaveLayout);
}

SimTelemetryView::~SimTelemetryView() {
    // A change pending in the debounce window must still land on teardown
    // (the runtime side likewise saves from MainWindow::closeEvent).
    if (layoutSaveTimer_->isActive()) {
        layoutSaveTimer_->stop();
        SaveLayout();
    }
}

NodeGUI::runtime::SignalPlotWidget* SimTelemetryView::MakePlot() {
    auto* plot = new runtime::SignalPlotWidget(
        QStringLiteral("Graph %1").arg(plots_.size() + 1), plotsSplitter_);
    plot->SetStore(&store_);
    plot->setMinimumHeight(kMinPaneHeightPx);
    // The slider row owns the view window (mirrors the runtime surface).
    plot->SetViewSecondsCap(viewCapTsec_);
    plot->SetViewSeconds(viewSlider_ ? viewSlider_->value() / 10.0 : 10.0);
    connect(plot, &runtime::SignalPlotWidget::cursorDragged,
            this, &SimTelemetryView::DragCursorTo);
    connect(plot, &runtime::SignalPlotWidget::cursorClearRequested,
            this, &SimTelemetryView::ClearCursor);
    connect(plot, &runtime::SignalPlotWidget::panRequested,
            this, &SimTelemetryView::PanWindowBy);
    connect(plot, &runtime::SignalPlotWidget::paneDragStarted, this,
            [this] { dropIndicator_->raise(); });
    connect(plot, &runtime::SignalPlotWidget::paneDragMoved, this,
            [this, plot](int globalY) { OnPaneDragMoved(plot, globalY); });
    connect(plot, &runtime::SignalPlotWidget::paneDragEnded, this,
            [this, plot](int globalY) { OnPaneDragEnded(plot, globalY); });
    // A graph added under a pinned cursor joins the same hold.
    if (hasCursor_) {
        plot->SetCursor(true, cursorTsec_);
    }
    paneOrder_.push_back(nextPaneId_++);
    plotsSplitter_->addWidget(plot);
    return plot;
}

void SimTelemetryView::RetitlePlots() {
    for (std::size_t i = 0; i < plots_.size(); ++i) {
        plots_[i]->SetTitle(QStringLiteral("Graph %1").arg(i + 1));
    }
}

bool SimTelemetryView::MovePane(int from, int to) {
    const int n = static_cast<int>(plots_.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) {
        return false;
    }
    auto* plot = plots_[static_cast<std::size_t>(from)];
    plots_.erase(plots_.begin() + from);
    plots_.insert(plots_.begin() + to, plot);
    // Assignments are PER SLOT: the pane's signal set moves with it, and the
    // table dropdown labels (G1..Gn) follow the new slot order — nothing is
    // lost. Same permutation on the three parallel states.
    QStringList set = graphSignals_[static_cast<std::size_t>(from)];
    graphSignals_.erase(graphSignals_.begin() + from);
    graphSignals_.insert(graphSignals_.begin() + to, set);
    const int id = paneOrder_[static_cast<std::size_t>(from)];
    paneOrder_.erase(paneOrder_.begin() + from);
    paneOrder_.insert(paneOrder_.begin() + to, id);
    plotsSplitter_->insertWidget(to, plot);
    RetitlePlots();
    RebuildSignalTable();
    ScheduleLayoutSave();
    return true;
}

bool SimTelemetryView::SetPlotSizes(const QList<int>& px) {
    if (px.size() != static_cast<int>(plots_.size())) {
        return false;
    }
    QList<int> clamped = px;
    for (int& size : clamped) {
        size = std::max(kMinPaneHeightPx, size);
    }
    plotsSplitter_->setSizes(clamped);
    ScheduleLayoutSave();
    return true;
}

QString SimTelemetryView::DumpPlotPanes() const {
    QStringList orderParts;
    for (int id : paneOrder_) {
        orderParts.push_back(QString::number(id));
    }
    QStringList sizeParts;
    for (int size : plotsSplitter_ ? plotsSplitter_->sizes() : QList<int>{}) {
        sizeParts.push_back(QString::number(size));
    }
    QStringList titleParts;
    for (const auto* plot : plots_) {
        titleParts.push_back(plot->Title());
    }
    return QStringLiteral("order=%1 heights=%2 titles=%3")
        .arg(orderParts.join(QStringLiteral(",")),
             sizeParts.join(QStringLiteral(",")),
             titleParts.join(QStringLiteral(",")));
}

void SimTelemetryView::OnPaneDragMoved(NodeGUI::runtime::SignalPlotWidget* pane,
                                       int globalY) {
    const int y = plotsSplitter_->mapFromGlobal(QPoint(0, globalY)).y();
    // Insertion boundary count among the OTHER panes: the dragged pane
    // would land before the first other pane whose center is below the
    // cursor. Positions come from the live layout (heights can differ).
    QList<QWidget*> others;
    for (auto* plot : plots_) {
        if (plot != pane) {
            others.push_back(plot);
        }
    }
    int boundary = 0;
    while (boundary < others.size()
           && others[boundary]->geometry().center().y() < y) {
        ++boundary;
    }
    int indicatorY = boundary < others.size()
                         ? others[boundary]->geometry().top()
                         : (others.isEmpty()
                                ? 0
                                : others.last()->geometry().bottom() + 1);
    indicatorY = std::clamp(indicatorY, 0,
                            std::max(0, plotsSplitter_->height() - 3));
    // dropIndicator_ is parented to `this`; map the splitter coordinate up.
    const QPoint topLeft = plotsSplitter_->mapTo(this, QPoint(0, indicatorY));
    dropIndicator_->setGeometry(plotsSplitter_->mapTo(this, QPoint()).x(),
                                topLeft.y(), plotsSplitter_->width(), 3);
    dropIndicator_->show();
    dropIndicator_->raise();
}

void SimTelemetryView::OnPaneDragEnded(NodeGUI::runtime::SignalPlotWidget* pane,
                                       int globalY) {
    dropIndicator_->hide();
    const auto it = std::find(plots_.begin(), plots_.end(), pane);
    if (it == plots_.end()) {
        return;
    }
    const int from = static_cast<int>(it - plots_.begin());
    const int y = plotsSplitter_->mapFromGlobal(QPoint(0, globalY)).y();
    QList<QWidget*> others;
    for (auto* plot : plots_) {
        if (plot != pane) {
            others.push_back(plot);
        }
    }
    int boundary = 0;
    while (boundary < others.size()
           && others[boundary]->geometry().center().y() < y) {
        ++boundary;
    }
    // Boundary is an index among the others list, which is exactly the full
    // order with the dragged pane extracted — the index the pane lands at.
    MovePane(from, boundary);
}

void SimTelemetryView::DragCursorTo(double tsec) {
    hasCursor_ = true;
    cursorTsec_ = tsec;
    for (auto* plot : plots_) {
        plot->SetCursor(true, tsec);
    }
    UpdateValueColumn();
    UpdateCursorInfo();
}

void SimTelemetryView::ClearCursor() {
    hasCursor_ = false;
    cursorTsec_ = 0.0;
    for (auto* plot : plots_) {
        plot->SetCursor(false, 0.0);
        // The double-click contract: released cursor finds the tail again.
        plot->FollowTail();
    }
    UpdateValueColumn();
    UpdateCursorInfo();
    UpdateStatusLine();
}

void SimTelemetryView::PanWindowBy(double deltaTsec) {
    for (auto* plot : plots_) {
        plot->PanWindowBy(deltaTsec);
    }
    UpdateCursorInfo();
    UpdateStatusLine();
}

bool SimTelemetryView::PlotWindowTsec(double& startTsec, double& endTsec) const {
    if (!session_ || plots_.empty()) {
        return false;
    }
    startTsec = plots_.front()->WindowStartTsec();
    endTsec = plots_.front()->WindowEndTsec();
    return endTsec > startTsec;
}

bool SimTelemetryView::SparklineSamples(const std::string& key,
                                        double startTsec,
                                        double endTsec,
                                        int maxPoints,
                                        std::vector<QPointF>& out) const {
    out.clear();
    if (!session_ || !(endTsec > startTsec) || maxPoints < 2) {
        return false;
    }
    // Lock-guarded full-history copy (the 30 Hz plot refresh's own read),
    // then decimate the window slice locally — the chip payload stays
    // bounded even on a 63-channel deck.
    std::vector<float> t;
    std::vector<float> y;
    if (!store_.CopyHistoryInto(key, t, y) || t.empty()) {
        return false;
    }
    const auto lo = std::lower_bound(t.begin(), t.end(),
                                     static_cast<float>(startTsec));
    const auto hi = std::upper_bound(t.begin(), t.end(),
                                     static_cast<float>(endTsec));
    const std::size_t count = static_cast<std::size_t>(hi - lo);
    if (count == 0) {
        return false;
    }
    const std::size_t stride =
        std::max<std::size_t>(1, (count + maxPoints - 1) / maxPoints);
    for (auto it = lo; it != hi; it += stride) {
        const std::size_t i = static_cast<std::size_t>(it - t.begin());
        out.emplace_back(static_cast<double>(t[i]), static_cast<double>(y[i]));
    }
    // Edge honesty: the last in-window sample always lands, so the strip's
    // right edge is the true window-edge value.
    const std::size_t last = static_cast<std::size_t>(hi - 1 - t.begin());
    const std::size_t emittedLast =
        static_cast<std::size_t>(lo - t.begin())
        + ((count - 1) / stride) * stride;
    if (emittedLast != last) {
        out.emplace_back(static_cast<double>(t[last]),
                         static_cast<double>(y[last]));
    }
    return true;
}

bool SimTelemetryView::AssignSignalFirstFreeGraph(const QString& name) {
    if (!session_ || !session_->ChannelNames().contains(name)
        || session_->ChannelStats(name).finiteSamples == 0) {
        return false;
    }
    if (AssignedGraph(name) >= 0) {
        return true;  // already on the grid
    }
    int target = -1;
    for (int i = 0; i < static_cast<int>(graphSignals_.size()); ++i) {
        if (graphSignals_[i].isEmpty()) {
            target = i;
            break;
        }
    }
    if (target < 0) {
        target = 0;
    }
    graphSignals_[target].push_back(name);
    ApplyAssignments();
    RebuildSignalTable();
    ScheduleLayoutSave();
    return true;
}

void SimTelemetryView::UpdateCursorInfo() {
    if (!cursorInfo_) {
        return;
    }
    // Window bounds + anchor state of the first plot stand in for the
    // surface; all plots share the same state ops from the hub above.
    const bool panned = !plots_.empty() && !plots_.front()->FollowingTail();
    const QString state = hasCursor_ ? QStringLiteral("held")
                                     : (panned ? QStringLiteral("panned")
                                               : QStringLiteral("following"));
    const QString cursorText =
        hasCursor_ ? QString::number(cursorTsec_, 'f', 3) + QStringLiteral(" s")
                   : QStringLiteral("\u2014");
    QString windowText = QStringLiteral("\u2014");
    if (!plots_.empty()) {
        windowText = QStringLiteral("%1..%2 s")
                         .arg(plots_.front()->WindowStartTsec(), 0, 'f', 1)
                         .arg(plots_.front()->WindowEndTsec(), 0, 'f', 1);
    }
    cursorInfo_->setText(QStringLiteral("cursor %1 · window %2 · %3")
                             .arg(cursorText, windowText, state));
}

void SimTelemetryView::ApplyViewSeconds() {
    const double seconds = viewSlider_ ? viewSlider_->value() / 10.0 : 10.0;
    for (auto* plot : plots_) {
        plot->SetViewSeconds(seconds);
        plot->Refresh();
    }
    UpdateStatusLine();
    UpdateCursorInfo();
}

bool SimTelemetryView::LoadOverlay(const QString& path) {
    SimSessionSeriesSource source;
    QString error;
    if (!source.Load(path, error)) {
        if (overlayInfo_) {
            overlayInfo_->setText(QStringLiteral("overlay load failed: %1").arg(error));
        }
        return false;
    }
    source.RegisterWith(store_);
    overlayName_ = QFileInfo(path).completeBaseName();
    // A new reference starts aligned 1:1 (offset 0), slider widened to its
    // span so the whole overlay range can be phase-swept.
    overlaySyncGuard_ = true;
    if (overlayOffsetSpin_) {
        overlayOffsetSpin_->setValue(0.0);
    }
    if (overlayOffsetSlider_) {
        const int cs =
            std::max(600, static_cast<int>(std::ceil(source.SpanSeconds() * 100.0)));
        overlayOffsetSlider_->setRange(-cs, cs);
        overlayOffsetSlider_->setValue(0);
    }
    overlaySyncGuard_ = false;
    store_.SetOverlayOffset(0.0);
    for (auto* plot : plots_) {
        plot->Refresh();
    }
    UpdateValueColumn();
    UpdateOverlayInfo();
    return true;
}

void SimTelemetryView::SetOverlayOffset(double tsec) {
    store_.SetOverlayOffset(tsec);
    overlaySyncGuard_ = true;
    if (overlayOffsetSpin_) {
        overlayOffsetSpin_->setValue(tsec);
    }
    if (overlayOffsetSlider_) {
        overlayOffsetSlider_->setValue(qRound(tsec * 100.0));
    }
    overlaySyncGuard_ = false;
    for (auto* plot : plots_) {
        plot->Refresh();
    }
    UpdateValueColumn();
    UpdateOverlayInfo();
}

void SimTelemetryView::ApplyOverlayOffsetFromSlider(int centiseconds) {
    if (overlaySyncGuard_) {
        return;
    }
    SetOverlayOffset(centiseconds / 100.0);
}

void SimTelemetryView::ClearOverlay() {
    store_.ClearOverlay();
    overlayName_.clear();
    overlaySyncGuard_ = true;
    if (overlayOffsetSpin_) {
        overlayOffsetSpin_->setValue(0.0);
    }
    if (overlayOffsetSlider_) {
        overlayOffsetSlider_->setRange(-600, 600);
        overlayOffsetSlider_->setValue(0);
    }
    overlaySyncGuard_ = false;
    for (auto* plot : plots_) {
        plot->Refresh();
    }
    UpdateValueColumn();
    UpdateOverlayInfo();
}

void SimTelemetryView::UpdateOverlayInfo() {
    if (!overlayInfo_) {
        return;
    }
    if (!store_.HasOverlay()) {
        overlayInfo_->setText(QStringLiteral("overlay: none"));
        return;
    }
    const double offset = store_.OverlayOffsetTsec();
    overlayInfo_->setText(
        QStringLiteral("overlay %1 \u00b7 %2 ch \u00b7 %3 s \u00b7 offset %4%5 s")
            .arg(overlayName_)
            .arg(store_.OverlayChannelCount())
            .arg(store_.OverlaySpanSeconds(), 0, 'f', 1)
            .arg(offset >= 0.0 ? QStringLiteral("+") : QStringLiteral("-"))
            .arg(std::abs(offset), 0, 'f', 3));
}

void SimTelemetryView::UpdateGraphCount() {
    while (graphSignals_.size() < plots_.size()) {
        graphSignals_.emplace_back();
    }
    if (graphSignals_.size() > plots_.size()) {
        // Removed graphs drop their sets: those signals revert to
        // unassigned (rows re-sync on RebuildSignalTable).
        graphSignals_.resize(plots_.size());
    }
    countLabel_->setText(QStringLiteral("(%1)").arg(plots_.size()));
}

void SimTelemetryView::AddGraph() {
    plots_.push_back(MakePlot());
    RetitlePlots();
    UpdateGraphCount();
    RebuildSignalTable();
    ScheduleLayoutSave();
}

void SimTelemetryView::RemoveLastGraph() {
    if (plots_.size() <= 1) {
        return;
    }
    auto* plot = plots_.back();
    plots_.pop_back();
    paneOrder_.pop_back();
    // Hiding detaches the pane from the splitter distribution ahead of the
    // deferred destruction (QSplitter removes destroyed children itself).
    plot->hide();
    plot->deleteLater();
    RetitlePlots();
    UpdateGraphCount();
    RebuildSignalTable();
    ScheduleLayoutSave();
}

int SimTelemetryView::AssignedGraph(const QString& name) const {
    for (int i = 0; i < static_cast<int>(graphSignals_.size()); ++i) {
        if (graphSignals_[i].contains(name)) {
            return i;
        }
    }
    return -1;
}

bool SimTelemetryView::LoadSession(const QString& path, QString& error) {
    auto session = std::make_unique<SimSession>();
    if (!session->Load(path, error)) {
        return false;
    }
    session_ = std::move(session);
    sessionPath_ = path;

    store_.ClearSession();
    store_.ClearConsole();
    // Recorded sessions play back whole: raise THIS store's retention floor
    // above the session's span and longest channel before PopulateStore —
    // the live Runtime store keeps its 30 s / 12000-sample ring (it never
    // calls SetRetentionLimit).
    int maxChannelSamples = 0;
    for (const QString& name : session_->ChannelNames()) {
        maxChannelSamples = std::max(maxChannelSamples,
                                     session_->ChannelStats(name).samples);
    }
    store_.SetRetentionLimit(static_cast<float>(session_->Meta().durationS) + 1.0f,
                             static_cast<std::size_t>(maxChannelSamples) + 16);
    session_->PopulateStore(store_);

    // Restore the persisted layout for this session class (file stem +
    // channel-set hash) ahead of the carry-over/default chain.
    const bool restoredLayout = TryRestoreLayout();

    // Keep only assignments that still exist in this session; when that
    // leaves every graph empty, fall back to the card-reader defaults
    // (skipped when a restored layout deliberately assigns nothing).
    const QStringList names = session_->ChannelNames();
    for (QStringList& set : graphSignals_) {
        set.erase(std::remove_if(set.begin(), set.end(),
                                 [&names](const QString& name) {
                                     return !names.contains(name);
                                 }),
                  set.end());
    }
    const bool anyAssigned = std::any_of(
        graphSignals_.begin(), graphSignals_.end(),
        [](const QStringList& set) { return !set.isEmpty(); });
    if (!anyAssigned && !restoredLayout) {
        ApplyDefaultAssignments();
    }
    ApplyAssignments();

    // A fresh session drops any pinned cursor/pan: the window follows the
    // new tail until the user drags again (cursor state is not persisted).
    hasCursor_ = false;
    cursorTsec_ = 0.0;
    for (auto* plot : plots_) {
        plot->SetCursor(false, 0.0);
        plot->FollowTail();
    }

    // The window follows the session duration; the slider is the single
    // owner of the plots' view seconds. Long sessions sample the cap (and
    // the slider maximum) up to their full span - a hard 60 s used to make
    // >60 s recordings unreachable in one window.
    viewCapTsec_ = std::max(60.0, session_->Meta().durationS);
    for (auto* plot : plots_) {
        plot->SetViewSecondsCap(viewCapTsec_);
    }
    viewSlider_->setMaximum(std::max(600, qRound(viewCapTsec_ * 10.0)));
    const double viewSeconds =
        std::clamp(session_->Meta().durationS, 0.5, 60.0);
    viewSlider_->setValue(std::clamp(qRound(viewSeconds * 10.0), 5,
                                     viewSlider_->maximum()));
    ApplyViewSeconds();

    RebuildSignalTable();
    UpdateStatusLine();
    stack_->setCurrentIndex(1);
    return true;
}

void SimTelemetryView::ApplyDefaultAssignments() {
    for (int group = 0;
         group < static_cast<int>(std::min(graphSignals_.size(), kDefaultSignals.size()));
         ++group) {
        QStringList& set = graphSignals_[group];
        for (const QString& favorite : kDefaultSignals[group]) {
            const SimChannelStats stats = session_->ChannelStats(favorite);
            if (stats.finiteSamples > 0 && !set.contains(favorite)) {
                set.push_back(favorite);
            }
        }
    }
}

bool SimTelemetryView::TryRestoreLayout() {
    // The save key always tracks the currently loaded session, even when no
    // persisted entry exists yet: later edits land under THIS session class.
    layoutStem_ = SanitizeLayoutStem(QFileInfo(sessionPath_).completeBaseName());
    layoutChannelHash_ = ChannelSetHash(session_->ChannelNames());

    auto settings = MakeSimSettings();
    QString group = LayoutGroupFor(layoutStem_, layoutChannelHash_);
    if (!settings.contains(group + QStringLiteral("/graphCount"))) {
        // A renamed copy of an earlier session keeps the channel set: borrow
        // the layout most recently stored under the same channel-set hash
        // (read-only — the next edit re-keys it to this stem).
        const QString borrowed =
            settings.value(LayoutStemHashKey(layoutChannelHash_)).toString();
        if (!borrowed.isEmpty() && borrowed != layoutStem_) {
            const QString alt = LayoutGroupFor(borrowed, layoutChannelHash_);
            if (settings.contains(alt + QStringLiteral("/graphCount"))) {
                group = alt;
            }
        }
    }
    if (!settings.contains(group + QStringLiteral("/graphCount"))) {
        return false;
    }

    // Same storage shape as RuntimeTab::LoadAutosave: graphCount + graphN
    // (naturally keyed, see ApplyNaturalSets note) inside the group, plus
    // the pane-order permutation and per-slot pane heights when the file
    // was written by a pane-aware build.
    settings.beginGroup(group);
    const int count = std::clamp(
        settings.value(QStringLiteral("graphCount"), 3).toInt(),
        1, kMaxPersistedGraphs);
    std::vector<QStringList> naturalSets;
    for (int i = 0; i < count; ++i) {
        naturalSets.push_back(
            settings.value(QStringLiteral("graph%1").arg(i + 1)).toStringList());
    }
    std::vector<int> ids = ParsePaneOrder(
        settings.value(QStringLiteral("paneOrder")).toString(), count);
    const QList<int> heights = ParsePaneHeights(
        settings.value(QStringLiteral("paneHeights")).toString());
    settings.endGroup();
    if (ids.empty()) {
        for (int i = 0; i < count; ++i) {
            ids.push_back(i);  // legacy/invalid: natural arrangement
        }
    }

    // Resize the plot row through the same widget add/remove semantics as
    // AddGraph / RemoveLastGraph.
    while (static_cast<int>(plots_.size()) < count) {
        plots_.push_back(MakePlot());
    }
    while (static_cast<int>(plots_.size()) > count) {
        auto* plot = plots_.back();
        plots_.pop_back();
        paneOrder_.pop_back();
        plot->hide();
        plot->deleteLater();
    }
    RetitlePlots();
    UpdateGraphCount();

    // Apply the stored permutation: ids map onto the freshly created panes
    // by rank (RuntimeTab's RestorePaneState is the runtime twin).
    if (ids != paneOrder_) {
        std::vector<int> sorted = ids;
        std::sort(sorted.begin(), sorted.end());
        std::vector<NodeGUI::runtime::SignalPlotWidget*> arranged(plots_.size());
        std::vector<int> arrangedOrder(paneOrder_.size());
        for (std::size_t k = 0; k < ids.size(); ++k) {
            const std::size_t rank = static_cast<std::size_t>(
                std::lower_bound(sorted.begin(), sorted.end(), ids[k])
                - sorted.begin());
            arranged[k] = plots_[rank];
            arrangedOrder[k] = paneOrder_[rank];
        }
        plots_ = std::move(arranged);
        paneOrder_ = std::move(arrangedOrder);
        for (auto* plot : plots_) {
            // addWidget on an existing child moves it; replaying in slot
            // order lands the restored arrangement.
            plotsSplitter_->addWidget(plot);
        }
        RetitlePlots();
    }

    // Per-slot sets: slot k takes the natural set of its pane's id rank
    // (the LoadSession prune right after this drops channels the stored
    // layout names but this session does not carry).
    std::vector<int> sortedIds = ids;
    std::sort(sortedIds.begin(), sortedIds.end());
    graphSignals_.assign(naturalSets.size(), QStringList{});
    for (std::size_t k = 0; k < ids.size(); ++k) {
        const std::size_t rank = static_cast<std::size_t>(
            std::lower_bound(sortedIds.begin(), sortedIds.end(), ids[k])
            - sortedIds.begin());
        graphSignals_[k] = naturalSets[rank];
    }

    // Pane heights: splitter px sizes, per pane (they travel on reorder),
    // stored naturally keyed by id rank — slot k's size is the stored rank
    // of its pane's id.
    if (heights.size() == count) {
        QList<int> applied;
        for (std::size_t k = 0; k < ids.size(); ++k) {
            const std::size_t rank = static_cast<std::size_t>(
                std::lower_bound(sortedIds.begin(), sortedIds.end(), ids[k])
                - sortedIds.begin());
            applied.push_back(std::max(kMinPaneHeightPx,
                                       heights[static_cast<int>(rank)]));
        }
        plotsSplitter_->setSizes(applied);
    }
    return true;
}

void SimTelemetryView::ScheduleLayoutSave() {
    if (layoutSaveTimer_ && session_) {
        layoutSaveTimer_->start();  // single-shot: restarts the quiet window
    }
}

void SimTelemetryView::SaveLayout() {
    if (!session_ || layoutStem_.isEmpty() || layoutChannelHash_.isEmpty()) {
        return;
    }
    auto settings = MakeSimSettings();
    // Same graphCount + graphN shape as RuntimeTab::SaveAutosave, one group
    // per session class. Sets are keyed NATURALLY (pane-id rank) so the
    // paneOrder member alone carries a reorder's permutation.
    const auto& slotSets = graphSignals_;
    std::vector<int> order = paneOrder_;
    if (order.size() != slotSets.size()) {
        // Transient skew guard: never write a mismatched order member.
        order.clear();
        for (std::size_t i = 0; i < slotSets.size(); ++i) {
            order.push_back(static_cast<int>(i));
        }
    }
    std::vector<int> sorted = order;
    std::sort(sorted.begin(), sorted.end());
    settings.beginGroup(LayoutGroupFor(layoutStem_, layoutChannelHash_));
    settings.setValue(QStringLiteral("graphCount"),
                      static_cast<int>(slotSets.size()));
    for (std::size_t i = 0; i < slotSets.size(); ++i) {
        const std::size_t rank = static_cast<std::size_t>(
            std::lower_bound(sorted.begin(), sorted.end(), order[i])
            - sorted.begin());
        settings.setValue(QStringLiteral("graph%1").arg(rank + 1),
                          slotSets[i]);
    }
    settings.setValue(QStringLiteral("paneOrder"), JoinIntList(order));
    // Heights are per-PANE like the sets (splitter sizes travel with the
    // pane on reorder), keyed naturally by id rank.
    const QList<int> sizes = plotsSplitter_->sizes();
    std::vector<int> naturalHeights(slotSets.size(), 0);
    for (std::size_t i = 0; i < slotSets.size(); ++i) {
        const std::size_t rank = static_cast<std::size_t>(
            std::lower_bound(sorted.begin(), sorted.end(), order[i])
            - sorted.begin());
        naturalHeights[rank] =
            i < static_cast<std::size_t>(sizes.size()) ? sizes[static_cast<int>(i)] : 0;
    }
    QStringList heightParts;
    for (int height : naturalHeights) {
        heightParts.push_back(QString::number(height));
    }
    settings.setValue(QStringLiteral("paneHeights"),
                      heightParts.join(QStringLiteral(",")));
    settings.endGroup();
    // Pointer for the renamed-copy borrow in TryRestoreLayout.
    settings.setValue(LayoutStemHashKey(layoutChannelHash_), layoutStem_);
}

void SimTelemetryView::ApplyAssignments() {
    for (std::size_t i = 0; i < plots_.size(); ++i) {
        plots_[i]->SetSignals(i < graphSignals_.size()
                                  ? graphSignals_[i]
                                  : QStringList{});
        plots_[i]->Refresh();
    }
    UpdateCursorInfo();
}

void SimTelemetryView::UpdateStatusLine() {
    if (!session_) {
        statusLabel_->clear();
        return;
    }
    const SimSessionMeta& meta = session_->Meta();
    int idleChannels = 0;
    QString firstDataChannel;
    for (const QString& name : session_->ChannelNames()) {
        if (session_->ChannelStats(name).finiteSamples == 0) {
            ++idleChannels;
        } else if (firstDataChannel.isEmpty()) {
            firstDataChannel = name;
        }
    }
    // Retained-span readout, from the store the plots actually read: a
    // recorded session must cover t 0..duration, not just the rolling tail.
    QString retained = QStringLiteral("(no data channels)");
    if (!firstDataChannel.isEmpty()) {
        std::vector<float> t;
        std::vector<float> y;
        if (store_.CopyHistoryInto(firstDataChannel.toStdString(), t, y)
            && !t.empty()) {
            retained = QStringLiteral("t %1..%2 s (%3 samples of %4)")
                           .arg(static_cast<double>(t.front()), 0, 'f', 3)
                           .arg(static_cast<double>(t.back()), 0, 'f', 3)
                           .arg(t.size())
                           .arg(firstDataChannel);
        }
    }
    // Window bounds the plots show right now, from the first plot with data
    // (cursor pans shift them off the tail); unassigned plots fall back to
    // the newest-sample anchor math the leaf used before the pan existed.
    double windowStart;
    double windowEnd;
    const NodeGUI::runtime::SignalPlotWidget* dataPlot = nullptr;
    for (const auto* plot : plots_) {
        if (!plot->Signals().isEmpty()) {
            dataPlot = plot;
            break;
        }
    }
    if (dataPlot) {
        windowStart = dataPlot->WindowStartTsec();
        windowEnd = dataPlot->WindowEndTsec();
    } else {
        const double windowSeconds =
            viewSlider_ ? viewSlider_->value() / 10.0 : 10.0;
        windowEnd = meta.durationS;
        windowStart = windowEnd - windowSeconds;
    }
    statusLabel_->setText(QStringLiteral(
        "%1 frames · %2 channels (%3 all-null) · %4 s · retained %5 · window %6..%7 s · plant: %8 · %9")
            .arg(meta.telemetryFrames)
            .arg(session_->ChannelNames().size())
            .arg(idleChannels)
            .arg(meta.durationS, 0, 'f', 3)
            .arg(retained,
                 QString::number(windowStart, 'f', 1),
                 QString::number(windowEnd, 'f', 1),
                 meta.plant.isEmpty() ? QStringLiteral("(no stamp)")
                                      : meta.plant,
                 sessionPath_));
}

void SimTelemetryView::UpdateValueColumn() {
    // Header tracks the column's source: session-final while no cursor is
    // pinned ("Last"), value-at-cursor while one is ("Cursor").
    if (auto* headerItem = signalTable_->horizontalHeaderItem(kValueColumn)) {
        headerItem->setText(hasCursor_ ? QStringLiteral("Cursor")
                                       : QStringLiteral("Last"));
    }
    if (!session_) {
        return;
    }
    for (int row = 0; row < signalTable_->rowCount(); ++row) {
        const QString name = signalTable_->item(row, kSignalColumn)->text();
        auto* valueItem = signalTable_->item(row, kValueColumn);
        const SimChannelStats stats = session_->ChannelStats(name);
        if (stats.finiteSamples == 0) {
            valueItem->setText(QStringLiteral("null"));
            continue;
        }
        float value = 0.0f;
        if (hasCursor_
            && store_.ValueAt(name.toStdString(),
                              static_cast<float>(cursorTsec_), value)) {
            // Delta readout at the cursor: when the overlay set carries this
            // channel, the cell extends to "live (ref R, Δ ±D)" — the ref
            // sampled under the current overlay offset. Channels the overlay
            // does not name keep the wave-5 plain value, byte-identical.
            float ref = 0.0f;
            if (store_.OverlayValueAt(name.toStdString(),
                                      static_cast<float>(cursorTsec_), ref)) {
                const double delta = static_cast<double>(value) - ref;
                valueItem->setText(
                    QStringLiteral("%1 (ref %2, \u0394 %3%4)")
                        .arg(QString::number(value, 'g', 6),
                             QString::number(ref, 'g', 6),
                             delta >= 0.0 ? QStringLiteral("+")
                                          : QStringLiteral("-"),
                             QString::number(std::abs(delta), 'f', 3)));
            } else {
                valueItem->setText(QString::number(value, 'g', 6));
            }
        } else {
            valueItem->setText(QString::number(stats.lastFinite, 'g', 6));
        }
    }
}

void SimTelemetryView::RebuildSignalTable() {
    if (!session_) {
        signalTable_->setRowCount(0);
        return;
    }

    QStringList names;
    const QString filter = filterEdit_->text();
    for (const QString& name : session_->ChannelNames()) {
        if (filter.isEmpty() || name.contains(filter, Qt::CaseInsensitive)) {
            names.push_back(name);
        }
    }

    QStringList current;
    current.reserve(signalTable_->rowCount());
    for (int row = 0; row < signalTable_->rowCount(); ++row) {
        current.push_back(signalTable_->item(row, kSignalColumn)->text());
    }
    const bool sameNames = current == names;
    if (sameNames) {
        // Just re-sync the assignment dropdowns (graph count changes and
        // load-time pruning land here without a full rebuild).
        rebuildingTable_ = true;
        for (int row = 0; row < signalTable_->rowCount(); ++row) {
            auto* combo = qobject_cast<QComboBox*>(
                signalTable_->cellWidget(row, kTargetColumn));
            if (!combo) {
                continue;
            }
            const QString name = signalTable_->item(row, kSignalColumn)->text();
            combo->clear();
            combo->addItem(QStringLiteral("\u2014"));
            for (int i = 0; i < static_cast<int>(graphSignals_.size()); ++i) {
                combo->addItem(QStringLiteral("G%1").arg(i + 1));
            }
            combo->setCurrentIndex(AssignedGraph(name) + 1);
        }
        rebuildingTable_ = false;
        UpdateValueColumn();
        return;
    }

    rebuildingTable_ = true;
    signalTable_->setRowCount(0);
    signalTable_->setRowCount(static_cast<int>(names.size()));
    for (int row = 0; row < names.size(); ++row) {
        const QString& name = names[row];
        const SimChannelStats stats = session_->ChannelStats(name);
        const bool dead = stats.finiteSamples == 0;

        auto* combo = new QComboBox(signalTable_);
        combo->setObjectName(QStringLiteral("target.%1").arg(name));
        combo->addItem(QStringLiteral("\u2014"));
        for (int i = 0; i < static_cast<int>(graphSignals_.size()); ++i) {
            combo->addItem(QStringLiteral("G%1").arg(i + 1));
        }
        combo->setCurrentIndex(AssignedGraph(name) + 1);
        combo->setEnabled(!dead);
        if (dead) {
            combo->setToolTip(QStringLiteral(
                "Every sample in this channel is null — nothing to plot."));
        }
        signalTable_->setCellWidget(row, kTargetColumn, combo);
        connect(combo, &QComboBox::currentIndexChanged, this,
                [this, row](int index) {
                    if (rebuildingTable_) {
                        return;
                    }
                    const QString key =
                        signalTable_->item(row, kSignalColumn)->text();
                    for (QStringList& set : graphSignals_) {
                        set.removeAll(key);
                    }
                    if (index > 0 && index <= static_cast<int>(graphSignals_.size())) {
                        graphSignals_[index - 1].push_back(key);
                    }
                    ApplyAssignments();
                    ScheduleLayoutSave();
                });

        auto* nameItem = new QTableWidgetItem(name);
        nameItem->setFlags(Qt::ItemIsEnabled);
        auto* valueItem = new QTableWidgetItem;
        valueItem->setFlags(Qt::ItemIsEnabled);
        valueItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (dead) {
            const QBrush gray(QColor(140, 140, 140));
            nameItem->setForeground(gray);
            valueItem->setForeground(gray);
            nameItem->setToolTip(QStringLiteral(
                "Every sample in this channel is null."));
            valueItem->setText(QStringLiteral("null"));
        } else {
            valueItem->setText(QString::number(stats.lastFinite, 'g', 6));
        }
        signalTable_->setItem(row, kSignalColumn, nameItem);
        signalTable_->setItem(row, kValueColumn, valueItem);
    }
    rebuildingTable_ = false;
    UpdateValueColumn();
}

}  // namespace NodeGUI::sim
