#include "SignalTablePanel.h"

#include "RuntimeController.h"

#include "SignalUnits.h"

#include <QCheckBox>
#include <QColor>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace NodeGUI::runtime {

namespace {

constexpr double kViewMinSeconds = 0.01;
constexpr double kViewMaxSeconds = 60.0;

double SliderPosToSeconds(int pos) {
    const double f = static_cast<double>(pos) / SignalTablePanel::kViewSliderSteps;
    return kViewMinSeconds * std::pow(kViewMaxSeconds / kViewMinSeconds, f);
}

int SecondsToSliderPos(double seconds) {
    const double clamped = std::clamp(seconds, kViewMinSeconds, kViewMaxSeconds);
    const double f = std::log(clamped / kViewMinSeconds) /
                     std::log(kViewMaxSeconds / kViewMinSeconds);
    return static_cast<int>(std::lround(f * SignalTablePanel::kViewSliderSteps));
}

QString FormatViewSeconds(double seconds) {
    if (seconds < 1.0) {
        return QStringLiteral("%1 ms").arg(seconds * 1000.0, 0, 'f', 0);
    }
    return QStringLiteral("%1 s").arg(seconds, 0, 'f', seconds < 10.0 ? 1 : 0);
}

}  // namespace

SignalTablePanel::SignalTablePanel(RuntimeController* controller, QWidget* parent)
    : QWidget(parent)
    , controller_(controller) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* viewRow = new QHBoxLayout;
    viewRow->addWidget(new QLabel(QStringLiteral("Plot view"), this));
    viewSlider_ = new QSlider(Qt::Horizontal, this);
    // Log scale: a linear slider spends almost all of its travel above 10 s,
    // but the useful PWM range is tens of milliseconds.
    viewSlider_->setRange(0, kViewSliderSteps);
    connect(viewSlider_, &QSlider::valueChanged, this, &SignalTablePanel::OnViewSecondsChanged);
    viewRow->addWidget(viewSlider_, 1);
    viewValueLabel_ = new QLabel(this);
    viewValueLabel_->setMinimumWidth(64);
    viewValueLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    viewRow->addWidget(viewValueLabel_);
    layout->addLayout(viewRow);

    auto* presetRow = new QHBoxLayout;
    presetRow->setSpacing(4);
    for (const double seconds : {0.01, 0.1, 0.5, 2.0, 10.0, 60.0}) {
        auto* button = new QPushButton(FormatViewSeconds(seconds), this);
        button->setMaximumWidth(52);
        connect(button, &QPushButton::clicked, this,
                [this, seconds] { SetViewSeconds(seconds); });
        presetRow->addWidget(button);
    }
    filterEdit_ = new QLineEdit(this);
    filterEdit_->setPlaceholderText(QStringLiteral("Filter"));
    connect(filterEdit_, &QLineEdit::textChanged, this, &SignalTablePanel::OnFilterChanged);
    presetRow->addWidget(filterEdit_, 1);
    layout->addLayout(presetRow);

    SetViewSeconds(0.5);

    signalTable_ = new QTableWidget(0, 6, this);
    signalTable_->setHorizontalHeaderLabels(
        {QStringLiteral("G1"), QStringLiteral("G2"), QStringLiteral("G3"),
         QStringLiteral("Signal"), QStringLiteral("Value"), QStringLiteral("Unit")});
    for (int col = 0; col < 3; ++col) {
        signalTable_->horizontalHeader()->setSectionResizeMode(col, QHeaderView::Fixed);
        signalTable_->setColumnWidth(col, 30);
        signalTable_->horizontalHeaderItem(col)->setToolTip(
            QStringLiteral("Assign this signal to graph %1").arg(col + 1));
    }
    signalTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    signalTable_->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    signalTable_->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    signalTable_->verticalHeader()->setVisible(false);
    signalTable_->setSelectionMode(QAbstractItemView::NoSelection);
    layout->addWidget(signalTable_, 1);

    connect(controller_, &RuntimeController::storeChanged,
            this, &SignalTablePanel::OnStoreChanged);

    valueTimer_ = new QTimer(this);
    valueTimer_->setInterval(100);  // 10 Hz value column refresh
    connect(valueTimer_, &QTimer::timeout, this, &SignalTablePanel::RefreshValues);
    valueTimer_->start();
}

std::array<QStringList, 3> SignalTablePanel::GraphSignalSets() const {
    return graphSignals_;
}

void SignalTablePanel::SetGraphSignalSets(const std::array<QStringList, 3>& sets) {
    graphSignals_ = sets;
    RebuildSignalTable();
    SyncCheckStates();
    emit graphSignalsChanged(graphSignals_);
}

void SignalTablePanel::SyncCheckStates() {
    // RebuildSignalTable() is a no-op when the row set is unchanged, so a
    // preset applied over an already-populated table would otherwise move the
    // plots while the checkboxes kept showing the previous assignment.
    rebuildingTable_ = true;
    for (int row = 0; row < signalTable_->rowCount(); ++row) {
        const QString name = signalTable_->item(row, 3)->text();
        for (int col = 0; col < 3; ++col) {
            signalTable_->item(row, col)->setCheckState(
                graphSignals_[col].contains(name) ? Qt::Checked : Qt::Unchecked);
        }
    }
    rebuildingTable_ = false;
}

void SignalTablePanel::OnStoreChanged() {
    const auto names = controller_->Store().SignalNames();
    if (names.size() != knownSignalCount_) {
        knownSignalCount_ = names.size();
        RebuildSignalTable();
    }
    valuesDirty_ = true;
}

void SignalTablePanel::RefreshValues() {
    if (signalTable_->rowCount() == 0) {
        return;
    }
    valuesDirty_ = false;
    for (int row = 0; row < signalTable_->rowCount(); ++row) {
        const auto key = signalTable_->item(row, 3)->text().toStdString();
        float value = 0.0f;
        if (controller_->Store().LatestValue(key, value)) {
            signalTable_->item(row, 4)->setText(FormatSignalValue(value));
        }
    }
}

void SignalTablePanel::OnFilterChanged(const QString& /*text*/) {
    RebuildSignalTable();
}

void SignalTablePanel::OnViewSecondsChanged(int value) {
    const double seconds = SliderPosToSeconds(value);
    viewValueLabel_->setText(FormatViewSeconds(seconds));
    emit viewSecondsChanged(seconds);
}

void SignalTablePanel::SetViewSeconds(double seconds) {
    const QSignalBlocker block(viewSlider_);
    viewSlider_->setValue(SecondsToSliderPos(seconds));
    viewValueLabel_->setText(FormatViewSeconds(seconds));
    emit viewSecondsChanged(seconds);
}

void SignalTablePanel::RebuildSignalTable() {
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
            // A real centered QCheckBox: a QTableWidgetItem checkbox would
            // leave an empty ghost text element next to the box.
            auto* check = new QCheckBox(signalTable_);
            check->setChecked(graphSignals_[col].contains(names[row]));
            auto* cell = new QWidget(signalTable_);
            auto* cellLayout = new QHBoxLayout(cell);
            cellLayout->setContentsMargins(0, 0, 0, 0);
            cellLayout->setAlignment(Qt::AlignCenter);
            cellLayout->addWidget(check);
            signalTable_->setCellWidget(row, col, cell);

            connect(check, &QCheckBox::toggled, this,
                    [this, row, col](bool checked) {
                        if (rebuildingTable_) {
                            return;
                        }
                        const QString name = signalTable_->item(row, 3)->text();
                        auto& set = graphSignals_[col];
                        if (checked) {
                            if (!set.contains(name)) {
                                set.push_back(name);
                            }
                        } else {
                            set.removeAll(name);
                        }
                        emit graphSignalsChanged(graphSignals_);
                    });
        }
        auto* nameItem = new QTableWidgetItem(names[row]);
        nameItem->setFlags(Qt::ItemIsEnabled);
        signalTable_->setItem(row, 3, nameItem);
        auto* valueItem = new QTableWidgetItem;
        valueItem->setFlags(Qt::ItemIsEnabled);
        valueItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        signalTable_->setItem(row, 4, valueItem);
        auto* unitItem = new QTableWidgetItem(UnitSuffixForSignal(names[row]));
        unitItem->setFlags(Qt::ItemIsEnabled);
        unitItem->setForeground(QColor(150, 155, 162));
        signalTable_->setItem(row, 5, unitItem);
    }
    rebuildingTable_ = false;
    knownSignalCount_ = static_cast<std::size_t>(names.size());
    valuesDirty_ = true;
}

}  // namespace NodeGUI::runtime
