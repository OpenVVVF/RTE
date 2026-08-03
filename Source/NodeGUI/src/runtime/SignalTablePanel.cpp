#include "SignalTablePanel.h"

#include "RuntimeController.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QSlider>
#include <QTableWidget>
#include <QVBoxLayout>

namespace NodeGUI::runtime {

SignalTablePanel::SignalTablePanel(RuntimeController* controller, QWidget* parent)
    : QWidget(parent)
    , controller_(controller) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* controlsRow = new QHBoxLayout;
    controlsRow->addWidget(new QLabel(QStringLiteral("Plot view (sec)"), this));
    viewSlider_ = new QSlider(Qt::Horizontal, this);
    viewSlider_->setRange(5, 600);  // 0.5 .. 60.0 s in 0.1 s steps
    viewSlider_->setValue(50);
    connect(viewSlider_, &QSlider::valueChanged, this, &SignalTablePanel::OnViewSecondsChanged);
    controlsRow->addWidget(viewSlider_, 1);
    filterEdit_ = new QLineEdit(this);
    filterEdit_->setPlaceholderText(QStringLiteral("Filter"));
    connect(filterEdit_, &QLineEdit::textChanged, this, &SignalTablePanel::OnFilterChanged);
    controlsRow->addWidget(filterEdit_, 1);
    layout->addLayout(controlsRow);

    signalTable_ = new QTableWidget(0, 5, this);
    signalTable_->setHorizontalHeaderLabels(
        {QStringLiteral("G1"), QStringLiteral("G2"), QStringLiteral("G3"),
         QStringLiteral("Signal"), QStringLiteral("Value")});
    // User-adjustable columns with sensible starting widths; the Value column
    // soaks up extra width when the dock is resized.
    signalTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    signalTable_->horizontalHeader()->setStretchLastSection(true);
    signalTable_->setColumnWidth(0, 30);
    signalTable_->setColumnWidth(1, 30);
    signalTable_->setColumnWidth(2, 30);
    signalTable_->setColumnWidth(3, 170);
    signalTable_->setColumnWidth(4, 90);
    signalTable_->verticalHeader()->setVisible(false);
    signalTable_->setSelectionMode(QAbstractItemView::NoSelection);
    layout->addWidget(signalTable_, 1);

    connect(controller_, &RuntimeController::storeChanged,
            this, &SignalTablePanel::OnStoreChanged);
}

std::array<QStringList, 3> SignalTablePanel::GraphSignalSets() const {
    return graphSignals_;
}

void SignalTablePanel::SetGraphSignalSets(const std::array<QStringList, 3>& sets) {
    graphSignals_ = sets;
    RebuildSignalTable();
    emit graphSignalsChanged(graphSignals_);
}

void SignalTablePanel::OnStoreChanged() {
    RebuildSignalTable();

    // Refresh the Value column in place.
    for (int row = 0; row < signalTable_->rowCount(); ++row) {
        const auto key = signalTable_->item(row, 3)->text().toStdString();
        float value = 0.0f;
        if (controller_->Store().LatestValue(key, value)) {
            signalTable_->item(row, 4)->setText(QString::asprintf("% .6f", value));
        }
    }
}

void SignalTablePanel::OnFilterChanged(const QString& /*text*/) {
    RebuildSignalTable();
}

void SignalTablePanel::OnViewSecondsChanged(int value) {
    emit viewSecondsChanged(value / 10.0);
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
        signalTable_->setItem(row, 4, valueItem);
    }
    rebuildingTable_ = false;
}

}  // namespace NodeGUI::runtime
