#include "TelemetryPanel.h"

#include "RuntimeController.h"
#include "SignalPlotWidget.h"
#include "sim/SimSessionSeriesSource.h"

#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QVBoxLayout>

#include <cmath>

namespace NodeGUI::runtime {

namespace {

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

    plotsContainer_ = new QWidget(this);
    plotsLayout_ = new QVBoxLayout(plotsContainer_);
    plotsLayout_->setContentsMargins(0, 0, 0, 0);
    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    scroll_->setWidget(plotsContainer_);

    for (int i = 0; i < 3; ++i) {
        plots_.push_back(MakePlot());
    }
    countLabel_->setText(QStringLiteral("(%1)").arg(plots_.size()));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(topRow);
    layout->addLayout(overlayRow);
    layout->addWidget(scroll_, 1);

    connect(controller_, &RuntimeController::storeChanged,
            this, &TelemetryPanel::OnStoreChanged);
    UpdateCursorInfo();
    UpdateOverlayInfo();
}

SignalPlotWidget* TelemetryPanel::MakePlot() {
    auto* plot = new SignalPlotWidget(
        QStringLiteral("Graph %1").arg(plots_.size() + 1), plotsContainer_);
    plot->SetStore(&controller_->Store());
    plot->setMinimumHeight(200);
    connect(plot, &SignalPlotWidget::cursorDragged,
            this, &TelemetryPanel::DragCursorTo);
    connect(plot, &SignalPlotWidget::cursorClearRequested,
            this, &TelemetryPanel::ClearCursor);
    connect(plot, &SignalPlotWidget::panRequested,
            this, &TelemetryPanel::PanWindowBy);
    // A graph added under a pinned cursor joins the same hold.
    if (hasCursor_) {
        plot->SetCursor(true, cursorTsec_);
    }
    plotsLayout_->addWidget(plot);
    return plot;
}

void TelemetryPanel::AddGraph() {
    plots_.push_back(MakePlot());
    countLabel_->setText(QStringLiteral("(%1)").arg(plots_.size()));
    emit graphCountChanged(GraphCount());
}

void TelemetryPanel::RemoveLastGraph() {
    if (plots_.size() <= 1) {
        return;
    }
    auto* plot = plots_.back();
    plots_.pop_back();
    plotsLayout_->removeWidget(plot);
    plot->deleteLater();
    countLabel_->setText(QStringLiteral("(%1)").arg(plots_.size()));
    emit graphCountChanged(GraphCount());
}

void TelemetryPanel::SetGraphCount(int count) {
    count = std::max(1, count);
    while (GraphCount() < count) {
        plots_.push_back(MakePlot());
    }
    while (GraphCount() > count) {
        auto* plot = plots_.back();
        plots_.pop_back();
        plotsLayout_->removeWidget(plot);
        plot->deleteLater();
    }
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
