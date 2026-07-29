#include "TelemetryPanel.h"

#include "RuntimeController.h"
#include "SignalPlotWidget.h"

#include <QSplitter>
#include <QTimer>
#include <QVBoxLayout>

namespace NodeGUI::runtime {

TelemetryPanel::TelemetryPanel(RuntimeController* controller, QWidget* parent)
    : QWidget(parent)
    , controller_(controller) {
    const QColor accents[] = {
        QColor(102, 204, 255),
        QColor(255, 176, 77),
        QColor(140, 235, 120),
    };

    auto* rightSplit = new QSplitter(Qt::Vertical, this);
    for (int i = 0; i < 3; ++i) {
        plots_[i] = new SignalPlotWidget(QStringLiteral("Graph %1").arg(i + 1), rightSplit);
        plots_[i]->SetStore(&controller_->Store());
        plots_[i]->SetAccentColor(accents[i]);
        connect(plots_[i], &SignalPlotWidget::viewSecondsRequested,
                this, &TelemetryPanel::viewSecondsRequested);
        rightSplit->addWidget(plots_[i]);
    }
    rightSplit->setSizes({220, 220, 220});

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(rightSplit);

    connect(controller_, &RuntimeController::plotTimeFreezeChanged,
            this, &TelemetryPanel::OnPlotTimeFreeze);

    plotRefreshTimer_ = new QTimer(this);
    plotRefreshTimer_->setInterval(40);
    connect(plotRefreshTimer_, &QTimer::timeout, this, &TelemetryPanel::RefreshPlots);
    plotRefreshTimer_->start();
}

void TelemetryPanel::SetGraphSignals(const std::array<QStringList, 3>& sets) {
    const bool anyAssigned = !sets[0].isEmpty() || !sets[1].isEmpty() || !sets[2].isEmpty();
    for (int i = 0; i < 3; ++i) {
        plots_[i]->SetSignals(sets[i]);
        // An unassigned plot is dead space; hand its height to the assigned
        // ones. Graph 1 stays up when nothing is assigned so there is still a
        // target for the first signal the user checks.
        plots_[i]->setVisible(!sets[i].isEmpty() || (i == 0 && !anyAssigned));
    }
}

void TelemetryPanel::SetViewSeconds(double seconds) {
    for (auto* plot : plots_) {
        plot->SetViewSeconds(seconds);
    }
}

void TelemetryPanel::SetGraphViewSeconds(const std::array<double, 3>& seconds) {
    for (int i = 0; i < 3; ++i) {
        plots_[i]->SetViewSeconds(seconds[i]);
    }
}

void TelemetryPanel::RefreshPlots() {
    for (auto* plot : plots_) {
        if (plot->isVisible()) {
            plot->Refresh();
        }
    }
}

void TelemetryPanel::OnPlotTimeFreeze(bool frozen, double anchorSimSec) {
    for (auto* plot : plots_) {
        plot->SetTimeFrozen(frozen, anchorSimSec);
    }
}

}  // namespace NodeGUI::runtime
