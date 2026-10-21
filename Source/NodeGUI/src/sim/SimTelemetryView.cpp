#include "SimTelemetryView.h"

#include "runtime/SignalPlotWidget.h"

#include <QColor>
#include <QComboBox>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace NodeGUI::sim {

namespace {

constexpr int kSignalColumn = 0;
constexpr int kTargetColumn = 1;
constexpr int kValueColumn = 2;

// First-load favorites, checked in order: G1 currents, G2 DC link, G3
// speed (additional graphs start empty). Applied only when neither a
// persisted layout nor the in-memory carry-over covers the new session.
const std::vector<QStringList> kDefaultSignals = {
    QStringList{QStringLiteral("cg_id_a"), QStringLiteral("cg_iq_a")},
    QStringList{QStringLiteral("vdc_v")},
    QStringList{QStringLiteral("Mech_RPM"), QStringLiteral("Elec_RPM")}};

// Corrupt-settings guard for the restored graph count (the runtime autosave
// LoadAutosave reads the same shape without a cap; the sim side caps).
constexpr int kMaxPersistedGraphs = 12;

// Same factory idiom as SimulateScreen::MakeSimSettings /
// RuntimeTab::MakeSettings (per-TU copies of the 3-liner).
QSettings MakeSimSettings() {
    return QSettings(QStringLiteral("RTE"), QStringLiteral("RTEStudio"));
}

// QSettings group components nest on '/', so the session-file stem is
// reduced to a filename-ish alphabet first (OnSavePreset does the same to
// layout names with name.replace('/', '_')).
QString SanitizeLayoutStem(const QString& stem) {
    QString clean = stem;
    for (QChar& ch : clean) {
        if (!ch.isLetterOrNumber() && ch != u'-' && ch != u'_' && ch != u'.') {
            ch = u'_';
        }
    }
    return clean.isEmpty() ? QStringLiteral("session") : clean;
}

// Session-class discriminator: two files that share a stem but carry
// different channel sets (a foc_demo run vs a DSR replay) keep separate
// layouts. SHA-1 because qHash is salted per process and therefore useless
// across restarts; 12 hex chars is ample here.
QString ChannelSetHash(const QStringList& sortedNames) {
    QCryptographicHash digest(QCryptographicHash::Sha1);
    for (const QString& name : sortedNames) {
        digest.addData(name.toUtf8() + '\n');
    }
    return QString::fromLatin1(digest.result().toHex().left(12));
}

QString LayoutGroupFor(const QString& sanitizedStem, const QString& channelHash) {
    return QStringLiteral("sim/layouts/") + sanitizedStem + u'/' + channelHash;
}

QString LayoutStemHashKey(const QString& channelHash) {
    return QStringLiteral("sim/layoutsByHash/") + channelHash;
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

    signalTable_ = new QTableWidget(0, 3, leftPanel);
    // "Last": the column shows the session-final value until a playback
    // cursor exists (the runtime live table's "Value" is live-latest).
    signalTable_->setHorizontalHeaderLabels(
        {QStringLiteral("Signal"), QStringLiteral("Graph"), QStringLiteral("Last")});
    signalTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    signalTable_->horizontalHeader()->setStretchLastSection(true);
    signalTable_->setColumnWidth(kSignalColumn, 170);
    signalTable_->setColumnWidth(kTargetColumn, 110);
    signalTable_->setColumnWidth(kValueColumn, 90);
    signalTable_->verticalHeader()->setVisible(false);
    signalTable_->setSelectionMode(QAbstractItemView::NoSelection);
    leftLayout->addWidget(signalTable_, 1);
    splitter->addWidget(leftPanel);

    auto* rightPanel = new QWidget(splitter);
    auto* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);

    auto* topRow = new QHBoxLayout;
    topRow->setContentsMargins(4, 2, 4, 2);
    topRow->addWidget(new QLabel(QStringLiteral("Graphs"), rightPanel));
    countLabel_ = new QLabel(rightPanel);
    countLabel_->setObjectName(QStringLiteral("simGraphCount"));
    topRow->addWidget(countLabel_);
    topRow->addStretch(1);
    topRow->addWidget(new QLabel(QStringLiteral("Plot view (sec)"), rightPanel));
    // Same idiom as the runtime SignalTablePanel slider: 0.5..60.0 s in
    // 0.1 s steps, driving every plot's SetViewSeconds.
    viewSlider_ = new QSlider(Qt::Horizontal, rightPanel);
    viewSlider_->setObjectName(QStringLiteral("simViewSeconds"));
    viewSlider_->setRange(5, 600);
    viewSlider_->setValue(100);  // 10 s, the SignalPlotWidget default
    connect(viewSlider_, &QSlider::valueChanged, this,
            [this](int) { ApplyViewSeconds(); });
    topRow->addWidget(viewSlider_, 1);
    auto* addButton = new QPushButton(QStringLiteral("+ Graph"), rightPanel);
    addButton->setObjectName(QStringLiteral("simAddGraph"));
    addButton->setToolTip(QStringLiteral("Add another telemetry graph"));
    connect(addButton, &QPushButton::clicked, this, &SimTelemetryView::AddGraph);
    topRow->addWidget(addButton);
    auto* removeButton = new QPushButton(QStringLiteral("- Graph"), rightPanel);
    removeButton->setObjectName(QStringLiteral("simRemoveGraph"));
    removeButton->setToolTip(QStringLiteral(
        "Remove the last graph (its signals unassign)"));
    connect(removeButton, &QPushButton::clicked, this,
            &SimTelemetryView::RemoveLastGraph);
    topRow->addWidget(removeButton);
    rightLayout->addLayout(topRow);

    plotsContainer_ = new QWidget(rightPanel);
    plotsLayout_ = new QVBoxLayout(plotsContainer_);
    plotsLayout_->setContentsMargins(0, 0, 0, 0);
    scroll_ = new QScrollArea(rightPanel);
    scroll_->setWidgetResizable(true);
    scroll_->setWidget(plotsContainer_);
    rightLayout->addWidget(scroll_, 1);
    splitter->addWidget(rightPanel);

    for (int i = 0; i < 3; ++i) {
        plots_.push_back(MakePlot());
    }
    UpdateGraphCount();

    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({370, 620});

    contentLayout->addWidget(splitter, 1);

    statusLabel_ = new QLabel(content);
    statusLabel_->setObjectName(QStringLiteral("simSessionInfo"));
    statusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    contentLayout->addWidget(statusLabel_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(stack_);

    // Write-through debounce for the per-session-class layout autosave
    // (combo edits, graph add/remove); ~500 ms quiet window, then save.
    layoutSaveTimer_ = new QTimer(this);
    layoutSaveTimer_->setSingleShot(true);
    layoutSaveTimer_->setInterval(500);
    connect(layoutSaveTimer_, &QTimer::timeout, this, &SimTelemetryView::SaveLayout);
}

SimTelemetryView::~SimTelemetryView() {
    // A change pending in the debounce window must still land on teardown
    // (the runtime side likewise saves from MainWindow::closeEvent).
    if (layoutSaveTimer_->isActive()) {
        layoutSaveTimer_->stop();
        SaveLayout();
    }
}

NodeGUI::runtime::SignalPlotWidget* SimTelemetryView::MakePlot() {
    auto* plot = new runtime::SignalPlotWidget(
        QStringLiteral("Graph %1").arg(plots_.size() + 1), plotsContainer_);
    plot->SetStore(&store_);
    plot->setMinimumHeight(200);
    // The slider row owns the view window (mirrors the runtime surface).
    plot->SetViewSeconds(viewSlider_ ? viewSlider_->value() / 10.0 : 10.0);
    plotsLayout_->addWidget(plot);
    return plot;
}

void SimTelemetryView::ApplyViewSeconds() {
    const double seconds = viewSlider_ ? viewSlider_->value() / 10.0 : 10.0;
    for (auto* plot : plots_) {
        plot->SetViewSeconds(seconds);
        plot->Refresh();
    }
    UpdateStatusLine();
}

void SimTelemetryView::UpdateGraphCount() {
    while (graphSignals_.size() < plots_.size()) {
        graphSignals_.emplace_back();
    }
    if (graphSignals_.size() > plots_.size()) {
        // Removed graphs drop their sets: those signals revert to
        // unassigned (rows re-sync on RebuildSignalTable).
        graphSignals_.resize(plots_.size());
    }
    countLabel_->setText(QStringLiteral("(%1)").arg(plots_.size()));
}

void SimTelemetryView::AddGraph() {
    plots_.push_back(MakePlot());
    UpdateGraphCount();
    RebuildSignalTable();
    ScheduleLayoutSave();
}

void SimTelemetryView::RemoveLastGraph() {
    if (plots_.size() <= 1) {
        return;
    }
    auto* plot = plots_.back();
    plots_.pop_back();
    plotsLayout_->removeWidget(plot);
    plot->deleteLater();
    UpdateGraphCount();
    RebuildSignalTable();
    ScheduleLayoutSave();
}

int SimTelemetryView::AssignedGraph(const QString& name) const {
    for (int i = 0; i < static_cast<int>(graphSignals_.size()); ++i) {
        if (graphSignals_[i].contains(name)) {
            return i;
        }
    }
    return -1;
}

bool SimTelemetryView::LoadSession(const QString& path, QString& error) {
    auto session = std::make_unique<SimSession>();
    if (!session->Load(path, error)) {
        return false;
    }
    session_ = std::move(session);
    sessionPath_ = path;

    store_.ClearSession();
    store_.ClearConsole();
    // Recorded sessions play back whole: raise THIS store's retention floor
    // above the session's span and longest channel before PopulateStore —
    // the live Runtime store keeps its 30 s / 12000-sample ring (it never
    // calls SetRetentionLimit).
    int maxChannelSamples = 0;
    for (const QString& name : session_->ChannelNames()) {
        maxChannelSamples = std::max(maxChannelSamples,
                                     session_->ChannelStats(name).samples);
    }
    store_.SetRetentionLimit(static_cast<float>(session_->Meta().durationS) + 1.0f,
                             static_cast<std::size_t>(maxChannelSamples) + 16);
    session_->PopulateStore(store_);

    // Restore the persisted layout for this session class (file stem +
    // channel-set hash) ahead of the carry-over/default chain.
    const bool restoredLayout = TryRestoreLayout();

    // Keep only assignments that still exist in this session; when that
    // leaves every graph empty, fall back to the card-reader defaults
    // (skipped when a restored layout deliberately assigns nothing).
    const QStringList names = session_->ChannelNames();
    for (QStringList& set : graphSignals_) {
        set.erase(std::remove_if(set.begin(), set.end(),
                                 [&names](const QString& name) {
                                     return !names.contains(name);
                                 }),
                  set.end());
    }
    const bool anyAssigned = std::any_of(
        graphSignals_.begin(), graphSignals_.end(),
        [](const QStringList& set) { return !set.isEmpty(); });
    if (!anyAssigned && !restoredLayout) {
        ApplyDefaultAssignments();
    }
    ApplyAssignments();

    // The window follows the session duration (capped at the leaf's 60 s);
    // the slider is the single owner of the plots' view seconds.
    const double viewSeconds =
        std::clamp(session_->Meta().durationS, 0.5, 60.0);
    viewSlider_->setValue(std::clamp(qRound(viewSeconds * 10.0), 5, 600));
    ApplyViewSeconds();

    RebuildSignalTable();
    UpdateStatusLine();
    stack_->setCurrentIndex(1);
    return true;
}

void SimTelemetryView::ApplyDefaultAssignments() {
    for (int group = 0;
         group < static_cast<int>(std::min(graphSignals_.size(), kDefaultSignals.size()));
         ++group) {
        QStringList& set = graphSignals_[group];
        for (const QString& favorite : kDefaultSignals[group]) {
            const SimChannelStats stats = session_->ChannelStats(favorite);
            if (stats.finiteSamples > 0 && !set.contains(favorite)) {
                set.push_back(favorite);
            }
        }
    }
}

bool SimTelemetryView::TryRestoreLayout() {
    // The save key always tracks the currently loaded session, even when no
    // persisted entry exists yet: later edits land under THIS session class.
    layoutStem_ = SanitizeLayoutStem(QFileInfo(sessionPath_).completeBaseName());
    layoutChannelHash_ = ChannelSetHash(session_->ChannelNames());

    auto settings = MakeSimSettings();
    QString group = LayoutGroupFor(layoutStem_, layoutChannelHash_);
    if (!settings.contains(group + QStringLiteral("/graphCount"))) {
        // A renamed copy of an earlier session keeps the channel set: borrow
        // the layout most recently stored under the same channel-set hash
        // (read-only — the next edit re-keys it to this stem).
        const QString borrowed =
            settings.value(LayoutStemHashKey(layoutChannelHash_)).toString();
        if (!borrowed.isEmpty() && borrowed != layoutStem_) {
            const QString alt = LayoutGroupFor(borrowed, layoutChannelHash_);
            if (settings.contains(alt + QStringLiteral("/graphCount"))) {
                group = alt;
            }
        }
    }
    if (!settings.contains(group + QStringLiteral("/graphCount"))) {
        return false;
    }

    // Same storage shape as RuntimeTab::LoadAutosave: graphCount + graphN
    // QStringLists inside the group.
    settings.beginGroup(group);
    const int count = std::clamp(
        settings.value(QStringLiteral("graphCount"), 3).toInt(),
        1, kMaxPersistedGraphs);
    std::vector<QStringList> sets;
    for (int i = 0; i < count; ++i) {
        sets.push_back(
            settings.value(QStringLiteral("graph%1").arg(i + 1)).toStringList());
    }
    settings.endGroup();

    graphSignals_ = std::move(sets);
    // Resize the plot row through the same widget add/remove semantics as
    // AddGraph / RemoveLastGraph, then UpdateGraphCount re-syncs; the
    // LoadSession prune right after this drops channels the stored layout
    // names but this session does not carry.
    while (static_cast<int>(plots_.size()) < count) {
        plots_.push_back(MakePlot());
    }
    while (static_cast<int>(plots_.size()) > count) {
        auto* plot = plots_.back();
        plots_.pop_back();
        plotsLayout_->removeWidget(plot);
        plot->deleteLater();
    }
    UpdateGraphCount();
    return true;
}

void SimTelemetryView::ScheduleLayoutSave() {
    if (layoutSaveTimer_ && session_) {
        layoutSaveTimer_->start();  // single-shot: restarts the quiet window
    }
}

void SimTelemetryView::SaveLayout() {
    if (!session_ || layoutStem_.isEmpty() || layoutChannelHash_.isEmpty()) {
        return;
    }
    auto settings = MakeSimSettings();
    // Same graphCount + graphN shape as RuntimeTab::SaveAutosave, one group
    // per session class.
    settings.beginGroup(LayoutGroupFor(layoutStem_, layoutChannelHash_));
    settings.setValue(QStringLiteral("graphCount"),
                      static_cast<int>(graphSignals_.size()));
    for (std::size_t i = 0; i < graphSignals_.size(); ++i) {
        settings.setValue(QStringLiteral("graph%1").arg(i + 1),
                          graphSignals_[i]);
    }
    settings.endGroup();
    // Pointer for the renamed-copy borrow in TryRestoreLayout.
    settings.setValue(LayoutStemHashKey(layoutChannelHash_), layoutStem_);
}

void SimTelemetryView::ApplyAssignments() {
    for (std::size_t i = 0; i < plots_.size(); ++i) {
        plots_[i]->SetSignals(i < graphSignals_.size()
                                  ? graphSignals_[i]
                                  : QStringList{});
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
    QString firstDataChannel;
    for (const QString& name : session_->ChannelNames()) {
        if (session_->ChannelStats(name).finiteSamples == 0) {
            ++idleChannels;
        } else if (firstDataChannel.isEmpty()) {
            firstDataChannel = name;
        }
    }
    // Retained-span readout, from the store the plots actually read: a
    // recorded session must cover t 0..duration, not just the rolling tail.
    QString retained = QStringLiteral("(no data channels)");
    if (!firstDataChannel.isEmpty()) {
        std::vector<float> t;
        std::vector<float> y;
        if (store_.CopyHistoryInto(firstDataChannel.toStdString(), t, y)
            && !t.empty()) {
            retained = QStringLiteral("t %1..%2 s (%3 samples of %4)")
                           .arg(static_cast<double>(t.front()), 0, 'f', 3)
                           .arg(static_cast<double>(t.back()), 0, 'f', 3)
                           .arg(t.size())
                           .arg(firstDataChannel);
        }
    }
    // Window bounds the plots show right now (anchored at the newest sample,
    // like the leaf widget computes them).
    const double windowSeconds = viewSlider_ ? viewSlider_->value() / 10.0 : 10.0;
    const double windowEnd = meta.durationS;
    const double windowStart = windowEnd - windowSeconds;
    statusLabel_->setText(QStringLiteral(
        "%1 frames · %2 channels (%3 all-null) · %4 s · retained %5 · window %6..%7 s · plant: %8 · %9")
            .arg(meta.telemetryFrames)
            .arg(session_->ChannelNames().size())
            .arg(idleChannels)
            .arg(meta.durationS, 0, 'f', 3)
            .arg(retained,
                 QString::number(windowStart, 'f', 1),
                 QString::number(windowEnd, 'f', 1),
                 meta.plant.isEmpty() ? QStringLiteral("(no stamp)")
                                      : meta.plant,
                 sessionPath_));
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
    const bool sameNames = current == names;
    if (sameNames) {
        // Just re-sync the assignment dropdowns (graph count changes and
        // load-time pruning land here without a full rebuild).
        rebuildingTable_ = true;
        for (int row = 0; row < signalTable_->rowCount(); ++row) {
            auto* combo = qobject_cast<QComboBox*>(
                signalTable_->cellWidget(row, kTargetColumn));
            if (!combo) {
                continue;
            }
            const QString name = signalTable_->item(row, kSignalColumn)->text();
            combo->clear();
            combo->addItem(QStringLiteral("\u2014"));
            for (int i = 0; i < static_cast<int>(graphSignals_.size()); ++i) {
                combo->addItem(QStringLiteral("G%1").arg(i + 1));
            }
            combo->setCurrentIndex(AssignedGraph(name) + 1);
        }
        rebuildingTable_ = false;
        return;
    }

    rebuildingTable_ = true;
    signalTable_->setRowCount(0);
    signalTable_->setRowCount(static_cast<int>(names.size()));
    for (int row = 0; row < names.size(); ++row) {
        const QString& name = names[row];
        const SimChannelStats stats = session_->ChannelStats(name);
        const bool dead = stats.finiteSamples == 0;

        auto* combo = new QComboBox(signalTable_);
        combo->setObjectName(QStringLiteral("target.%1").arg(name));
        combo->addItem(QStringLiteral("\u2014"));
        for (int i = 0; i < static_cast<int>(graphSignals_.size()); ++i) {
            combo->addItem(QStringLiteral("G%1").arg(i + 1));
        }
        combo->setCurrentIndex(AssignedGraph(name) + 1);
        combo->setEnabled(!dead);
        if (dead) {
            combo->setToolTip(QStringLiteral(
                "Every sample in this channel is null — nothing to plot."));
        }
        signalTable_->setCellWidget(row, kTargetColumn, combo);
        connect(combo, &QComboBox::currentIndexChanged, this,
                [this, row](int index) {
                    if (rebuildingTable_) {
                        return;
                    }
                    const QString key =
                        signalTable_->item(row, kSignalColumn)->text();
                    for (QStringList& set : graphSignals_) {
                        set.removeAll(key);
                    }
                    if (index > 0 && index <= static_cast<int>(graphSignals_.size())) {
                        graphSignals_[index - 1].push_back(key);
                    }
                    ApplyAssignments();
                    ScheduleLayoutSave();
                });

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
