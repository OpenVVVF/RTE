#pragma once

#include "FirmwareUpdater.h"

#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTimer;

namespace NodeGUI::runtime {

class HttpApiServer;
class RuntimeController;

// Firmware Update tab: firmware path + Flash button + Auto-GPIO toggle, HTTP
// server controls, and the updater state/log. Mirrors the old ImGui
// "Firmware Update" tab. Polls the Qt-free FirmwareUpdater on a timer.
class FlashPanel : public QWidget {
    Q_OBJECT

public:
    FlashPanel(FirmwareUpdater* updater,
               RuntimeController* controller,
               HttpApiServer* httpServer,
               QWidget* parent = nullptr);

private slots:
    void OnFlashClicked();
    void OnBrowse();
    void OnHttpToggle();
    void PollStatus();

private:
    FirmwareUpdater* updater_;
    RuntimeController* controller_;
    HttpApiServer* httpServer_;

    QLabel* portLabel_ = nullptr;
    QLineEdit* pathEdit_ = nullptr;
    QPushButton* flashButton_ = nullptr;
    QCheckBox* autoGpioCheck_ = nullptr;
    QLabel* manualHint_ = nullptr;
    QLineEdit* httpPortEdit_ = nullptr;
    QPushButton* httpButton_ = nullptr;
    QLabel* httpStatus_ = nullptr;
    QLabel* stateLabel_ = nullptr;
    QLabel* errorLabel_ = nullptr;
    QPlainTextEdit* logView_ = nullptr;
    QTimer* pollTimer_ = nullptr;

    std::size_t shownLogLines_ = 0;
};

}  // namespace NodeGUI::runtime
