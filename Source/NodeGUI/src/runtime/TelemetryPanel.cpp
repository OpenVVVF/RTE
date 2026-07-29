#include "TelemetryPanel.h"

#include "RuntimeController.h"
#include "SignalPlotWidget.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSlider>
#include <QSplitter>
#include <QTableWidget>
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

TelemetryPanel::TelemetryPanel(RuntimeController* controller, QWidget* parent)
    : QWidget(parent)
    , controller_(controller) {
    auto* rootSplit = new QSplitter(Qt::Horizontal, this);

    // ---- Left: controls + signal table + console ----
    auto* leftSplit = new QSplitter(Qt::Vertical, rootSplit);

    auto* tablePane = new QWidget(leftSplit);
    auto* tableLayout = new QVBoxLayout(tablePane);
    tableLayout->setContentsMargins(0, 0, 0, 0);

    auto* controlsRow = new QHBoxLayout;
    controlsRow->addWidget(new QLabel(QStringLiteral("Plot view (sec)"), tablePane));
    viewSlider_ = new QSlider(Qt::Horizontal, tablePane);
    viewSlider_->setRange(5, 600);  // 0.5 .. 60.0 s in 0.1 s steps
    viewSlider_->setValue(50);
    connect(viewSlider_, &QSlider::valueChanged, this, &TelemetryPanel::OnViewSecondsChanged);
    controlsRow->addWidget(viewSlider_, 1);
    filterEdit_ = new QLineEdit(tablePane);
    filterEdit_->setPlaceholderText(QStringLiteral("Filter"));
    connect(filterEdit_, &QLineEdit::textChanged, this, &TelemetryPanel::OnFilterChanged);
    controlsRow->addWidget(filterEdit_, 1);
    tableLayout->addLayout(controlsRow);

    signalTable_ = new QTableWidget(0, 5, tablePane);
    signalTable_->setHorizontalHeaderLabels(
        {QStringLiteral("G1"), QStringLiteral("G2"), QStringLiteral("G3"),
         QStringLiteral("Signal"), QStringLiteral("Value")});
    signalTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    signalTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    signalTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    signalTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    signalTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    signalTable_->verticalHeader()->setVisible(false);
    signalTable_->setSelectionMode(QAbstractItemView::NoSelection);
    connect(signalTable_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (rebuildingTable_ || item->column() > 2) {
            return;
        }
        const QString name = signalTable_->item(item->row(), 3)->text();
        auto& set = graphSignals_[item->column()];
        if (item->checkState() == Qt::Checked) {
            if (!set.contains(name)) {
                set.push_back(name);
            }
        } else {
            set.removeAll(name);
        }
        ApplyGraphAssignments();
    });
    tableLayout->addWidget(signalTable_, 1);

    auto* consolePane = new QWidget(leftSplit);
    auto* consoleLayout = new QVBoxLayout(consolePane);
    consoleLayout->setContentsMargins(0, 0, 0, 0);

    auto* consoleButtons = new QHBoxLayout;
    auto* clearButton = new QPushButton(QStringLiteral("Clear"), consolePane);
    connect(clearButton, &QPushButton::clicked, this, [this] {
        controller_->Store().ClearConsole();
        consoleView_->clear();
        lastConsoleSeq_ = 0;
    });
    consoleButtons->addWidget(clearButton);
    autoscrollCheck_ = new QCheckBox(QStringLiteral("Autoscroll"), consolePane);
    autoscrollCheck_->setChecked(true);
    consoleButtons->addWidget(autoscrollCheck_);
    consoleButtons->addStretch(1);
    consoleButtons->addWidget(new QLabel(QStringLiteral("Commands"), consolePane));
    consoleLayout->addLayout(consoleButtons);

    consoleView_ = new QPlainTextEdit(consolePane);
    consoleView_->setReadOnly(true);
    consoleView_->setMaximumBlockCount(static_cast<int>(TelemetryStore::kConsoleCapLines));
    consoleLayout->addWidget(consoleView_, 1);

    auto* sendRow = new QHBoxLayout;
    sendRow->addWidget(new QLabel(QStringLiteral("Send:"), consolePane));
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
    connect(commandEdit, &QLineEdit::returnPressed, this, &TelemetryPanel::OnSendCommand);
    sendRow->addWidget(commandEdit, 1);
    auto* sendButton = new QPushButton(QStringLiteral("Send"), consolePane);
    connect(sendButton, &QPushButton::clicked, this, &TelemetryPanel::OnSendCommand);
    sendRow->addWidget(sendButton);
    consoleLayout->addLayout(sendRow);

    leftSplit->addWidget(tablePane);
    leftSplit->addWidget(consolePane);
    leftSplit->setSizes({400, 300});

    // ---- Right: three stacked plots ----
    auto* rightSplit = new QSplitter(Qt::Vertical, rootSplit);
    for (int i = 0; i < 3; ++i) {
        plots_[i] = new SignalPlotWidget(QStringLiteral("Graph %1").arg(i + 1), rightSplit);
        plots_[i]->SetStore(&controller_->Store());
        plots_[i]->SetViewSeconds(viewSlider_->value() / 10.0);
        rightSplit->addWidget(plots_[i]);
    }
    rightSplit->setSizes({220, 220, 220});

    rootSplit->addWidget(leftSplit);
    rootSplit->addWidget(rightSplit);
    rootSplit->setSizes({520, 800});

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(rootSplit);

    connect(controller_, &RuntimeController::storeChanged,
            this, &TelemetryPanel::OnStoreChanged);
}

std::array<QStringList, 3> TelemetryPanel::GraphSignalSets() const {
    return graphSignals_;
}

void TelemetryPanel::SetGraphSignalSets(const std::array<QStringList, 3>& sets) {
    graphSignals_ = sets;
    RebuildSignalTable();
    ApplyGraphAssignments();
}

void TelemetryPanel::OnStoreChanged() {
    RebuildSignalTable();

    // Refresh the Value column in place.
    for (int row = 0; row < signalTable_->rowCount(); ++row) {
        const auto key = signalTable_->item(row, 3)->text().toStdString();
        float value = 0.0f;
        if (controller_->Store().LatestValue(key, value)) {
            signalTable_->item(row, 4)->setText(QString::asprintf("% .6f", value));
        }
    }

    AppendConsoleLines();

    for (auto* plot : plots_) {
        plot->Refresh();
    }
}

void TelemetryPanel::OnSendCommand() {
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
    AppendConsoleLines();
}

void TelemetryPanel::OnFilterChanged(const QString& /*text*/) {
    RebuildSignalTable();
}

void TelemetryPanel::OnViewSecondsChanged(int value) {
    const double seconds = value / 10.0;
    for (auto* plot : plots_) {
        plot->SetViewSeconds(seconds);
    }
}

void TelemetryPanel::RebuildSignalTable() {
    // Only rebuild when the visible name set changes; otherwise rows would
    // reset their scroll position and checkboxes every refresh.
    const auto namesStd = controller_->Store().SignalNames();
    QStringList names;
    names.reserve(static_cast<qsizetype>(namesStd.size()));
    const QString filter = filterEdit_->text();
    for (const auto& name : namesStd) {
        const QString qname = QString::fromStdString(name);
        if (filter.isEmpty() || qname.contains(filter, Qt::CaseInsensitive)) {
            names.push_back(qname);
        }
    }

    QStringList current;
    current.reserve(signalTable_->rowCount());
    for (int row = 0; row < signalTable_->rowCount(); ++row) {
        current.push_back(signalTable_->item(row, 3)->text());
    }
    if (current == names) {
        return;
    }

    rebuildingTable_ = true;
    signalTable_->setRowCount(0);
    signalTable_->setRowCount(static_cast<int>(names.size()));
    for (int row = 0; row < names.size(); ++row) {
        for (int col = 0; col < 3; ++col) {
            auto* check = new QTableWidgetItem;
            check->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            check->setCheckState(graphSignals_[col].contains(names[row]) ? Qt::Checked
                                                                         : Qt::Unchecked);
            signalTable_->setItem(row, col, check);
        }
        auto* nameItem = new QTableWidgetItem(names[row]);
        nameItem->setFlags(Qt::ItemIsEnabled);
        signalTable_->setItem(row, 3, nameItem);
        auto* valueItem = new QTableWidgetItem;
        valueItem->setFlags(Qt::ItemIsEnabled);
        signalTable_->setItem(row, 4, valueItem);
    }
    rebuildingTable_ = false;
}

void TelemetryPanel::ApplyGraphAssignments() {
    for (int i = 0; i < 3; ++i) {
        plots_[i]->SetSignals(graphSignals_[i]);
    }
}

void TelemetryPanel::AppendConsoleLines() {
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

}  // namespace NodeGUI::runtime
