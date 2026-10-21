#include "TelemetryPanel.h"

#include "RuntimeController.h"
#include "SignalPlotWidget.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace NodeGUI::runtime {

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
    layout->addWidget(scroll_, 1);

    connect(controller_, &RuntimeController::storeChanged,
            this, &TelemetryPanel::OnStoreChanged);
    UpdateCursorInfo();
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
