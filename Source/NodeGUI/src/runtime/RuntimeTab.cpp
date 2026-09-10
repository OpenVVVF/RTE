#include "RuntimeTab.h"

#include "ConsolePanel.h"
#include "FramKeysManager.h"
#include "RuntimeController.h"
#include "RuntimeSessionExporter.h"
#include "SignalTablePanel.h"
#include "TelemetryPanel.h"

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace NodeGUI::runtime {

namespace {

constexpr int kMaxRecentPresets = 10;

QSettings MakeSettings() {
    return QSettings(QStringLiteral("RTE"), QStringLiteral("RTEStudio"));
}

bool LayoutIsEmpty(const std::array<QStringList, 3>& sets) {
    for (const QStringList& list : sets) {
        if (!list.isEmpty()) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::array<QStringList, 3> RuntimeTab::BuiltinSpwmLayout() {
    return {{
        QStringList{QStringLiteral("pwm_gate_u"),
                    QStringLiteral("pwm_gate_v"),
                    QStringLiteral("pwm_gate_w")},
        QStringList{QStringLiteral("duty_u"),
                    QStringLiteral("duty_v"),
                    QStringLiteral("duty_w")},
        QStringList{QStringLiteral("i_a"), QStringLiteral("i_b"), QStringLiteral("i_c")},
    }};
}

std::array<QStringList, 3> RuntimeTab::BuiltinFocLayout() {
    return {{
        QStringList{QStringLiteral("cg_id_a"), QStringLiteral("cg_iq_a")},
        QStringList{QStringLiteral("cg_vd_v"), QStringLiteral("cg_vq_v")},
        QStringList{QStringLiteral("cg_iu_a"),
                    QStringLiteral("cg_iv_a"),
                    QStringLiteral("cg_iw_a")},
    }};
}

void RuntimeTab::EnsureBuiltinPresets() {
    auto settings = MakeSettings();
    const bool hasSpwm = settings.contains(QStringLiteral("runtime/presets/SPWM"));
    const bool hasFoc = settings.contains(QStringLiteral("runtime/presets/FOC"));
    if (hasSpwm && hasFoc) {
        return;
    }

    if (!hasSpwm) {
        const auto layout = BuiltinSpwmLayout();
        settings.beginGroup(QStringLiteral("runtime/presets/SPWM"));
        for (int i = 0; i < 3; ++i) {
            settings.setValue(QStringLiteral("graph%1").arg(i + 1), layout[i]);
        }
        settings.endGroup();
    }

    if (!hasFoc) {
        const auto layout = BuiltinFocLayout();
        settings.beginGroup(QStringLiteral("runtime/presets/FOC"));
        for (int i = 0; i < 3; ++i) {
            settings.setValue(QStringLiteral("graph%1").arg(i + 1), layout[i]);
        }
        settings.endGroup();
    }

    QStringList recent = settings.value(QStringLiteral("runtime/recent")).toStringList();
    if (!hasSpwm && !recent.contains(QStringLiteral("SPWM"))) {
        recent.prepend(QStringLiteral("SPWM"));
    }
    if (!hasFoc && !recent.contains(QStringLiteral("FOC"))) {
        recent.prepend(QStringLiteral("FOC"));
    }
    while (recent.size() > kMaxRecentPresets) {
        recent.removeLast();
    }
    settings.setValue(QStringLiteral("runtime/recent"), recent);
}

void RuntimeTab::ApplyLayoutIfEmpty(const std::array<QStringList, 3>& layout) {
    if (!LayoutIsEmpty(signalTablePanel_->GraphSignalSets())) {
        return;
    }
    signalTablePanel_->SetGraphSignalSets(layout);
    if (layout == BuiltinFocLayout()) {
        ApplyFocViewWindows();
        presetStatus_->setText(QStringLiteral("applied FOC plot layout"));
    } else {
        ApplySpwmViewWindows();
        presetStatus_->setText(QStringLiteral("applied SPWM plot layout"));
    }
}

void RuntimeTab::ApplySpwmViewWindows() {
    // G1 scope (~5 carrier periods @ 100 Hz), G2 duty slow, G3 current.
    // The slider path only syncs the shared-window control; its
    // viewSecondsChanged emission must not fire here, or it would stomp the
    // per-plot windows with one uniform window.
    const QSignalBlocker blockSignals(signalTablePanel_);
    signalTablePanel_->SetViewSeconds(1.0);
    telemetryPanel_->SetGraphViewSeconds({0.05, 1.0, 0.5});
}

void RuntimeTab::ApplyFocViewWindows() {
    // G1 d/q current, G2 d/q voltage, G3 phase currents.
    const QSignalBlocker blockSignals(signalTablePanel_);
    signalTablePanel_->SetViewSeconds(0.5);
    telemetryPanel_->SetGraphViewSeconds({0.5, 0.5, 0.5});
}

RuntimeTab::RuntimeTab(RuntimeController* controller, QWidget* parent)
    : QWidget(parent)
    , controller_(controller) {
    framKeysManager_ = new FramKeysManager(controller_, this);

    auto* layout = new QVBoxLayout(this);

    // Link status header (same fields as the old app's header line), plus a
    // full-session export that is independent of the rolling plot buffers.
    auto* headerRow = new QHBoxLayout;
    headerLabel_ = new QLabel(this);
    headerRow->addWidget(headerLabel_, 1);
    exportStatus_ = new QLabel(this);
    headerRow->addWidget(exportStatus_);
    auto* clearSessionButton =
        new QPushButton(QStringLiteral("Clear Session"), this);
    clearSessionButton->setToolTip(
        QStringLiteral("Discard all telemetry, console output, and commands recorded in this session"));
    connect(clearSessionButton,
            &QPushButton::clicked,
            this,
            &RuntimeTab::OnClearSession);
    headerRow->addWidget(clearSessionButton);
    auto* exportButton =
        new QPushButton(QStringLiteral("Export Session\u2026"), this);
    exportButton->setToolTip(
        QStringLiteral("Save all telemetry, console output, and commands from this runtime session"));
    connect(exportButton,
            &QPushButton::clicked,
            this,
            &RuntimeTab::OnExportSession);
    headerRow->addWidget(exportButton);
    layout->addLayout(headerRow);

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
    auto* spwmButton = new QPushButton(QStringLiteral("SPWM"), this);
    spwmButton->setToolTip(QStringLiteral("Apply the SPWM demo plot layout"));
    connect(spwmButton, &QPushButton::clicked, this, &RuntimeTab::OnLoadBuiltinSpwm);
    presetRow->addWidget(spwmButton);
    auto* focButton = new QPushButton(QStringLiteral("FOC"), this);
    focButton->setToolTip(QStringLiteral("Apply the FOC plot layout"));
    connect(focButton, &QPushButton::clicked, this, &RuntimeTab::OnLoadBuiltinFoc);
    presetRow->addWidget(focButton);
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

    EnsureBuiltinPresets();
    RefreshRecentCombo();
    OnStoreChanged();
}

void RuntimeTab::OnStoreChanged() {
    if (!applied_builtin_layout_) {
        // FOC graphs publish d/q currents; prefer the FOC layout when those
        // are present. SPWM demo graphs do not, so duty_u triggers the SPWM
        // layout. Only auto-applies while the layout is still untouched.
        float probe = 0.0f;
        if (controller_->Store().LatestValue("cg_id_a", probe)) {
            ApplyLayoutIfEmpty(BuiltinFocLayout());
            applied_builtin_layout_ = true;
        } else if (controller_->Store().LatestValue("duty_u", probe)) {
            ApplyLayoutIfEmpty(BuiltinSpwmLayout());
            applied_builtin_layout_ = true;
        }
    }

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

void RuntimeTab::OnLoadBuiltinSpwm() {
    signalTablePanel_->SetGraphSignalSets(BuiltinSpwmLayout());
    ApplySpwmViewWindows();
    recentCombo_->setCurrentText(QStringLiteral("SPWM"));
    presetStatus_->setText(QStringLiteral("loaded SPWM layout"));
    applied_builtin_layout_ = true;
}

void RuntimeTab::OnLoadBuiltinFoc() {
    signalTablePanel_->SetGraphSignalSets(BuiltinFocLayout());
    ApplyFocViewWindows();
    recentCombo_->setCurrentText(QStringLiteral("FOC"));
    presetStatus_->setText(QStringLiteral("loaded FOC layout"));
    applied_builtin_layout_ = true;
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

void RuntimeTab::OnExportSession() {
    const QString timestamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString suggestedPath =
        QDir::home().filePath(
            QStringLiteral("runtime-session-%1.jsonl").arg(timestamp));
    QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Export Runtime Session"),
        suggestedPath,
        QStringLiteral("RTE Runtime Session (*.jsonl);;All Files (*)"));
    if (path.isEmpty()) {
        return;
    }
    if (QFileInfo(path).suffix().isEmpty()) {
        path += QStringLiteral(".jsonl");
    }

    RuntimeSessionMetadata metadata;
    metadata.port = controller_->Port();
    metadata.mode = controller_->IsSimulating()
                        ? QStringLiteral("simulation")
                        : QStringLiteral("device");
    metadata.protocol =
        controller_->GetProtocol() == Protocol::Legacy
            ? QStringLiteral("legacy")
            : QStringLiteral("inverter");

    exportStatus_->setText(QStringLiteral("exporting\u2026"));
    const RuntimeSessionSnapshot session = controller_->CaptureSession();
    QString error;
    if (!ExportRuntimeSession(path, session, metadata, error)) {
        exportStatus_->setText(QStringLiteral("export failed"));
        QMessageBox::critical(
            this,
            QStringLiteral("Export Runtime Session"),
            QStringLiteral("Could not export the runtime session:\n%1")
                .arg(error));
        return;
    }

    exportStatus_->setText(
        QStringLiteral("exported %1").arg(QFileInfo(path).fileName()));
}

void RuntimeTab::OnClearSession() {
    QMessageBox confirmation(this);
    confirmation.setIcon(QMessageBox::Warning);
    confirmation.setWindowTitle(QStringLiteral("Clear Runtime Session"));
    confirmation.setText(
        QStringLiteral(
            "Clear all recorded telemetry, console output, and command "
            "history?\n\nThis cannot be undone."));
    auto* clearButton = confirmation.addButton(
        QStringLiteral("Clear Session"), QMessageBox::DestructiveRole);
    confirmation.addButton(QMessageBox::Cancel);
    confirmation.setDefaultButton(QMessageBox::Cancel);
    confirmation.exec();
    if (confirmation.clickedButton() != clearButton) {
        return;
    }

    controller_->ClearSession();
    exportStatus_->setText(QStringLiteral("session cleared"));
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

void RuntimeTab::OnSaveFramKeys() {
    const QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Save FRAM Keys"),
        QDir::home().filePath(QStringLiteral("fram-keys.json")),
        QStringLiteral("JSON (*.json);;All Files (*)"));
    if (path.isEmpty()) {
        return;
    }

    QString error;
    if (!framKeysManager_->SaveToFile(path, error)) {
        QMessageBox::critical(
            this,
            QStringLiteral("Save FRAM Keys"),
            QStringLiteral("Could not save FRAM keys:\n%1").arg(error));
        return;
    }
    exportStatus_->setText(QStringLiteral("saved FRAM keys to %1")
                               .arg(QFileInfo(path).fileName()));
}

void RuntimeTab::OnLoadFramKeys() {
    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Load FRAM Keys"),
        QDir::homePath(),
        QStringLiteral("JSON (*.json);;All Files (*)"));
    if (path.isEmpty()) {
        return;
    }

    const bool clearFirst =
        QMessageBox::question(
            this,
            QStringLiteral("Load FRAM Keys"),
            QStringLiteral("Delete all existing FRAM keys before loading?\n\n"
                           "Choose No to merge the file with existing keys."),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) == QMessageBox::Yes;

    QString error;
    const bool ok = framKeysManager_->LoadFromFile(path, clearFirst, error);
    if (!ok) {
        QMessageBox::critical(
            this,
            QStringLiteral("Load FRAM Keys"),
            QStringLiteral("Could not load FRAM keys:\n%1").arg(error));
        return;
    }
    exportStatus_->setText(error);
}

}  // namespace NodeGUI::runtime
