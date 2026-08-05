#include "FlashPanel.h"

#include "RemoteFlashBackend.h"
#include "RuntimeController.h"

#include <QCheckBox>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace NodeGUI::runtime {

FlashPanel::FlashPanel(FlashBackend* backend,
                       RuntimeController* controller,
                       QWidget* parent)
    : QWidget(parent)
    , backend_(backend)
    , controller_(controller) {
    auto* layout = new QVBoxLayout(this);

    serverLabel_ = new QLabel(this);
    layout->addWidget(serverLabel_);

    auto* pathRow = new QHBoxLayout;
    pathRow->addWidget(new QLabel(QStringLiteral("Firmware path"), this));
    pathEdit_ = new QLineEdit(this);
    pathRow->addWidget(pathEdit_, 1);
    auto* browseButton = new QPushButton(QStringLiteral("Browse..."), this);
    connect(browseButton, &QPushButton::clicked, this, &FlashPanel::OnBrowse);
    pathRow->addWidget(browseButton);
    layout->addLayout(pathRow);

    auto* flashRow = new QHBoxLayout;
    flashButton_ = new QPushButton(QStringLiteral("Flash"), this);
    connect(flashButton_, &QPushButton::clicked, this, &FlashPanel::OnFlashClicked);
    flashRow->addWidget(flashButton_);
    autoGpioCheck_ = new QCheckBox(QStringLiteral("Auto GPIO (MCP2221A)"), this);
    autoGpioCheck_->setChecked(true);
    connect(autoGpioCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        manualHint_->setVisible(!checked);
    });
    flashRow->addWidget(autoGpioCheck_);
    flashRow->addStretch(1);
    layout->addLayout(flashRow);

    manualHint_ = new QLabel(
        QStringLiteral("Manual mode: hold BOOT0 high and pulse NRST to enter the "
                       "bootloader when prompted (5 s wait)."),
        this);
    manualHint_->setWordWrap(true);
    manualHint_->setVisible(false);
    layout->addWidget(manualHint_);

    auto* line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    layout->addWidget(line);

    stateLabel_ = new QLabel(this);
    layout->addWidget(stateLabel_);

    progressBar_ = new QProgressBar(this);
    progressBar_->setRange(0, 100);
    progressBar_->setTextVisible(true);
    progressBar_->hide();  // shown while a flash job runs
    layout->addWidget(progressBar_);

    errorLabel_ = new QLabel(this);
    errorLabel_->setStyleSheet(QStringLiteral("color: #ef5350;"));
    errorLabel_->setWordWrap(true);
    layout->addWidget(errorLabel_);

    logView_ = new QPlainTextEdit(this);
    logView_->setReadOnly(true);
    layout->addWidget(logView_, 1);

    pollTimer_ = new QTimer(this);
    pollTimer_->setInterval(100);
    connect(pollTimer_, &QTimer::timeout, this, &FlashPanel::PollStatus);
    pollTimer_->start();
    PollStatus();
}

void FlashPanel::OnFlashClicked() {
    const QString path = pathEdit_->text().trimmed();
    if (path.isEmpty()) {
        errorLabel_->setText(QStringLiteral("Error: no firmware path given"));
        return;
    }

    if (!backend_->QueueFlash(path.toStdString(), autoGpioCheck_->isChecked())) {
        // The concrete reason surfaces through Status() on the next poll.
    }
    shownLog_.clear();
    logView_->clear();
}

void FlashPanel::OnBrowse() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Select firmware binary"), QString(),
        QStringLiteral("Firmware binaries (*.bin);;All files (*)"));
    if (!path.isEmpty()) {
        pathEdit_->setText(path);
    }
}

void FlashPanel::PollStatus() {
    const FlashBackendStatus status = backend_->Status();

    if (auto* remote = dynamic_cast<RemoteFlashBackend*>(backend_)) {
        serverLabel_->setText(QStringLiteral("Gateway: %1").arg(remote->BaseUrl()));
    }

    QString color = QStringLiteral("#e0e0e0");
    QString stateText = QString::fromStdString(status.state);
    if (!status.reachable) {
        color = QStringLiteral("#ef5350");
        stateText = QStringLiteral("Unreachable");
    } else if (status.state == "Done") {
        color = QStringLiteral("#66bb6a");
    } else if (status.state == "Failed") {
        color = QStringLiteral("#ef5350");
    } else if (status.busy) {
        color = QStringLiteral("#ffb74d");
    }
    stateLabel_->setText(QStringLiteral("State: <span style=\"color:%1\">%2</span>")
                             .arg(color, stateText));
    errorLabel_->setText(status.lastError.empty()
                             ? QString()
                             : QStringLiteral("Error: %1").arg(
                                   QString::fromStdString(status.lastError)));

    flashButton_->setEnabled(!status.busy && controller_->HasControl());

    // Progress bar: determinate once the server reports percentages (flash /
    // verify phases), busy-bounce during drains and GPIO waits, hidden when
    // idle.
    if (status.busy) {
        progressBar_->show();
        if (status.progress >= 0) {
            progressBar_->setRange(0, 100);
            progressBar_->setValue(status.progress);
        } else {
            progressBar_->setRange(0, 0);  // indeterminate
        }
    } else {
        if (status.state == "Done") {
            progressBar_->setRange(0, 100);
            progressBar_->setValue(100);
        } else {
            progressBar_->hide();
            progressBar_->setValue(0);
        }
    }

    std::size_t commonLines = 0;
    while (commonLines < shownLog_.size()
           && commonLines < status.log.size()
           && shownLog_[commonLines] == status.log[commonLines]) {
        ++commonLines;
    }
    if (commonLines < shownLog_.size()) {
        logView_->clear();
        commonLines = 0;
    }
    for (std::size_t i = commonLines; i < status.log.size(); ++i) {
        logView_->appendPlainText(QString::fromStdString(status.log[i]));
    }
    shownLog_ = status.log;
}

}  // namespace NodeGUI::runtime
