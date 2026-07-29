#include "ConsolePanel.h"

#include "RuntimeController.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace NodeGUI::runtime {

namespace {

class HistoryLineEdit : public QLineEdit {
public:
    std::function<void(int)> onHistoryStep;  // -1 = older, +1 = newer

protected:
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Up && onHistoryStep) {
            onHistoryStep(-1);
            return;
        }
        if (event->key() == Qt::Key_Down && onHistoryStep) {
            onHistoryStep(+1);
            return;
        }
        QLineEdit::keyPressEvent(event);
    }
};

}  // namespace

ConsolePanel::ConsolePanel(RuntimeController* controller, QWidget* parent)
    : QWidget(parent)
    , controller_(controller) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* buttonsRow = new QHBoxLayout;
    auto* clearButton = new QPushButton(QStringLiteral("Clear"), this);
    connect(clearButton, &QPushButton::clicked, this, [this] {
        controller_->Store().ClearConsole();
        consoleView_->clear();
        lastConsoleSeq_ = 0;
    });
    buttonsRow->addWidget(clearButton);
    autoscrollCheck_ = new QCheckBox(QStringLiteral("Autoscroll"), this);
    autoscrollCheck_->setChecked(true);
    buttonsRow->addWidget(autoscrollCheck_);
    buttonsRow->addStretch(1);
    buttonsRow->addWidget(new QLabel(QStringLiteral("Commands"), this));
    layout->addLayout(buttonsRow);

    consoleView_ = new QPlainTextEdit(this);
    consoleView_->setReadOnly(true);
    consoleView_->setMaximumBlockCount(static_cast<int>(TelemetryStore::kConsoleCapLines));
    layout->addWidget(consoleView_, 1);

    auto* sendRow = new QHBoxLayout;
    sendRow->addWidget(new QLabel(QStringLiteral("Send:"), this));
    auto* commandEdit = new HistoryLineEdit;
    commandEdit_ = commandEdit;
    commandEdit->onHistoryStep = [this](int dir) {
        if (commandHistory_.isEmpty()) {
            return;
        }
        if (historyIndex_ < 0) {
            historyIndex_ = static_cast<int>(commandHistory_.size());
        }
        historyIndex_ = std::clamp(historyIndex_ + dir, 0,
                                   static_cast<int>(commandHistory_.size()) - 1);
        commandEdit_->setText(commandHistory_[historyIndex_]);
    };
    connect(commandEdit, &QLineEdit::returnPressed, this, &ConsolePanel::OnSendCommand);
    sendRow->addWidget(commandEdit, 1);
    auto* sendButton = new QPushButton(QStringLiteral("Send"), this);
    connect(sendButton, &QPushButton::clicked, this, &ConsolePanel::OnSendCommand);
    sendRow->addWidget(sendButton);
    layout->addLayout(sendRow);

    connect(controller_, &RuntimeController::storeChanged,
            this, &ConsolePanel::OnStoreChanged);
}

void ConsolePanel::OnStoreChanged() {
    // Detect a ClearConsole() from another console instance: the store's
    // newest seq moved backwards.
    if (controller_->Store().LatestConsoleSeq() < lastConsoleSeq_) {
        consoleView_->clear();
        lastConsoleSeq_ = 0;
    }

    const auto lines = controller_->Store().ConsoleSince(lastConsoleSeq_);
    for (const auto& line : lines) {
        consoleView_->appendPlainText(QString::fromStdString(line.text));
        lastConsoleSeq_ = line.seq;
    }
    if (autoscrollCheck_->isChecked() && !lines.empty()) {
        auto bar = consoleView_->verticalScrollBar();
        bar->setValue(bar->maximum());
    }
}

void ConsolePanel::OnSendCommand() {
    const QString line = commandEdit_->text().trimmed();
    if (line.isEmpty()) {
        return;
    }
    commandEdit_->clear();
    if (commandHistory_.isEmpty() || commandHistory_.last() != line) {
        commandHistory_.push_back(line);
    }
    historyIndex_ = -1;
    controller_->SendCommand(line);
    OnStoreChanged();
}

}  // namespace NodeGUI::runtime
