#pragma once

#include <QWidget>

#include <QStringList>

class QCheckBox;
class QDoubleSpinBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace NodeGUI::runtime {

class RuntimeController;

class ConsolePanel : public QWidget {
    Q_OBJECT

public:
    explicit ConsolePanel(RuntimeController* controller, QWidget* parent = nullptr);

private slots:
    void OnStoreChanged();
    void OnSendCommand();
    void OnSendThrottle();
    void OnSendDuty();
    void OnClearOverrides();
    void OnTogglePause();
    void OnSimPauseChanged(bool paused);
    void OnSessionCleared();

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
    bool simPaused_ = false;

    uint64_t lastConsoleSeq_ = 0;

    QStringList commandHistory_;
    int historyIndex_ = -1;
};

}  // namespace NodeGUI::runtime
