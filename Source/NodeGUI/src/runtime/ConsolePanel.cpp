#include "ConsolePanel.h"

#include "RuntimeController.h"
#include "SimSpeedControl.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace NodeGUI::runtime {

namespace {

class HistoryLineEdit : public QLineEdit {
public:
    std::function<void(int)> onHistoryStep;  // -1 = older, +1 = newer

protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Up && onHistoryStep) {
            onHistoryStep(-1);
            return;
        }
        if (event->key() == Qt::Key_Down && onHistoryStep) {
            onHistoryStep(+1);
            return;
        }
        QLineEdit::keyPressEvent(event);
    }
};

}  // namespace

ConsolePanel::ConsolePanel(RuntimeController* controller, QWidget* parent)
    : QWidget(parent)
    , controller_(controller) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* buttonsRow = new QHBoxLayout;
    auto* clearButton = new QPushButton(QStringLiteral("Clear"), this);
    connect(clearButton, &QPushButton::clicked, this, [this] {
        controller_->Store().ClearConsole();
        consoleView_->clear();
        lastConsoleSeq_ = 0;
    });
    buttonsRow->addWidget(clearButton);
    pauseButton_ = new QPushButton(QStringLiteral("Pause Sim"), this);
    connect(pauseButton_, &QPushButton::clicked, this, &ConsolePanel::OnTogglePause);
    buttonsRow->addWidget(pauseButton_);
    autoscrollCheck_ = new QCheckBox(QStringLiteral("Autoscroll"), this);
    autoscrollCheck_->setChecked(true);
    buttonsRow->addWidget(autoscrollCheck_);
    buttonsRow->addStretch(1);
    buttonsRow->addWidget(new QLabel(QStringLiteral("Commands"), this));
    layout->addLayout(buttonsRow);

    auto* liveRow = new QHBoxLayout;
    liveRow->addWidget(new QLabel(QStringLiteral("Throttle A"), this));
    throttleASpin_ = new QDoubleSpinBox(this);
    throttleASpin_->setRange(0.0, 1.0);
    throttleASpin_->setSingleStep(0.05);
    throttleASpin_->setDecimals(3);
    liveRow->addWidget(throttleASpin_);
    liveRow->addWidget(new QLabel(QStringLiteral("Throttle B"), this));
    throttleBSpin_ = new QDoubleSpinBox(this);
    throttleBSpin_->setRange(0.0, 1.0);
    throttleBSpin_->setSingleStep(0.05);
    throttleBSpin_->setDecimals(3);
    liveRow->addWidget(throttleBSpin_);
    auto* throttleButton = new QPushButton(QStringLiteral("Apply Throttle"), this);
    connect(throttleButton, &QPushButton::clicked, this, &ConsolePanel::OnSendThrottle);
    liveRow->addWidget(throttleButton);
    liveRow->addSpacing(12);
    liveRow->addWidget(new QLabel(QStringLiteral("Duty U/V/W"), this));
    dutyUSpin_ = new QDoubleSpinBox(this);
    dutyUSpin_->setRange(0.0, 100.0);
    dutyUSpin_->setSingleStep(1.0);
    dutyUSpin_->setDecimals(2);
    liveRow->addWidget(dutyUSpin_);
    dutyVSpin_ = new QDoubleSpinBox(this);
    dutyVSpin_->setRange(0.0, 100.0);
    dutyVSpin_->setSingleStep(1.0);
    dutyVSpin_->setDecimals(2);
    liveRow->addWidget(dutyVSpin_);
    dutyWSpin_ = new QDoubleSpinBox(this);
    dutyWSpin_->setRange(0.0, 100.0);
    dutyWSpin_->setSingleStep(1.0);
    dutyWSpin_->setDecimals(2);
    liveRow->addWidget(dutyWSpin_);
    auto* dutyButton = new QPushButton(QStringLiteral("Apply Duty"), this);
    connect(dutyButton, &QPushButton::clicked, this, &ConsolePanel::OnSendDuty);
    liveRow->addWidget(dutyButton);
    auto* clearOverridesButton = new QPushButton(QStringLiteral("Clear Overrides"), this);
    connect(clearOverridesButton, &QPushButton::clicked, this, &ConsolePanel::OnClearOverrides);
    liveRow->addWidget(clearOverridesButton);
    layout->addLayout(liveRow);

    auto* plantRow = new QHBoxLayout;
    plantRow->addWidget(new QLabel(QStringLiteral("Plant"), this));
    backendCombo_ = new QComboBox(this);
    backendCombo_->addItem(QStringLiteral("ode (fast)"), QStringLiteral("ode"));
    backendCombo_->addItem(QStringLiteral("ngspice (accurate)"), QStringLiteral("ngspice"));
    backendCombo_->setToolTip(QStringLiteral(
        "Plant model backend: ode = fast real-time view, ngspice = accurate SPICE view"));
    plantRow->addWidget(backendCombo_);
    auto* backendButton = new QPushButton(QStringLiteral("Apply Backend"), this);
    connect(backendButton, &QPushButton::clicked, this, &ConsolePanel::OnApplyBackend);
    plantRow->addWidget(backendButton);
    plantRow->addSpacing(12);
    plantRow->addWidget(new QLabel(QStringLiteral("Render s"), this));
    renderDurSpin_ = new QDoubleSpinBox(this);
    renderDurSpin_->setRange(0.0005, 10.0);
    renderDurSpin_->setSingleStep(0.005);
    renderDurSpin_->setDecimals(4);
    renderDurSpin_->setValue(0.005);
    renderDurSpin_->setToolTip(QStringLiteral(
        "How many seconds of simulation the SPICE burst renders"));
    plantRow->addWidget(renderDurSpin_);
    plantRow->addWidget(new QLabel(QStringLiteral("ISR Hz"), this));
    renderIsrSpin_ = new QDoubleSpinBox(this);
    renderIsrSpin_->setRange(1000.0, 500000.0);
    renderIsrSpin_->setSingleStep(10000.0);
    renderIsrSpin_->setDecimals(0);
    renderIsrSpin_->setValue(50000.0);
    renderIsrSpin_->setToolTip(QStringLiteral(
        "TIM ISR rate for the burst. Rule: ISR >= 20 x electrical freq x highest "
        "harmonic of interest (e.g. 20th harmonic at 1 kHz needs >= 400 kHz; "
        "substeps multiply the effective SPICE rate)"));
    plantRow->addWidget(renderIsrSpin_);
    plantRow->addWidget(new QLabel(QStringLiteral("Substeps"), this));
    renderSubstepsSpin_ = new QSpinBox(this);
    renderSubstepsSpin_->setRange(1, 16);
    renderSubstepsSpin_->setValue(4);
    renderSubstepsSpin_->setToolTip(QStringLiteral(
        "SPICE steps per ISR tick (multiplies effective sample rate)"));
    plantRow->addWidget(renderSubstepsSpin_);
    auto* renderButton = new QPushButton(QStringLiteral("Render SPICE"), this);
    renderButton->setToolTip(QStringLiteral(
        "Run an ngspice burst for the given duration at the given ISR rate, then "
        "restore the previous live settings. Blocks HostSim commands during the burst."));
    connect(renderButton, &QPushButton::clicked, this, &ConsolePanel::OnRenderSpice);
    plantRow->addWidget(renderButton);
    plantRow->addStretch(1);
    layout->addLayout(plantRow);

    layout->addWidget(new SimSpeedControl(controller_, this));

    consoleView_ = new QPlainTextEdit(this);
    consoleView_->setReadOnly(true);
    consoleView_->setMaximumBlockCount(static_cast<int>(TelemetryStore::kConsoleCapLines));
    layout->addWidget(consoleView_, 1);

    auto* sendRow = new QHBoxLayout;
    sendRow->addWidget(new QLabel(QStringLiteral("Send:"), this));
    auto* commandEdit = new HistoryLineEdit;
    commandEdit_ = commandEdit;
    commandEdit->onHistoryStep = [this](int dir) {
        if (commandHistory_.isEmpty()) {
            return;
        }
        if (historyIndex_ < 0) {
            historyIndex_ = static_cast<int>(commandHistory_.size());
        }
        historyIndex_ = std::clamp(historyIndex_ + dir, 0,
                                   static_cast<int>(commandHistory_.size()) - 1);
        commandEdit_->setText(commandHistory_[historyIndex_]);
    };
    connect(commandEdit, &QLineEdit::returnPressed, this, &ConsolePanel::OnSendCommand);
    sendRow->addWidget(commandEdit, 1);
    auto* sendButton = new QPushButton(QStringLiteral("Send"), this);
    connect(sendButton, &QPushButton::clicked, this, &ConsolePanel::OnSendCommand);
    sendRow->addWidget(sendButton);
    layout->addLayout(sendRow);

    connect(controller_, &RuntimeController::storeChanged,
            this, &ConsolePanel::OnStoreChanged);
    connect(controller_, &RuntimeController::sessionCleared,
            this, &ConsolePanel::OnSessionCleared);
    connect(controller_, &RuntimeController::simPauseChanged,
            this, &ConsolePanel::OnSimPauseChanged);
    OnSimPauseChanged(controller_->IsSimPaused());
}

void ConsolePanel::OnStoreChanged() {
    // Detect a ClearConsole() from another console instance: the store's
    // newest seq moved backwards.
    if (controller_->Store().LatestConsoleSeq() < lastConsoleSeq_) {
        consoleView_->clear();
        lastConsoleSeq_ = 0;
    }

    const auto lines = controller_->Store().ConsoleSince(lastConsoleSeq_);
    if (!lines.empty()) {
        QString batch;
        batch.reserve(lines.size() * 64);
        for (std::size_t i = 0; i < lines.size(); ++i) {
            batch += QString::fromStdString(lines[i].text);
            if (i + 1 < lines.size()) {
                batch += u'\n';
            }
            lastConsoleSeq_ = lines[i].seq;
        }
        consoleView_->appendPlainText(batch);
    }
    if (autoscrollCheck_->isChecked() && !lines.empty()) {
        auto bar = consoleView_->verticalScrollBar();
        bar->setValue(bar->maximum());
    }
}

void ConsolePanel::OnSendCommand() {
    const QString line = commandEdit_->text().trimmed();
    if (line.isEmpty()) {
        return;
    }
    commandEdit_->clear();
    if (commandHistory_.isEmpty() || commandHistory_.last() != line) {
        commandHistory_.push_back(line);
    }
    historyIndex_ = -1;
    controller_->SendCommand(line);
    OnStoreChanged();
}

void ConsolePanel::OnSessionCleared() {
    consoleView_->clear();
    commandEdit_->clear();
    commandHistory_.clear();
    historyIndex_ = -1;
    lastConsoleSeq_ = 0;
void ConsolePanel::OnSendThrottle() {
    controller_->SendCommand(QStringLiteral("throttle a %1")
                                 .arg(throttleASpin_->value(), 0, 'f', 3));
    controller_->SendCommand(QStringLiteral("throttle b %1")
                                 .arg(throttleBSpin_->value(), 0, 'f', 3));
    OnStoreChanged();
}

void ConsolePanel::OnSendDuty() {
    controller_->SendCommand(QStringLiteral("duty u %1")
                                 .arg(dutyUSpin_->value(), 0, 'f', 2));
    controller_->SendCommand(QStringLiteral("duty v %1")
                                 .arg(dutyVSpin_->value(), 0, 'f', 2));
    controller_->SendCommand(QStringLiteral("duty w %1")
                                 .arg(dutyWSpin_->value(), 0, 'f', 2));
    OnStoreChanged();
}

void ConsolePanel::OnClearOverrides() {
    controller_->SendCommand(QStringLiteral("clear"));
    OnStoreChanged();
}

void ConsolePanel::OnTogglePause() {
    controller_->ToggleSimPause();
    OnStoreChanged();
}

void ConsolePanel::OnApplyBackend() {
    controller_->SendCommand(QStringLiteral("plant backend %1")
                                 .arg(backendCombo_->currentData().toString()));
    OnStoreChanged();
}

void ConsolePanel::OnRenderSpice() {
    controller_->SendCommand(QStringLiteral("render %1 %2 %3")
                                 .arg(renderDurSpin_->value(), 0, 'f', 4)
                                 .arg(renderIsrSpin_->value(), 0, 'f', 0)
                                 .arg(renderSubstepsSpin_->value()));
    OnStoreChanged();
}

void ConsolePanel::OnSimPauseChanged(bool paused) {
    simPaused_ = paused;
    if (pauseButton_) {
        pauseButton_->setText(paused ? QStringLiteral("Resume Sim")
                                     : QStringLiteral("Pause Sim"));
    }
}

}  // namespace NodeGUI::runtime
