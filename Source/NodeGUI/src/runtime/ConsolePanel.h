#pragma once

#include <QWidget>

#include <QDateTime>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTimer;

namespace NodeGUI::runtime {

class RuntimeController;

class ConsolePanel : public QWidget {
    Q_OBJECT

public:
    explicit ConsolePanel(RuntimeController* controller, QWidget* parent = nullptr);

private slots:
    void OnStoreChanged();
    void OnSendCommand();
    void OnSessionCleared();
    void OnSendThrottle();
    void OnSendDuty();
    void OnClearOverrides();
    void OnTogglePause();
    void OnSimPauseChanged(bool paused);
    void OnApplyBackend();
    void OnRenderSpice();
    void OnShowRender();
    void OnRenderWatchTick();

private:
    RuntimeController* controller_;

    QPlainTextEdit* consoleView_ = nullptr;
    QLineEdit* commandEdit_ = nullptr;
    QCheckBox* autoscrollCheck_ = nullptr;
    QPushButton* pauseButton_ = nullptr;
    QDoubleSpinBox* throttleASpin_ = nullptr;
    QDoubleSpinBox* throttleBSpin_ = nullptr;
    QDoubleSpinBox* dutyUSpin_ = nullptr;
    QDoubleSpinBox* dutyVSpin_ = nullptr;
    QDoubleSpinBox* dutyWSpin_ = nullptr;
    QComboBox* backendCombo_ = nullptr;
    QDoubleSpinBox* renderDurSpin_ = nullptr;
    QDoubleSpinBox* renderIsrSpin_ = nullptr;
    QSpinBox* renderSubstepsSpin_ = nullptr;
    QLineEdit* renderPathEdit_ = nullptr;
    QCheckBox* renderAutoShowCheck_ = nullptr;
    QPushButton* renderButton_ = nullptr;
    QLabel* renderStatusLabel_ = nullptr;
    QTimer* renderWatchTimer_ = nullptr;
    // Auto-show bookkeeping: fires when the render CSV has been rewritten
    // after the command was sent and its size has been stable for two ticks.
    QDateTime renderSentAt_;
    qint64 renderWatchLastSize_ = -1;
    int renderWatchStableTicks_ = 0;
    // Incremental CSV row counting for the "Rendering… N%" status.
    qint64 renderExpectedRows_ = 0;
    qint64 renderWatchRowOffset_ = 0;
    qint64 renderWatchRows_ = 0;
    bool renderWatchHeaderSeen_ = false;
    bool simPaused_ = false;

    uint64_t lastConsoleSeq_ = 0;

    QStringList commandHistory_;
    int historyIndex_ = -1;
};

}  // namespace NodeGUI::runtime
