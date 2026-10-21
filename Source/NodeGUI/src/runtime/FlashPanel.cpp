#include "FlashPanel.h"

#include "RuntimeController.h"

#include <RTEAutomation/Platform.h>

#include <QCheckBox>
#include <QCoreApplication>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include <filesystem>

namespace NodeGUI::runtime {
namespace {

QString RteCliPath() {
    const std::filesystem::path adjacent =
        std::filesystem::path(QCoreApplication::applicationDirPath().toStdString())
        / RTEAutomation::ExecutableName("rte");
    if (std::filesystem::is_regular_file(adjacent)) {
        return QString::fromStdString(adjacent.string());
    }
#ifdef RTE_CLI_DEVELOPMENT_PATH
    if (std::filesystem::is_regular_file(RTE_CLI_DEVELOPMENT_PATH)) {
        return QString::fromUtf8(RTE_CLI_DEVELOPMENT_PATH);
    }
#endif
    return QStringLiteral("rte");
}

}  // namespace

FlashPanel::FlashPanel(RuntimeController* controller, QWidget* parent)
    : QWidget(parent), controller_(controller), process_(new QProcess(this)) {
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(QStringLiteral("Port: %1").arg(controller_->Port()), this));

    auto* pathRow = new QHBoxLayout;
    pathRow->addWidget(new QLabel(QStringLiteral("Firmware path"), this));
    pathEdit_ = new QLineEdit(this);
    pathRow->addWidget(pathEdit_, 1);
    // Never start empty (clunk quick-win): prefill with the last flashed
    // binary when we know one.
    const QSettings settings(QStringLiteral("RTE"), QStringLiteral("RTEStudio"));
    const QString last = settings.value(QStringLiteral("flash/lastFirmware")).toString();
    if (!last.isEmpty()) {
        pathEdit_->setText(last);
    }
    auto* browse = new QPushButton(QStringLiteral("Browse..."), this);
    connect(browse, &QPushButton::clicked, this, &FlashPanel::OnBrowse);
    pathRow->addWidget(browse);
    layout->addLayout(pathRow);

    auto* flashRow = new QHBoxLayout;
    flashButton_ = new QPushButton(QStringLiteral("Flash"), this);
    connect(flashButton_, &QPushButton::clicked, this, &FlashPanel::OnFlashClicked);
    flashRow->addWidget(flashButton_);
    cancelButton_ = new QPushButton(QStringLiteral("Cancel"), this);
    cancelButton_->setObjectName(QStringLiteral("flashCancel"));
    cancelButton_->setEnabled(false);
    cancelButton_->setToolTip(QStringLiteral("Abort the running flash."));
    connect(cancelButton_, &QPushButton::clicked, this, &FlashPanel::OnCancel);
    flashRow->addWidget(cancelButton_);
    autoGpioCheck_ = new QCheckBox(QStringLiteral("Auto GPIO (MCP2221A)"), this);
    autoGpioCheck_->setChecked(true);
    flashRow->addWidget(autoGpioCheck_);
    flashRow->addStretch(1);
    layout->addLayout(flashRow);

    manualHint_ = new QLabel(
        QStringLiteral("Manual mode: hold BOOT0 high and pulse NRST when prompted."), this);
    manualHint_->setWordWrap(true);
    manualHint_->hide();
    connect(autoGpioCheck_, &QCheckBox::toggled, manualHint_, &QWidget::setHidden);
    layout->addWidget(manualHint_);

    stateLabel_ = new QLabel(QStringLiteral("State: Idle"), this);
    layout->addWidget(stateLabel_);
    progressBar_ = new QProgressBar(this);
    progressBar_->setRange(0, 100);
    progressBar_->hide();
    layout->addWidget(progressBar_);
    errorLabel_ = new QLabel(this);
    errorLabel_->setStyleSheet(QStringLiteral("color: #ef5350;"));
    errorLabel_->setWordWrap(true);
    layout->addWidget(errorLabel_);
    logView_ = new QPlainTextEdit(this);
    // Tab leaves the field; the all-text-edit Tab trap ends keyboard nav.
    logView_->setTabChangesFocus(true);
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(500);
    layout->addWidget(logView_, 1);

    process_->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process_, &QProcess::readyReadStandardOutput, this, &FlashPanel::ReadOutput);
    connect(process_, &QProcess::readyReadStandardError, this, [this] {
        const QString text = QString::fromUtf8(process_->readAllStandardError()).trimmed();
        if (!text.isEmpty()) logView_->appendPlainText(text);
    });
    connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this](int code, QProcess::ExitStatus) { Finish(code); });
}

void FlashPanel::OnFlashClicked() {
    if (process_->state() != QProcess::NotRunning) return;
    const QString firmware = pathEdit_->text().trimmed();
    if (firmware.isEmpty()) {
        errorLabel_->setText(QStringLiteral("Choose a firmware binary first."));
        return;
    }
    // Remember the last flash target for next time (prefill + browse seed).
    QSettings lastRun(QStringLiteral("RTE"), QStringLiteral("RTEStudio"));
    lastRun.setValue(QStringLiteral("flash/lastFirmware"), firmware);
    lastRun.setValue(QStringLiteral("flash/lastDir"),
                     QFileInfo(firmware).dir().absolutePath());
    QStringList arguments = {QStringLiteral("--format"), QStringLiteral("jsonl"),
                             QStringLiteral("flash"), QStringLiteral("--firmware"), firmware,
                             QStringLiteral("--serial"), controller_->Port()};
    if (!autoGpioCheck_->isChecked()) arguments << QStringLiteral("--manual-boot");
    outputBuffer_.clear();
    logView_->clear();
    errorLabel_->clear();
    stateLabel_->setText(QStringLiteral("State: Starting"));
    progressBar_->setRange(0, 0);
    progressBar_->show();
    cancelledByUser_ = false;
    flashButton_->setEnabled(false);
    cancelButton_->setEnabled(true);
    controller_->SuspendForFlash();
    process_->start(RteCliPath(), arguments);
    if (!process_->waitForStarted(1000)) {
        errorLabel_->setText(QStringLiteral("Could not start rte: %1").arg(process_->errorString()));
        Finish(-1);
    }
}

void FlashPanel::OnCancel() {
    if (process_->state() == QProcess::NotRunning) return;
    // The kill runs through the normal finished wiring; the flag makes
    // Finish() report a cancellation instead of a failure.
    cancelledByUser_ = true;
    logView_->appendPlainText(QStringLiteral("[cancelled] flash aborted by the user"));
    cancelButton_->setEnabled(false);
    process_->kill();
}

void FlashPanel::OnBrowse() {
    const QSettings settings(QStringLiteral("RTE"), QStringLiteral("RTEStudio"));
    const QString seedDir = settings.value(QStringLiteral("flash/lastDir")).toString();
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Select firmware binary"), seedDir,
        QStringLiteral("Firmware binaries (*.bin);;All files (*)"));
    if (!path.isEmpty()) pathEdit_->setText(path);
}

void FlashPanel::ReadOutput() {
    outputBuffer_ += process_->readAllStandardOutput();
    for (;;) {
        const qsizetype newline = outputBuffer_.indexOf('\n');
        if (newline < 0) break;
        const QByteArray line = outputBuffer_.left(newline).trimmed();
        outputBuffer_.remove(0, newline + 1);
        if (!line.isEmpty()) AppendJsonLine(line);
    }
}

void FlashPanel::AppendJsonLine(const QByteArray& line) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        logView_->appendPlainText(QString::fromUtf8(line));
        return;
    }
    const QJsonObject object = document.object();
    const QString message = object.value(QStringLiteral("message")).toString();
    if (!message.isEmpty()) logView_->appendPlainText(message);
    if (object.value(QStringLiteral("event")).toString() == QStringLiteral("error")) {
        errorLabel_->setText(message);
    }
    const QString phase = object.value(QStringLiteral("phase")).toString();
    if (!phase.isEmpty()) stateLabel_->setText(QStringLiteral("State: %1").arg(phase));
    const int percent = object.value(QStringLiteral("percent")).toInt(-1);
    if (percent >= 0) {
        progressBar_->setRange(0, 100);
        progressBar_->setValue(percent);
    }
}

void FlashPanel::Finish(int exitCode) {
    controller_->ResumeAfterFlash();
    flashButton_->setEnabled(true);
    cancelButton_->setEnabled(false);
    progressBar_->setRange(0, 100);
    if (cancelledByUser_) {
        cancelledByUser_ = false;
        progressBar_->hide();
        stateLabel_->setText(QStringLiteral("State: Cancelled"));
        return;
    }
    if (exitCode == 0) {
        progressBar_->setValue(100);
        stateLabel_->setText(QStringLiteral("State: Complete"));
    } else {
        progressBar_->hide();
        stateLabel_->setText(QStringLiteral("State: Failed"));
        if (errorLabel_->text().isEmpty()) {
            errorLabel_->setText(QStringLiteral("rte flash exited with code %1").arg(exitCode));
        }
    }
}

}  // namespace NodeGUI::runtime
