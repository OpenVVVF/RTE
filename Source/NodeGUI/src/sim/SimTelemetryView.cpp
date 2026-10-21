#include "SimTelemetryView.h"

#include "runtime/SignalPlotWidget.h"

#include <QCheckBox>
#include <QColor>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace NodeGUI::sim {

namespace {

constexpr int kSignalColumn = 3;
constexpr int kValueColumn = 4;

// First-load favorites, checked in order: G1 currents, G2 DC link, G3 speed.
// Applied only when the user's sets come up empty for the new session.
const std::array<QStringList, 3> kDefaultSignals = {
    QStringList{QStringLiteral("cg_id_a"), QStringLiteral("cg_iq_a")},
    QStringList{QStringLiteral("vdc_v")},
    QStringList{QStringLiteral("Mech_RPM"), QStringLiteral("Elec_RPM")}};

QWidget* CenteredCheckCell(QTableWidget* table, QCheckBox* check) {
    auto* cell = new QWidget(table);
    auto* layout = new QHBoxLayout(cell);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setAlignment(Qt::AlignCenter);
    layout->addWidget(check);
    return cell;
}

}  // namespace

SimTelemetryView::SimTelemetryView(QWidget* parent)
    : QWidget(parent) {
    stack_ = new QStackedWidget(this);

    auto* emptyLabel = new QLabel(
        QStringLiteral("No session yet — run a scenario or load a session file."),
        this);
    emptyLabel->setAlignment(Qt::AlignCenter);
    stack_->addWidget(emptyLabel);

    auto* content = new QWidget(this);
    stack_->addWidget(content);

    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);

    auto* splitter = new QSplitter(Qt::Horizontal, content);

    auto* leftPanel = new QWidget(splitter);
    auto* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    filterEdit_ = new QLineEdit(leftPanel);
    filterEdit_->setObjectName(QStringLiteral("simSignalFilter"));
    filterEdit_->setPlaceholderText(QStringLiteral("Filter"));
    connect(filterEdit_, &QLineEdit::textChanged, this,
            [this] { RebuildSignalTable(); });
    leftLayout->addWidget(filterEdit_);

    signalTable_ = new QTableWidget(0, 5, leftPanel);
    signalTable_->setHorizontalHeaderLabels(
        {QStringLiteral("G1"), QStringLiteral("G2"), QStringLiteral("G3"),
         QStringLiteral("Signal"), QStringLiteral("Value")});
    signalTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    signalTable_->horizontalHeader()->setStretchLastSection(true);
    signalTable_->setColumnWidth(0, 30);
    signalTable_->setColumnWidth(1, 30);
    signalTable_->setColumnWidth(2, 30);
    signalTable_->setColumnWidth(3, 170);
    signalTable_->setColumnWidth(4, 90);
    signalTable_->verticalHeader()->setVisible(false);
    signalTable_->setSelectionMode(QAbstractItemView::NoSelection);
    leftLayout->addWidget(signalTable_, 1);
    splitter->addWidget(leftPanel);

    auto* rightSplit = new QSplitter(Qt::Vertical, splitter);
    for (int i = 0; i < 3; ++i) {
        plots_[i] = new runtime::SignalPlotWidget(
            QStringLiteral("Graph %1").arg(i + 1), rightSplit);
        plots_[i]->SetStore(&store_);
        rightSplit->addWidget(plots_[i]);
    }
    rightSplit->setSizes({160, 160, 160});
    splitter->addWidget(rightSplit);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({370, 620});

    contentLayout->addWidget(splitter, 1);

    statusLabel_ = new QLabel(content);
    statusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    contentLayout->addWidget(statusLabel_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(stack_);
}

bool SimTelemetryView::LoadSession(const QString& path, QString& error) {
    auto session = std::make_unique<SimSession>();
    if (!session->Load(path, error)) {
        return false;
    }
    session_ = std::move(session);

    store_.ClearSession();
    store_.ClearConsole();
    session_->PopulateStore(store_);

    // Keep only assignments that still exist in this session; when that
    // leaves every graph empty, fall back to the card-reader defaults.
    const QStringList names = session_->ChannelNames();
    for (QStringList& set : graphSignals_) {
        set.erase(std::remove_if(set.begin(), set.end(),
                                 [&names](const QString& name) {
                                     return !names.contains(name);
                                 }),
                  set.end());
    }
    if (graphSignals_[0].isEmpty() && graphSignals_[1].isEmpty()
        && graphSignals_[2].isEmpty()) {
        ApplyDefaultAssignments();
    }
    ApplyAssignments();

    for (auto* plot : plots_) {
        plot->SetViewSeconds(std::clamp(session_->Meta().durationS, 0.5, 60.0));
        plot->Refresh();
    }

    RebuildSignalTable();
    UpdateStatusLine();
    stack_->setCurrentIndex(1);
    return true;
}

void SimTelemetryView::ApplyDefaultAssignments() {
    for (int group = 0; group < 3; ++group) {
        QStringList& set = graphSignals_[group];
        for (const QString& favorite : kDefaultSignals[group]) {
            const SimChannelStats stats = session_->ChannelStats(favorite);
            if (stats.finiteSamples > 0 && !set.contains(favorite)) {
                set.push_back(favorite);
            }
        }
    }
}

void SimTelemetryView::ApplyAssignments() {
    for (int i = 0; i < 3; ++i) {
        plots_[i]->SetSignals(graphSignals_[i]);
        plots_[i]->Refresh();
    }
}

void SimTelemetryView::UpdateStatusLine() {
    if (!session_) {
        statusLabel_->clear();
        return;
    }
    const SimSessionMeta& meta = session_->Meta();
    int idleChannels = 0;
    for (const QString& name : session_->ChannelNames()) {
        if (session_->ChannelStats(name).finiteSamples == 0) {
            ++idleChannels;
        }
    }
    statusLabel_->setText(QStringLiteral(
        "%1 frames · %2 channels (%3 all-null) · %4 s · plant: %5")
            .arg(meta.telemetryFrames)
            .arg(session_->ChannelNames().size())
            .arg(idleChannels)
            .arg(meta.durationS, 0, 'f', 3)
            .arg(meta.plant.isEmpty() ? QStringLiteral("(no stamp)")
                                      : meta.plant));
}

void SimTelemetryView::RebuildSignalTable() {
    if (!session_) {
        signalTable_->setRowCount(0);
        return;
    }

    QStringList names;
    const QString filter = filterEdit_->text();
    for (const QString& name : session_->ChannelNames()) {
        if (filter.isEmpty() || name.contains(filter, Qt::CaseInsensitive)) {
            names.push_back(name);
        }
    }

    QStringList current;
    current.reserve(signalTable_->rowCount());
    for (int row = 0; row < signalTable_->rowCount(); ++row) {
        current.push_back(signalTable_->item(row, kSignalColumn)->text());
    }
    if (current == names) {
        return;
    }

    rebuildingTable_ = true;
    signalTable_->setRowCount(0);
    signalTable_->setRowCount(static_cast<int>(names.size()));
    for (int row = 0; row < names.size(); ++row) {
        const QString& name = names[row];
        const SimChannelStats stats = session_->ChannelStats(name);
        const bool dead = stats.finiteSamples == 0;

        for (int col = 0; col < 3; ++col) {
            auto* check = new QCheckBox(signalTable_);
            check->setChecked(graphSignals_[col].contains(name));
            check->setEnabled(!dead);
            if (dead) {
                check->setToolTip(QStringLiteral(
                    "Every sample in this channel is null — nothing to plot."));
            }
            signalTable_->setCellWidget(row, col,
                                        CenteredCheckCell(signalTable_, check));
            connect(check, &QCheckBox::toggled, this,
                    [this, row, col](bool checked) {
                        if (rebuildingTable_) {
                            return;
                        }
                        const QString key =
                            signalTable_->item(row, kSignalColumn)->text();
                        QStringList& set = graphSignals_[col];
                        if (checked) {
                            if (!set.contains(key)) {
                                set.push_back(key);
                            }
                        } else {
                            set.removeAll(key);
                        }
                        ApplyAssignments();
                    });
        }

        auto* nameItem = new QTableWidgetItem(name);
        nameItem->setFlags(Qt::ItemIsEnabled);
        auto* valueItem = new QTableWidgetItem;
        valueItem->setFlags(Qt::ItemIsEnabled);
        valueItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (dead) {
            const QBrush gray(QColor(140, 140, 140));
            nameItem->setForeground(gray);
            valueItem->setForeground(gray);
            nameItem->setToolTip(QStringLiteral(
                "Every sample in this channel is null."));
            valueItem->setText(QStringLiteral("null"));
        } else {
            valueItem->setText(QString::number(stats.lastFinite, 'g', 6));
        }
        signalTable_->setItem(row, kSignalColumn, nameItem);
        signalTable_->setItem(row, kValueColumn, valueItem);
    }
    rebuildingTable_ = false;
}

}  // namespace NodeGUI::sim
