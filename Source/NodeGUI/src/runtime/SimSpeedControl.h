#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class QSlider;
class QTimer;

namespace NodeGUI::runtime {

class RuntimeController;

// Slider + label for live HostSim speed (0.05x .. 2.0x) plus Turbo preset.
class SimSpeedControl : public QWidget {
    Q_OBJECT

public:
    explicit SimSpeedControl(RuntimeController* controller, QWidget* parent = nullptr);

    void SetSpeedFactor(double factor, bool turbo);

private slots:
    void OnSliderChanged(int value);
    void OnSliderReleased();
    void OnTurboClicked();
    void OnHostSpeedChanged(double factor);
    void FlushDebouncedSpeed();

private:
    void SendSpeed(double factor, bool turbo);
    void UpdateUi(double factor, bool turbo);

    RuntimeController* controller_ = nullptr;
    QSlider* slider_ = nullptr;
    QLabel* valueLabel_ = nullptr;
    QPushButton* turboButton_ = nullptr;
    QTimer* debounceTimer_ = nullptr;
    bool updatingUi_ = false;
    double pendingFactor_ = 1.0;
    bool pendingTurbo_ = false;
};

}  // namespace NodeGUI::runtime
