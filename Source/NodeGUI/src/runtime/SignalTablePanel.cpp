#include "SignalTablePanel.h"

#include "RuntimeController.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QSlider>
#include <QTableWidget>
#include <QVBoxLayout>

namespace NodeGUI::runtime {

namespace {
constexpr int kSignalColumn = 0;
constexpr int kTargetColumn = 1;
constexpr int kValueColumn = 2;
}  // namespace

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

    signalTable_ = new QTableWidget(0, 3, this);
    signalTable_->setObjectName(QStringLiteral("rtSignalTable"));
    signalTable_->setHorizontalHeaderLabels(
        {QStringLiteral("Signal"), QStringLiteral("Graph"), QStringLiteral("Value")});
    signalTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    signalTable_->horizontalHeader()->setStretchLastSection(true);
    signalTable_->setColumnWidth(kSignalColumn, 170);
    signalTable_->setColumnWidth(kTargetColumn, 120);
    signalTable_->setColumnWidth(kValueColumn, 90);
    signalTable_->verticalHeader()->setVisible(false);
    signalTable_->setSelectionMode(QAbstractItemView::NoSelection);
    layout->addWidget(signalTable_, 1);

    connect(controller_, &RuntimeController::storeChanged,
            this, &SignalTablePanel::OnStoreChanged);
}

std::vector<QStringList> SignalTablePanel::GraphSignalSets() const {
    return graphSignals_;
}

void SignalTablePanel::SetGraphSignalSets(const std::vector<QStringList>& sets) {
    graphSignals_ = sets;
    RebuildSignalTable();
    emit graphSignalsChanged(graphSignals_);
}

void SignalTablePanel::SetGraphCount(int count) {
    count = std::max(1, count);
    while (static_cast<int>(graphSignals_.size()) < count) {
        graphSignals_.emplace_back();
    }
    if (static_cast<int>(graphSignals_.size()) > count) {
        // Removed graphs drop their sets: their signals revert to
        // unassigned (the combo falls back to "--" on rebuild).
        graphSignals_.resize(count);
        emit graphSignalsChanged(graphSignals_);
    }
    RebuildSignalTable();
}

void SignalTablePanel::PopulateTargetCombo(QComboBox* combo) const {
    combo->clear();
    combo->addItem(QStringLiteral("\u2014"));   // em dash = unassigned
    for (int i = 0; i < static_cast<int>(graphSignals_.size()); ++i) {
        combo->addItem(QStringLiteral("G%1").arg(i + 1));
    }
}

int SignalTablePanel::AssignedGraph(const QString& name) const {
    for (int i = 0; i < static_cast<int>(graphSignals_.size()); ++i) {
        if (graphSignals_[i].contains(name)) {
            return i;
        }
    }
    return -1;
}

void SignalTablePanel::OnStoreChanged() {
    RebuildSignalTable();
    RefreshValueColumn();
}

void SignalTablePanel::SetCursor(bool hasCursor, double tsec) {
    hasCursor_ = hasCursor;
    cursorTsec_ = tsec;
    RefreshValueColumn();
}

void SignalTablePanel::RefreshValueColumn() {
    // Value column: latest by default; interpolated value at the pinned
    // cursor time while a cursor holds (live keeps streaming behind).
    for (int row = 0; row < signalTable_->rowCount(); ++row) {
        const auto key = signalTable_->item(row, kSignalColumn)->text().toStdString();
        float value = 0.0f;
        const bool haveValue =
            (hasCursor_ && controller_->Store().ValueAt(
                               key, static_cast<float>(cursorTsec_), value))
            || controller_->Store().LatestValue(key, value);
        if (haveValue) {
            signalTable_->item(row, kValueColumn)
                ->setText(QString::asprintf("% .6f", value));
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
    // reset their scroll position and assignments every refresh. The graph
    // count no longer participates naturally in that check, so it is
    // handled by an explicit combo repopulation below.
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
        current.push_back(signalTable_->item(row, kSignalColumn)->text());
    }
    const bool sameNames = current == names;
    if (!sameNames) {
        rebuildingTable_ = true;
        signalTable_->setRowCount(0);
        signalTable_->setRowCount(static_cast<int>(names.size()));
        for (int row = 0; row < names.size(); ++row) {
            auto* combo = new QComboBox(signalTable_);
            combo->setObjectName(QStringLiteral("target.%1").arg(names[row]));
            PopulateTargetCombo(combo);
            const int assigned = AssignedGraph(names[row]);
            combo->setCurrentIndex(assigned + 1);
            signalTable_->setCellWidget(row, kTargetColumn, combo);
            connect(combo, &QComboBox::currentIndexChanged, this,
                    [this, row](int index) {
                        if (rebuildingTable_) {
                            return;
                        }
                        const QString name =
                            signalTable_->item(row, kSignalColumn)->text();
                        for (QStringList& set : graphSignals_) {
                            set.removeAll(name);
                        }
                        if (index > 0 && index <= static_cast<int>(graphSignals_.size())) {
                            graphSignals_[index - 1].push_back(name);
                        }
                        emit graphSignalsChanged(graphSignals_);
                    });

            auto* nameItem = new QTableWidgetItem(names[row]);
            nameItem->setFlags(Qt::ItemIsEnabled);
            signalTable_->setItem(row, kSignalColumn, nameItem);
            auto* valueItem = new QTableWidgetItem;
            valueItem->setFlags(Qt::ItemIsEnabled);
            signalTable_->setItem(row, kValueColumn, valueItem);
        }
        rebuildingTable_ = false;
        return;
    }

    // Same names: just re-sync the combos against the current assignment
    // state (graph count changes land here without a full rebuild).
    rebuildingTable_ = true;
    for (int row = 0; row < signalTable_->rowCount(); ++row) {
        auto* combo = qobject_cast<QComboBox*>(
            signalTable_->cellWidget(row, kTargetColumn));
        if (!combo) {
            continue;
        }
        const QString name = signalTable_->item(row, kSignalColumn)->text();
        PopulateTargetCombo(combo);
        const int assigned = AssignedGraph(name);
        combo->setCurrentIndex(assigned + 1);
    }
    rebuildingTable_ = false;
}

}  // namespace NodeGUI::runtime
