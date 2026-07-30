#include "RuntimeTab.h"

#include "ConsolePanel.h"
#include "RuntimeController.h"
#include "SignalTablePanel.h"
#include "TelemetryPanel.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

namespace NodeGUI::runtime {

namespace {

constexpr int kMaxRecentPresets = 10;

QSettings MakeSettings() {
    return QSettings(QStringLiteral("RTE"), QStringLiteral("NodeGUI"));
}

}  // namespace

RuntimeTab::RuntimeTab(RuntimeController* controller, QWidget* parent)
    : QWidget(parent)
    , controller_(controller) {
    auto* layout = new QVBoxLayout(this);

    // Link status header (same fields as the old app's header line).
    headerLabel_ = new QLabel(this);
    layout->addWidget(headerLabel_);

    // Graph-layout presets.
    auto* presetRow = new QHBoxLayout;
    presetNameEdit_ = new QLineEdit(this);
    presetNameEdit_->setPlaceholderText(QStringLiteral("Layout name"));
    presetRow->addWidget(presetNameEdit_, 1);
    auto* saveButton = new QPushButton(QStringLiteral("Save"), this);
    connect(saveButton, &QPushButton::clicked, this, &RuntimeTab::OnSavePreset);
    presetRow->addWidget(saveButton);
    recentCombo_ = new QComboBox(this);
    presetRow->addWidget(recentCombo_, 1);
    auto* loadButton = new QPushButton(QStringLiteral("Load"), this);
    connect(loadButton, &QPushButton::clicked, this, &RuntimeTab::OnLoadPreset);
    presetRow->addWidget(loadButton);
    presetStatus_ = new QLabel(this);
    presetRow->addWidget(presetStatus_);
    presetRow->addStretch(1);
    layout->addLayout(presetRow);

    // The plots are the central content; the signal table and console are
    // dockable panels hosted by the main window (created here, fetched via
    // GetSignalTable()/GetConsole()).
    signalTablePanel_ = new SignalTablePanel(controller_);
    consolePanel_ = new ConsolePanel(controller_);

    telemetryPanel_ = new TelemetryPanel(controller_, this);
    layout->addWidget(telemetryPanel_, 1);

    connect(signalTablePanel_, &SignalTablePanel::graphSignalsChanged,
            telemetryPanel_, &TelemetryPanel::SetGraphSignals);
    connect(signalTablePanel_, &SignalTablePanel::viewSecondsChanged,
            telemetryPanel_, &TelemetryPanel::SetViewSeconds);

    connect(controller_, &RuntimeController::storeChanged,
            this, &RuntimeTab::OnStoreChanged);

    RefreshRecentCombo();
    OnStoreChanged();
}

void RuntimeTab::OnStoreChanged() {
    // Cheap scalar read — the full Snapshot() copies every history and is far
    // too expensive for the ~30 Hz header refresh.
    const auto stats = controller_->Store().GetStatsLine();

    // Same bandwidth estimate as the old app: fraction of the 460800 8N1 link.
    const double bandwidthPct =
        static_cast<double>(stats.rxBytesPerSec) * 10.0 / 460800.0 * 100.0;

    headerLabel_->setText(
        QStringLiteral("Port: %1 | RX: %2 Hz | Bandwidth: %3% | Seq: %4 | "
                       "Good: %5 | Bad: %6 | Reject: crc %7 / hdr %8 / len %9 / "
                       "parse %10 / unknown_id %11")
            .arg(controller_->Port())
            .arg(stats.rxHz, 0, 'f', 1)
            .arg(bandwidthPct, 0, 'f', 1)
            .arg(stats.lastSeq)
            .arg(stats.goodFrames)
            .arg(stats.badFrames)
            .arg(stats.rejectCrc)
            .arg(stats.rejectHdr)
            .arg(stats.rejectLen)
            .arg(stats.rejectPayloadParse)
            .arg(stats.rejectUnknownId));
}

void RuntimeTab::OnSavePreset() {
    QString name = presetNameEdit_->text().trimmed();
    if (name.isEmpty()) {
        presetStatus_->setText(QStringLiteral("name required"));
        return;
    }
    name.replace('/', '_');

    auto settings = MakeSettings();
    const auto sets = signalTablePanel_->GraphSignalSets();
    settings.beginGroup(QStringLiteral("runtime/presets/") + name);
    for (int i = 0; i < 3; ++i) {
        settings.setValue(QStringLiteral("graph%1").arg(i + 1), sets[i]);
    }
    settings.endGroup();

    // MRU list, most recent first, deduped, capped.
    QStringList recent = settings.value(QStringLiteral("runtime/recent")).toStringList();
    recent.removeAll(name);
    recent.prepend(name);
    while (recent.size() > kMaxRecentPresets) {
        recent.removeLast();
    }
    settings.setValue(QStringLiteral("runtime/recent"), recent);

    presetStatus_->setText(QStringLiteral("saved '%1'").arg(name));
    RefreshRecentCombo();
    recentCombo_->setCurrentText(name);
}

void RuntimeTab::OnLoadPreset() {
    const QString name = recentCombo_->currentText();
    if (name.isEmpty()) {
        presetStatus_->setText(QStringLiteral("no preset selected"));
        return;
    }

    auto settings = MakeSettings();
    settings.beginGroup(QStringLiteral("runtime/presets/") + name);
    std::array<QStringList, 3> sets;
    for (int i = 0; i < 3; ++i) {
        sets[i] = settings.value(QStringLiteral("graph%1").arg(i + 1)).toStringList();
    }
    settings.endGroup();

    signalTablePanel_->SetGraphSignalSets(sets);
    presetStatus_->setText(QStringLiteral("loaded '%1'").arg(name));
}

void RuntimeTab::RefreshRecentCombo() {
    auto settings = MakeSettings();
    const QStringList recent =
        settings.value(QStringLiteral("runtime/recent")).toStringList();
    recentCombo_->clear();
    recentCombo_->addItem(QStringLiteral("load recent..."));
    recentCombo_->addItems(recent);
}

void RuntimeTab::LoadAutosave() {
    auto settings = MakeSettings();
    settings.beginGroup(QStringLiteral("runtime/autosave"));
    std::array<QStringList, 3> sets;
    for (int i = 0; i < 3; ++i) {
        sets[i] = settings.value(QStringLiteral("graph%1").arg(i + 1)).toStringList();
    }
    settings.endGroup();
    signalTablePanel_->SetGraphSignalSets(sets);
}

void RuntimeTab::SaveAutosave() {
    auto settings = MakeSettings();
    const auto sets = signalTablePanel_->GraphSignalSets();
    settings.beginGroup(QStringLiteral("runtime/autosave"));
    for (int i = 0; i < 3; ++i) {
        settings.setValue(QStringLiteral("graph%1").arg(i + 1), sets[i]);
    }
    settings.endGroup();
}

}  // namespace NodeGUI::runtime
