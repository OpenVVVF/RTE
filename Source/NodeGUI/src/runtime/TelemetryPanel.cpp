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
}

SignalPlotWidget* TelemetryPanel::MakePlot() {
    auto* plot = new SignalPlotWidget(
        QStringLiteral("Graph %1").arg(plots_.size() + 1), plotsContainer_);
    plot->SetStore(&controller_->Store());
    plot->setMinimumHeight(200);
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
}

void TelemetryPanel::OnStoreChanged() {
    for (auto* plot : plots_) {
        plot->Refresh();
    }
}

}  // namespace NodeGUI::runtime
