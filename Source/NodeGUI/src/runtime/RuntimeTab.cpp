#include "RuntimeTab.h"

#include "ConsolePanel.h"
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

    // Server connection row: Local spawns an RTEServer child process, Remote
    // connects to one over IP. Everything runtime (telemetry, console, flash)
    // goes through that one server.
    auto* serverRow = new QHBoxLayout;
    serverRow->addWidget(new QLabel(QStringLiteral("Server:"), this));
    serverModeCombo_ = new QComboBox(this);
    serverModeCombo_->addItem(QStringLiteral("Local (spawn RTEServer)"));
    serverModeCombo_->addItem(QStringLiteral("Remote (IP)"));
    serverRow->addWidget(serverModeCombo_);
    serverHostEdit_ = new QLineEdit(this);
    serverHostEdit_->setPlaceholderText(QStringLiteral("192.168.1.x"));
    serverHostEdit_->setMaximumWidth(220);
    serverRow->addWidget(serverHostEdit_);
    auto* connectButton = new QPushButton(QStringLiteral("Connect"), this);
    connect(connectButton, &QPushButton::clicked, this, &RuntimeTab::OnConnectClicked);
    serverRow->addWidget(connectButton);
    serverStatusLabel_ = new QLabel(this);
    serverRow->addWidget(serverStatusLabel_);
    serverRow->addStretch(1);
    layout->addLayout(serverRow);

    auto updateHostEnabled = [this] {
        serverHostEdit_->setEnabled(serverModeCombo_->currentIndex() == 1);
    };
    connect(serverModeCombo_, &QComboBox::activated, this, updateHostEnabled);
    updateHostEnabled();

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

    const QString endpoint = controller_->GetProtocol() == Protocol::Inverter
                                 ? controller_->Port()
                                 : QStringLiteral("%1:%2")
                                       .arg(controller_->ServerHost())
                                       .arg(controller_->BridgePort());
    headerLabel_->setText(
        QStringLiteral("Server: %1 | RX: %2 Hz | Bandwidth: %3% | Seq: %4 | "
                       "Good: %5 | Bad: %6 | Reject: crc %7 / hdr %8 / len %9 / "
                       "parse %10 / unknown_id %11")
            .arg(endpoint)
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

void RuntimeTab::OnLoadPreset() {    const QString name = recentCombo_->currentText();
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

void RuntimeTab::SetConnectionState(bool remote, const QString& host) {
    serverModeCombo_->setCurrentIndex(remote ? 1 : 0);
    serverHostEdit_->setEnabled(remote);
    serverHostEdit_->setText(host);
}

void RuntimeTab::SetServerStatus(const QString& text) {
    serverStatusLabel_->setText(text);
}

void RuntimeTab::OnConnectClicked() {
    const bool remote = serverModeCombo_->currentIndex() == 1;
    const QString host = remote ? serverHostEdit_->text().trimmed() : QString();
    if (remote && host.isEmpty()) {
        serverStatusLabel_->setText(QStringLiteral("enter an IP or hostname"));
        return;
    }
    emit connectRequested(remote, host);
}

}  // namespace NodeGUI::runtime
