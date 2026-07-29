#include "FlashPanel.h"

#include "HttpApiServer.h"
#include "RuntimeController.h"

#include <QCheckBox>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace NodeGUI::runtime {

FlashPanel::FlashPanel(FirmwareUpdater* updater,
                       RuntimeController* controller,
                       HttpApiServer* httpServer,
                       QWidget* parent)
    : QWidget(parent)
    , updater_(updater)
    , controller_(controller)
    , httpServer_(httpServer) {
    auto* layout = new QVBoxLayout(this);

    portLabel_ = new QLabel(QStringLiteral("Port: %1").arg(controller_->Port()), this);
    layout->addWidget(portLabel_);

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

    auto* line1 = new QFrame(this);
    line1->setFrameShape(QFrame::HLine);
    line1->setFrameShadow(QFrame::Sunken);
    layout->addWidget(line1);

    auto* httpRow = new QHBoxLayout;
    httpRow->addWidget(new QLabel(QStringLiteral("HTTP port"), this));
    httpPortEdit_ = new QLineEdit(QStringLiteral("18080"), this);
    httpPortEdit_->setMaximumWidth(90);
    httpRow->addWidget(httpPortEdit_);
    httpButton_ = new QPushButton(this);
    connect(httpButton_, &QPushButton::clicked, this, &FlashPanel::OnHttpToggle);
    httpRow->addWidget(httpButton_);
    httpStatus_ = new QLabel(this);
    httpRow->addWidget(httpStatus_);
    httpRow->addStretch(1);
    layout->addLayout(httpRow);

    auto* httpHint = new QLabel(
        QStringLiteral("POST the raw .bin body to /flash to queue an update."),
        this);
    layout->addWidget(httpHint);

    auto* line2 = new QFrame(this);
    line2->setFrameShape(QFrame::HLine);
    line2->setFrameShadow(QFrame::Sunken);
    layout->addWidget(line2);

    stateLabel_ = new QLabel(this);
    layout->addWidget(stateLabel_);
    errorLabel_ = new QLabel(this);
    errorLabel_->setStyleSheet(QStringLiteral("color: #ef5350;"));
    errorLabel_->setWordWrap(true);
    layout->addWidget(errorLabel_);

    logView_ = new QPlainTextEdit(this);
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(200);
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

    FlashJob job;
    job.firmware_path = path.toStdString();
    job.port = controller_->Port().toStdString();
    job.auto_gpio = autoGpioCheck_->isChecked();
    if (!updater_->queueFlash(job, false)) {
        errorLabel_->setText(QStringLiteral("Error: a flash job is already running"));
    }
    shownLogLines_ = 0;
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

void FlashPanel::OnHttpToggle() {
    if (httpServer_->isRunning()) {
        httpServer_->stop();
    } else {
        httpServer_->start(httpPortEdit_->text().trimmed().toStdString());
    }
    PollStatus();
}

void FlashPanel::PollStatus() {
    const FlashStatus status = updater_->status();

    const char* stateText = FirmwareUpdater::stateString(status.state);
    QString color = QStringLiteral("#e0e0e0");
    if (status.state == FlashState::Done) {
        color = QStringLiteral("#66bb6a");
    } else if (status.state == FlashState::Failed) {
        color = QStringLiteral("#ef5350");
    } else if (status.busy) {
        color = QStringLiteral("#ffb74d");
    }
    stateLabel_->setText(QStringLiteral("State: <span style=\"color:%1\">%2</span>")
                             .arg(color, QString::fromUtf8(stateText)));
    errorLabel_->setText(status.last_error.empty()
                             ? QString()
                             : QStringLiteral("Error: %1").arg(
                                   QString::fromStdString(status.last_error)));

    flashButton_->setEnabled(!status.busy);

    if (status.log.size() < shownLogLines_) {
        shownLogLines_ = 0;
        logView_->clear();
    }
    for (std::size_t i = shownLogLines_; i < status.log.size(); ++i) {
        logView_->appendPlainText(QString::fromStdString(status.log[i]));
    }
    shownLogLines_ = status.log.size();

    if (httpServer_->isRunning()) {
        httpButton_->setText(QStringLiteral("Stop Server"));
        httpStatus_->setText(
            QStringLiteral("<span style=\"color:#66bb6a\">Running on "
                           "http://localhost:%1/flash</span>")
                .arg(httpServer_->actualPort()));
    } else {
        httpButton_->setText(QStringLiteral("Start Server"));
        httpStatus_->setText(QString());
    }
}

}  // namespace NodeGUI::runtime
