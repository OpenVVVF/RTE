#include "TelemetryPanel.h"

#include "RuntimeController.h"
#include "SignalPlotWidget.h"

#include <QSplitter>
#include <QVBoxLayout>

namespace NodeGUI::runtime {

TelemetryPanel::TelemetryPanel(RuntimeController* controller, QWidget* parent)
    : QWidget(parent)
    , controller_(controller) {
    auto* rightSplit = new QSplitter(Qt::Vertical, this);
    for (int i = 0; i < 3; ++i) {
        plots_[i] = new SignalPlotWidget(QStringLiteral("Graph %1").arg(i + 1), rightSplit);
        plots_[i]->SetStore(&controller_->Store());
        rightSplit->addWidget(plots_[i]);
    }
    rightSplit->setSizes({220, 220, 220});

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(rightSplit);

    connect(controller_, &RuntimeController::storeChanged,
            this, &TelemetryPanel::OnStoreChanged);
}

void TelemetryPanel::SetGraphSignals(const std::array<QStringList, 3>& sets) {
    for (int i = 0; i < 3; ++i) {
        plots_[i]->SetSignals(sets[i]);
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
