#include "TelemetryPanel.h"

#include "RuntimeController.h"
#include "SignalPlotWidget.h"
#include "sim/SimSessionSeriesSource.h"

#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace NodeGUI::runtime {

namespace {

// Lower clamp for persisted pane heights (splitter px sizes).
constexpr int kMinPaneHeightPx = 60;

QSettings MakeOverlaySettings() {
    return QSettings(QStringLiteral("RTE"), QStringLiteral("RTEStudio"));
}

}  // namespace

TelemetryPanel::TelemetryPanel(RuntimeController* controller, QWidget* parent)
    : QWidget(parent)
    , controller_(controller) {
    auto* topRow = new QHBoxLayout;
    topRow->setContentsMargins(4, 2, 4, 2);
    topRow->addWidget(new QLabel(QStringLiteral("Graphs"), this));
    countLabel_ = new QLabel(this);
    topRow->addWidget(countLabel_);
    topRow->addStretch(1);
    auto* addButton = new QPushButton(QStringLiteral("+ Graph"), this);
    addButton->setObjectName(QStringLiteral("rtAddGraph"));
    addButton->setToolTip(QStringLiteral("Add another telemetry graph"));
    connect(addButton, &QPushButton::clicked, this, &TelemetryPanel::AddGraph);
    topRow->addWidget(addButton);
    auto* removeButton = new QPushButton(QStringLiteral("- Graph"), this);
    removeButton->setObjectName(QStringLiteral("rtRemoveGraph"));
    removeButton->setToolTip(QStringLiteral(
        "Remove the last graph (its signals unassign)"));
    connect(removeButton, &QPushButton::clicked, this,
            &TelemetryPanel::RemoveLastGraph);
    topRow->addWidget(removeButton);
    // Cursor/window breadcrumb; doubles as the headless smoke read dump.
    cursorInfo_ = new QLabel(this);
    cursorInfo_->setObjectName(QStringLiteral("rtCursorInfo"));
    topRow->addWidget(cursorInfo_);

    // Recorded-session overlay row (backlog P1-8): load/clear + offset
    // (spin seconds, slider centiseconds) + the rtOverlayInfo dump label.
    // Nothing is persisted — same convention as the cursor.
    auto* overlayRow = new QHBoxLayout;
    overlayRow->setContentsMargins(4, 0, 4, 2);
    overlayRow->addWidget(new QLabel(QStringLiteral("Overlay"), this));
    auto* overlayLoadButton =
        new QPushButton(QStringLiteral("Load\u2026"), this);
    overlayLoadButton->setObjectName(QStringLiteral("rtOverlayLoad"));
    overlayLoadButton->setToolTip(QStringLiteral(
        "Load a recorded session as an overlay reference (dashed traces)"));
    connect(overlayLoadButton, &QPushButton::clicked, this, [this] {
        const QString startDir = MakeOverlaySettings()
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
    auto* overlayClearButton = new QPushButton(QStringLiteral("Clear"), this);
    overlayClearButton->setObjectName(QStringLiteral("rtOverlayClear"));
    overlayClearButton->setToolTip(QStringLiteral("Unload the overlay set"));
    connect(overlayClearButton, &QPushButton::clicked, this,
            &TelemetryPanel::ClearOverlay);
    overlayRow->addWidget(overlayClearButton);
    overlayRow->addWidget(new QLabel(QStringLiteral("offset (s)"), this));
    overlayOffsetSpin_ = new QDoubleSpinBox(this);
    overlayOffsetSpin_->setObjectName(QStringLiteral("rtOverlayOffsetSpin"));
    overlayOffsetSpin_->setRange(-100000.0, 100000.0);
    overlayOffsetSpin_->setDecimals(3);
    overlayOffsetSpin_->setSingleStep(0.1);
    overlayOffsetSpin_->setToolTip(QStringLiteral(
        "Shift the overlay reference against the live time axis"));
    connect(overlayOffsetSpin_,
            qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, [this](double value) {
                if (!overlaySyncGuard_) {
                    SetOverlayOffset(value);
                }
            });
    overlayRow->addWidget(overlayOffsetSpin_);
    overlayOffsetSlider_ = new QSlider(Qt::Horizontal, this);
    overlayOffsetSlider_->setObjectName(QStringLiteral("rtOverlayOffsetSlider"));
    overlayOffsetSlider_->setRange(-600, 600);  // ±6 s; widened on load
    overlayOffsetSlider_->setValue(0);
    connect(overlayOffsetSlider_, &QSlider::valueChanged, this,
            &TelemetryPanel::ApplyOverlayOffsetFromSlider);
    overlayRow->addWidget(overlayOffsetSlider_, 1);
    overlayRow->addStretch(1);
    overlayInfo_ = new QLabel(this);
    overlayInfo_->setObjectName(QStringLiteral("rtOverlayInfo"));
    overlayInfo_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    overlayRow->addWidget(overlayInfo_);

    plotsSplitter_ = new QSplitter(Qt::Vertical, this);
    plotsSplitter_->setObjectName(QStringLiteral("rtPlotSplitter"));
    plotsSplitter_->setChildrenCollapsible(false);

    // Drop-position strip for reorder drags: overlaid on this panel at the
    // insertion boundary (never a splitter child — that would make IT a
    // pane).
    dropIndicator_ = new QFrame(this);
    dropIndicator_->setFixedHeight(3);
    dropIndicator_->setAutoFillBackground(true);
    dropIndicator_->setStyleSheet(
        QStringLiteral("background-color: #e8963c;"));
    dropIndicator_->hide();

    for (int i = 0; i < 3; ++i) {
        plots_.push_back(MakePlot());
    }
    RetitlePlots();
    countLabel_->setText(QStringLiteral("(%1)").arg(plots_.size()));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(topRow);
    layout->addLayout(overlayRow);
    layout->addWidget(plotsSplitter_, 1);

    // Splitter-handle drags change the persisted pane heights.
    connect(plotsSplitter_, &QSplitter::splitterMoved,
            this, &TelemetryPanel::layoutChanged);

    connect(controller_, &RuntimeController::storeChanged,
            this, &TelemetryPanel::OnStoreChanged);
    UpdateCursorInfo();
    UpdateOverlayInfo();
}

SignalPlotWidget* TelemetryPanel::MakePlot() {
    auto* plot = new SignalPlotWidget(
        QStringLiteral("Graph %1").arg(plots_.size() + 1), plotsSplitter_);
    plot->SetStore(&controller_->Store());
    plot->setMinimumHeight(kMinPaneHeightPx);
    connect(plot, &SignalPlotWidget::cursorDragged,
            this, &TelemetryPanel::DragCursorTo);
    connect(plot, &SignalPlotWidget::cursorClearRequested,
            this, &TelemetryPanel::ClearCursor);
    connect(plot, &SignalPlotWidget::panRequested,
            this, &TelemetryPanel::PanWindowBy);
    connect(plot, &SignalPlotWidget::paneDragStarted, this,
            [this] { dropIndicator_->raise(); });
    connect(plot, &SignalPlotWidget::paneDragMoved, this,
            [this, plot](int globalY) { OnPaneDragMoved(plot, globalY); });
    connect(plot, &SignalPlotWidget::paneDragEnded, this,
            [this, plot](int globalY) { OnPaneDragEnded(plot, globalY); });
    // A graph added under a pinned cursor joins the same hold.
    if (hasCursor_) {
        plot->SetCursor(true, cursorTsec_);
    }
    paneOrder_.push_back(nextPaneId_++);
    plotsSplitter_->addWidget(plot);
    return plot;
}

void TelemetryPanel::RetitlePlots() {
    for (std::size_t i = 0; i < plots_.size(); ++i) {
        plots_[i]->SetTitle(QStringLiteral("Graph %1").arg(i + 1));
    }
}

void TelemetryPanel::AddGraph() {
    plots_.push_back(MakePlot());
    RetitlePlots();
    countLabel_->setText(QStringLiteral("(%1)").arg(plots_.size()));
    emit graphCountChanged(GraphCount());
    emit layoutChanged();
}

void TelemetryPanel::RemoveLastGraph() {
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
    countLabel_->setText(QStringLiteral("(%1)").arg(plots_.size()));
    emit graphCountChanged(GraphCount());
    emit layoutChanged();
}

void TelemetryPanel::SetGraphCount(int count) {
    count = std::max(1, count);
    while (GraphCount() < count) {
        plots_.push_back(MakePlot());
    }
    while (GraphCount() > count) {
        auto* plot = plots_.back();
        plots_.pop_back();
        paneOrder_.pop_back();
        plot->hide();
        plot->deleteLater();
    }
    RetitlePlots();
    countLabel_->setText(QStringLiteral("(%1)").arg(plots_.size()));
    emit graphCountChanged(GraphCount());
}

void TelemetryPanel::SetGraphSignals(const std::vector<QStringList>& sets) {
    for (std::size_t i = 0; i < plots_.size(); ++i) {
        plots_[i]->SetSignals(i < sets.size() ? sets[i] : QStringList{});
    }
}

void TelemetryPanel::SetViewSeconds(double seconds) {
    for (auto* plot : plots_) {
        plot->SetViewSeconds(seconds);
    }
    UpdateCursorInfo();
}

bool TelemetryPanel::MovePane(int from, int to) {
    const int n = GraphCount();
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) {
        return false;
    }
    auto* plot = plots_[static_cast<std::size_t>(from)];
    plots_.erase(plots_.begin() + from);
    plots_.insert(plots_.begin() + to, plot);
    const int id = paneOrder_[static_cast<std::size_t>(from)];
    paneOrder_.erase(paneOrder_.begin() + from);
    paneOrder_.insert(paneOrder_.begin() + to, id);
    plotsSplitter_->insertWidget(to, plot);
    RetitlePlots();
    // Assignments are per slot: the table permutes its sets in step
    // (RuntimeTab forwarding), which answers SetGraphSignals back here.
    emit panesReordered(from, to);
    emit layoutChanged();
    return true;
}

bool TelemetryPanel::SetPlotSizes(const QList<int>& px) {
    if (px.size() != GraphCount()) {
        return false;
    }
    QList<int> clamped = px;
    for (int& size : clamped) {
        size = std::max(kMinPaneHeightPx, size);
    }
    plotsSplitter_->setSizes(clamped);
    emit layoutChanged();
    return true;
}

QList<int> TelemetryPanel::PaneSizes() const {
    return plotsSplitter_ ? plotsSplitter_->sizes() : QList<int>{};
}

void TelemetryPanel::RestorePaneState(const std::vector<int>& order,
                                      const QList<int>& heights) {
    const int n = GraphCount();
    // Validate the persisted pane-id list: exact length and all-unique.
    // Anything legacy/foreign keeps the natural (creation) order.
    std::vector<int> ids = order;
    if (static_cast<int>(ids.size()) == n) {
        std::vector<int> sorted = ids;
        std::sort(sorted.begin(), sorted.end());
        if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
            ids = paneOrder_;
        }
    } else {
        ids = paneOrder_;
    }
    std::vector<int> sorted = ids;
    std::sort(sorted.begin(), sorted.end());
    if (ids != paneOrder_) {
        // Rank semantics: the persisted ids map onto the freshly numbered
        // panes by their rank in the sorted id list, so slot k takes the
        // freshly created pane whose rank the stored permutation names.
        std::vector<SignalPlotWidget*> arranged(plots_.size());
        std::vector<int> arrangedOrder(paneOrder_.size());
        for (int slot = 0; slot < n; ++slot) {
            const int rank = static_cast<int>(
                std::lower_bound(sorted.begin(), sorted.end(),
                                 ids[static_cast<std::size_t>(slot)])
                - sorted.begin());
            arranged[static_cast<std::size_t>(slot)] =
                plots_[static_cast<std::size_t>(rank)];
            arrangedOrder[static_cast<std::size_t>(slot)] =
                paneOrder_[static_cast<std::size_t>(rank)];
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
    // Heights are per-PANE like the sets (QSplitter sizes travel with the
    // pane on reorder): heights come in naturally keyed by id rank, so the
    // size applied to slot k is the stored rank of its pane's id.
    if (heights.size() == n) {
        QList<int> applied;
        for (int slot = 0; slot < n; ++slot) {
            const auto rankIt =
                std::lower_bound(sorted.begin(), sorted.end(),
                                 ids[static_cast<std::size_t>(slot)]);
            if (rankIt == sorted.end()) {
                return;
            }
            applied.push_back(
                std::max(kMinPaneHeightPx,
                         heights[static_cast<int>(rankIt - sorted.begin())]));
        }
        plotsSplitter_->setSizes(applied);
    }
}

QString TelemetryPanel::DumpPlotPanes() const {
    QStringList orderParts;
    for (int id : paneOrder_) {
        orderParts.push_back(QString::number(id));
    }
    QStringList sizeParts;
    for (int size : PaneSizes()) {
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

void TelemetryPanel::OnPaneDragMoved(SignalPlotWidget* pane, int globalY) {
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
    int indicatorY = 0;
    while (boundary < others.size()
           && others[boundary]->geometry().center().y() < y) {
        ++boundary;
    }
    indicatorY = boundary < others.size()
                     ? others[boundary]->geometry().top()
                     : (others.isEmpty()
                            ? 0
                            : others.last()->geometry().bottom() + 1);
    indicatorY = std::clamp(indicatorY, 0,
                            std::max(0, plotsSplitter_->height() - 3));
    dropIndicator_->setGeometry(plotsSplitter_->x(), plotsSplitter_->y() + indicatorY,
                                plotsSplitter_->width(), 3);
    dropIndicator_->show();
    dropIndicator_->raise();
}

void TelemetryPanel::OnPaneDragEnded(SignalPlotWidget* pane, int globalY) {
    dropIndicator_->hide();
    const int from = static_cast<int>(
        std::find(plots_.begin(), plots_.end(), pane) - plots_.begin());
    if (from == GraphCount() || pane != plots_[static_cast<std::size_t>(from)]) {
        return;
    }
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
    // order with the dragged pane extracted — so it is the index the pane
    // lands at.
    MovePane(from, boundary);
}

void TelemetryPanel::DragCursorTo(double tsec) {
    hasCursor_ = true;
    cursorTsec_ = tsec;
    for (auto* plot : plots_) {
        plot->SetCursor(true, tsec);
    }
    emit cursorChanged(true, tsec);
    UpdateCursorInfo();
}

void TelemetryPanel::ClearCursor() {
    hasCursor_ = false;
    cursorTsec_ = 0.0;
    for (auto* plot : plots_) {
        plot->SetCursor(false, 0.0);
        // The double-click contract: released cursor finds the tail again
        // (also the runtime resume-following path).
        plot->FollowTail();
    }
    emit cursorChanged(false, 0.0);
    UpdateCursorInfo();
}

void TelemetryPanel::PanWindowBy(double deltaTsec) {
    for (auto* plot : plots_) {
        plot->PanWindowBy(deltaTsec);
    }
    UpdateCursorInfo();
}

bool TelemetryPanel::LoadOverlay(const QString& path) {
    sim::SimSessionSeriesSource source;
    QString error;
    if (!source.Load(path, error)) {
        if (overlayInfo_) {
            overlayInfo_->setText(QStringLiteral("overlay load failed: %1").arg(error));
        }
        return false;
    }
    source.RegisterWith(controller_->Store());
    overlayName_ = QFileInfo(path).completeBaseName();
    // A new reference starts aligned 1:1, slider widened to its span.
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
    controller_->Store().SetOverlayOffset(0.0);
    for (auto* plot : plots_) {
        plot->Refresh();
    }
    UpdateOverlayInfo();
    return true;
}

void TelemetryPanel::SetOverlayOffset(double tsec) {
    controller_->Store().SetOverlayOffset(tsec);
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
    UpdateOverlayInfo();
}

void TelemetryPanel::ApplyOverlayOffsetFromSlider(int centiseconds) {
    if (overlaySyncGuard_) {
        return;
    }
    SetOverlayOffset(centiseconds / 100.0);
}

void TelemetryPanel::ClearOverlay() {
    controller_->Store().ClearOverlay();
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
    UpdateOverlayInfo();
}

void TelemetryPanel::UpdateOverlayInfo() {
    if (!overlayInfo_) {
        return;
    }
    const TelemetryStore& store = controller_->Store();
    if (!store.HasOverlay()) {
        overlayInfo_->setText(QStringLiteral("overlay: none"));
        return;
    }
    const double offset = store.OverlayOffsetTsec();
    overlayInfo_->setText(
        QStringLiteral("overlay %1 \u00b7 %2 ch \u00b7 %3 s \u00b7 offset %4%5 s")
            .arg(overlayName_)
            .arg(store.OverlayChannelCount())
            .arg(store.OverlaySpanSeconds(), 0, 'f', 1)
            .arg(offset >= 0.0 ? QStringLiteral("+") : QStringLiteral("-"))
            .arg(std::abs(offset), 0, 'f', 3));
}

void TelemetryPanel::UpdateCursorInfo() {
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

void TelemetryPanel::OnStoreChanged() {
    for (auto* plot : plots_) {
        plot->Refresh();
    }
    UpdateCursorInfo();
}

}  // namespace NodeGUI::runtime
