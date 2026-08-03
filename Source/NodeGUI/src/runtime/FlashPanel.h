#pragma once

#include "FlashBackend.h"

#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTimer;

namespace NodeGUI::runtime {

class RuntimeController;

// Firmware Update tab: firmware path + Flash button + Auto-GPIO toggle, the
// updater state/log and flash progress. All flashing goes through the
// RTEServer's HTTP API via FlashBackend (spawned-local or remote server).
class FlashPanel : public QWidget {
    Q_OBJECT

public:
    FlashPanel(FlashBackend* backend,
               RuntimeController* controller,
               QWidget* parent = nullptr);

private slots:
    void OnFlashClicked();
    void OnBrowse();
    void PollStatus();

private:
    FlashBackend* backend_;
    RuntimeController* controller_;

    QLabel* serverLabel_ = nullptr;
    QLineEdit* pathEdit_ = nullptr;
    QPushButton* flashButton_ = nullptr;
    QCheckBox* autoGpioCheck_ = nullptr;
    QLabel* manualHint_ = nullptr;
    QLabel* stateLabel_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QLabel* errorLabel_ = nullptr;
    QPlainTextEdit* logView_ = nullptr;
    QTimer* pollTimer_ = nullptr;

    std::size_t shownLogLines_ = 0;
};

}  // namespace NodeGUI::runtime
