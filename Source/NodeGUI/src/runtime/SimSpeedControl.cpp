#include "SimSpeedControl.h"

#include "RuntimeController.h"

#include <algorithm>

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QTimer>

namespace NodeGUI::runtime {

namespace {
constexpr int kSliderMin = 5;    // 0.05x
constexpr int kSliderMax = 200;  // 2.00x
constexpr int kSliderDefault = 100;
}  // namespace

SimSpeedControl::SimSpeedControl(RuntimeController* controller, QWidget* parent)
    : QWidget(parent)
    , controller_(controller) {
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    layout->addWidget(new QLabel(QStringLiteral("Speed"), this));

    slider_ = new QSlider(Qt::Horizontal, this);
    slider_->setRange(kSliderMin, kSliderMax);
    slider_->setValue(kSliderDefault);
    slider_->setMinimumWidth(120);
    layout->addWidget(slider_, 1);

    valueLabel_ = new QLabel(QStringLiteral("1.00x"), this);
    valueLabel_->setMinimumWidth(52);
    layout->addWidget(valueLabel_);

    turboButton_ = new QPushButton(QStringLiteral("Turbo"), this);
    layout->addWidget(turboButton_);

    debounceTimer_ = new QTimer(this);
    debounceTimer_->setSingleShot(true);
    debounceTimer_->setInterval(120);

    connect(slider_, &QSlider::valueChanged, this, &SimSpeedControl::OnSliderChanged);
    connect(slider_, &QSlider::sliderReleased, this, &SimSpeedControl::OnSliderReleased);
    connect(turboButton_, &QPushButton::clicked, this, &SimSpeedControl::OnTurboClicked);
    connect(debounceTimer_, &QTimer::timeout, this, &SimSpeedControl::FlushDebouncedSpeed);
    connect(controller_, &RuntimeController::simSpeedChanged, this,
            &SimSpeedControl::OnHostSpeedChanged);

    UpdateUi(1.0, false);
}

void SimSpeedControl::SetSpeedFactor(double factor, bool turbo) {
    UpdateUi(factor, turbo);
}

void SimSpeedControl::OnSliderChanged(int value) {
    if (updatingUi_) {
        return;
    }
    const double factor = static_cast<double>(value) / 100.0;
    valueLabel_->setText(QStringLiteral("%1x").arg(factor, 0, 'f', 2));
    pendingFactor_ = factor;
    pendingTurbo_ = false;
    debounceTimer_->start();
}

void SimSpeedControl::OnSliderReleased() {
    debounceTimer_->stop();
    SendSpeed(pendingFactor_, false);
}

void SimSpeedControl::FlushDebouncedSpeed() {
    if (!pendingTurbo_) {
        SendSpeed(pendingFactor_, false);
    }
}

void SimSpeedControl::OnTurboClicked() {
    debounceTimer_->stop();
    SendSpeed(0.0, true);
}

void SimSpeedControl::OnHostSpeedChanged(double factor) {
    UpdateUi(factor, factor <= 0.0);
}

void SimSpeedControl::SendSpeed(double factor, bool turbo) {
    controller_->SetSimSpeed(factor, turbo);
}

void SimSpeedControl::UpdateUi(double factor, bool turbo) {
    updatingUi_ = true;
    if (turbo || factor <= 0.0) {
        valueLabel_->setText(QStringLiteral("Turbo"));
        turboButton_->setStyleSheet(QStringLiteral("font-weight: bold;"));
    } else {
        const int sliderValue =
            static_cast<int>(std::clamp(factor * 100.0, static_cast<double>(kSliderMin),
                                        static_cast<double>(kSliderMax)));
        slider_->setValue(sliderValue);
        valueLabel_->setText(QStringLiteral("%1x").arg(factor, 0, 'f', 2));
        turboButton_->setStyleSheet(QString());
    }
    updatingUi_ = false;
}

}  // namespace NodeGUI::runtime
