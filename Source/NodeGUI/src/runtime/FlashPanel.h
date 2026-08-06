#pragma once

#include <QByteArray>
#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProcess;
class QProgressBar;
class QPushButton;

namespace NodeGUI::runtime {

class RuntimeController;

// Firmware Update tab backed by the same finite `rte flash` worker used by
// graph actions and automation clients.
class FlashPanel final : public QWidget {
    Q_OBJECT

public:
    explicit FlashPanel(RuntimeController* controller, QWidget* parent = nullptr);

private:
    void OnFlashClicked();
    void OnBrowse();
    void ReadOutput();
    void Finish(int exitCode);
    void AppendJsonLine(const QByteArray& line);

    RuntimeController* controller_ = nullptr;
    QProcess* process_ = nullptr;
    QByteArray outputBuffer_;

    QLineEdit* pathEdit_ = nullptr;
    QPushButton* flashButton_ = nullptr;
    QCheckBox* autoGpioCheck_ = nullptr;
    QLabel* manualHint_ = nullptr;
    QLabel* stateLabel_ = nullptr;
    QProgressBar* progressBar_ = nullptr;
    QLabel* errorLabel_ = nullptr;
    QPlainTextEdit* logView_ = nullptr;
};

}  // namespace NodeGUI::runtime
