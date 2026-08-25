#include "PreferencesDialog.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFormLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

namespace NodeGUI {

namespace {

void MigrateLegacySettings() {
    QSettings settings(QStringLiteral("RTE"), QStringLiteral("RTEStudio"));
    if (settings.allKeys().isEmpty()) {
        QSettings legacy(QStringLiteral("RTE"), QStringLiteral("NodeGUI"));
        for (const QString& key : legacy.allKeys()) settings.setValue(key, legacy.value(key));
    }
}

QSettings MakeSettings() {
    MigrateLegacySettings();
    return QSettings(QStringLiteral("RTE"), QStringLiteral("RTEStudio"));
}

constexpr int kMinimumHistoryLimit = 10;
constexpr int kMaximumHistoryLimit = 1000;
constexpr int kMinimumLogLines = 100;
constexpr int kMaximumLogLines = 100000;

bool IsSupportedPanButton(Qt::MouseButton button) {
    return button == Qt::LeftButton || button == Qt::MiddleButton;
}

}  // namespace

AppPreferences LoadAppPreferences() {
    QSettings settings = MakeSettings();
    AppPreferences preferences;
    settings.beginGroup(QStringLiteral("preferences"));
    preferences.confirmNewGraph =
        settings.value(QStringLiteral("confirmNewGraph"), true).toBool();
    preferences.automaticallyShowBuildLogs =
        settings.value(QStringLiteral("automaticallyShowBuildLogs"), true).toBool();
    preferences.rememberWindowGeometry =
        settings.value(QStringLiteral("rememberWindowGeometry"), true).toBool();
    preferences.allowExternalDeviceWrites =
        settings.value(QStringLiteral("allowExternalDeviceWrites"), false).toBool();
    preferences.undoHistoryLimit =
        std::clamp(settings.value(QStringLiteral("undoHistoryLimit"), 100).toInt(),
                   kMinimumHistoryLimit,
                   kMaximumHistoryLimit);
    preferences.buildLogLineLimit =
        std::clamp(settings.value(QStringLiteral("buildLogLineLimit"), 5000).toInt(),
                   kMinimumLogLines,
                   kMaximumLogLines);
    preferences.firmwareBuildType =
        settings.value(QStringLiteral("firmwareBuildType"),
                       QStringLiteral("Release")).toString();
    preferences.serialPort =
        settings.value(QStringLiteral("serialPort"), preferences.serialPort).toString();
    preferences.panMouseButton = static_cast<Qt::MouseButton>(
        settings.value(QStringLiteral("panMouseButton"),
                       static_cast<int>(Qt::MiddleButton)).toInt());
    settings.endGroup();

    const QStringList supportedBuildTypes = {
        QStringLiteral("Debug"),
        QStringLiteral("Release"),
        QStringLiteral("RelWithDebInfo"),
        QStringLiteral("MinSizeRel"),
    };
    if (!supportedBuildTypes.contains(preferences.firmwareBuildType)) {
        preferences.firmwareBuildType = QStringLiteral("Release");
    }
    if (!IsSupportedPanButton(preferences.panMouseButton)) {
        preferences.panMouseButton = Qt::MiddleButton;
    }
    return preferences;
}

void SaveAppPreferences(const AppPreferences& preferences) {
    QSettings settings = MakeSettings();
    settings.beginGroup(QStringLiteral("preferences"));
    settings.setValue(QStringLiteral("confirmNewGraph"), preferences.confirmNewGraph);
    settings.setValue(QStringLiteral("automaticallyShowBuildLogs"),
                      preferences.automaticallyShowBuildLogs);
    settings.setValue(QStringLiteral("rememberWindowGeometry"),
                      preferences.rememberWindowGeometry);
    settings.setValue(QStringLiteral("allowExternalDeviceWrites"),
                      preferences.allowExternalDeviceWrites);
    settings.setValue(QStringLiteral("undoHistoryLimit"), preferences.undoHistoryLimit);
    settings.setValue(QStringLiteral("buildLogLineLimit"),
                      preferences.buildLogLineLimit);
    settings.setValue(QStringLiteral("firmwareBuildType"),
                      preferences.firmwareBuildType);
    settings.setValue(QStringLiteral("serialPort"), preferences.serialPort);
    settings.setValue(QStringLiteral("panMouseButton"),
                      static_cast<int>(preferences.panMouseButton));
    settings.endGroup();
}

QKeySequence LoadShortcutPreference(const QString& id,
                                    const QKeySequence& defaultSequence) {
    QSettings settings = MakeSettings();
    const QString key = QStringLiteral("shortcuts/") + id;
    if (!settings.contains(key)) {
        return defaultSequence;
    }
    return QKeySequence::fromString(settings.value(key).toString(),
                                    QKeySequence::PortableText);
}

void SaveShortcutPreferences(const QMap<QString, QKeySequence>& shortcuts) {
    QSettings settings = MakeSettings();
    settings.beginGroup(QStringLiteral("shortcuts"));
    for (auto it = shortcuts.cbegin(); it != shortcuts.cend(); ++it) {
        settings.setValue(it.key(), it.value().toString(QKeySequence::PortableText));
    }
    settings.endGroup();
}

PreferencesDialog::PreferencesDialog(const AppPreferences& preferences,
                                     const QVector<ShortcutBinding>& bindings,
                                     QWidget* parent)
    : QDialog(parent)
    , bindings_(bindings) {
    setWindowTitle(QStringLiteral("Preferences"));
    setMinimumSize(720, 520);

    auto* outerLayout = new QVBoxLayout(this);
    auto* tabs = new QTabWidget(this);
    outerLayout->addWidget(tabs, 1);

    auto* generalPage = new QWidget(tabs);
    auto* generalLayout = new QFormLayout(generalPage);
    confirmNewGraphCheck_ =
        new QCheckBox(QStringLiteral("Ask before discarding the current graph"),
                      generalPage);
    confirmNewGraphCheck_->setChecked(preferences.confirmNewGraph);
    generalLayout->addRow(QStringLiteral("New graph"), confirmNewGraphCheck_);

    rememberWindowGeometryCheck_ =
        new QCheckBox(QStringLiteral("Restore the previous window size and position"),
                      generalPage);
    rememberWindowGeometryCheck_->setChecked(preferences.rememberWindowGeometry);
    generalLayout->addRow(QStringLiteral("Window"), rememberWindowGeometryCheck_);

    undoHistoryLimitSpin_ = new QSpinBox(generalPage);
    undoHistoryLimitSpin_->setRange(kMinimumHistoryLimit, kMaximumHistoryLimit);
    undoHistoryLimitSpin_->setValue(preferences.undoHistoryLimit);
    undoHistoryLimitSpin_->setSuffix(QStringLiteral(" changes"));
    undoHistoryLimitSpin_->setToolTip(
        QStringLiteral("Maximum number of graph changes retained for Undo"));
    generalLayout->addRow(QStringLiteral("Undo history"), undoHistoryLimitSpin_);
    tabs->addTab(generalPage, QStringLiteral("General"));

    auto* devicePage = new QWidget(tabs);
    auto* deviceLayout = new QFormLayout(devicePage);
    serialPortCombo_ = new QComboBox(devicePage);
    serialPortCombo_->setEditable(true);
    serialPortCombo_->setInsertPolicy(QComboBox::NoInsert);
#ifdef _WIN32
    for (int port = 1; port <= 32; ++port) {
        serialPortCombo_->addItem(QStringLiteral("COM%1").arg(port));
    }
#else
    QStringList detectedPorts;
    const QDir byId(QStringLiteral("/dev/serial/by-id"));
    for (const QString& entry : byId.entryList(QDir::Files | QDir::System)) {
        detectedPorts.push_back(byId.absoluteFilePath(entry));
    }
    const QDir dev(QStringLiteral("/dev"));
    for (const QString& pattern : {QStringLiteral("ttyACM*"),
                                   QStringLiteral("ttyUSB*")}) {
        for (const QString& entry : dev.entryList({pattern}, QDir::System)) {
            detectedPorts.push_back(dev.absoluteFilePath(entry));
        }
    }
    detectedPorts.removeDuplicates();
    detectedPorts.sort();
    serialPortCombo_->addItems(detectedPorts);
#endif
    serialPortCombo_->setCurrentText(preferences.serialPort);
    serialPortCombo_->setToolTip(
        QStringLiteral("Serial port used for telemetry, commands, and firmware flashing. "
                       "Stable /dev/serial/by-id paths are recommended on Linux."));
    deviceLayout->addRow(QStringLiteral("Device port"), serialPortCombo_);
    tabs->addTab(devicePage, QStringLiteral("Device"));

    auto* buildPage = new QWidget(tabs);
    auto* buildLayout = new QFormLayout(buildPage);
    buildTypeCombo_ = new QComboBox(buildPage);
    buildTypeCombo_->addItems({
        QStringLiteral("Release"),
        QStringLiteral("RelWithDebInfo"),
        QStringLiteral("MinSizeRel"),
        QStringLiteral("Debug"),
    });
    buildTypeCombo_->setCurrentText(preferences.firmwareBuildType);
    buildTypeCombo_->setToolTip(
        QStringLiteral("Debug builds can be too slow for the full control ISR load"));
    buildLayout->addRow(QStringLiteral("Firmware build type"), buildTypeCombo_);

    showBuildLogsCheck_ =
        new QCheckBox(QStringLiteral("Open the detachable Logs tab when an operation starts"),
                      buildPage);
    showBuildLogsCheck_->setChecked(preferences.automaticallyShowBuildLogs);
    buildLayout->addRow(QStringLiteral("Build logs"), showBuildLogsCheck_);

    buildLogLineLimitSpin_ = new QSpinBox(buildPage);
    buildLogLineLimitSpin_->setRange(kMinimumLogLines, kMaximumLogLines);
    buildLogLineLimitSpin_->setSingleStep(500);
    buildLogLineLimitSpin_->setValue(preferences.buildLogLineLimit);
    buildLogLineLimitSpin_->setSuffix(QStringLiteral(" lines"));
    buildLayout->addRow(QStringLiteral("Log retention"), buildLogLineLimitSpin_);
    tabs->addTab(buildPage, QStringLiteral("Build"));

    auto* automationPage = new QWidget(tabs);
    auto* automationLayout = new QVBoxLayout(automationPage);
    allowExternalDeviceWritesCheck_ = new QCheckBox(
        QStringLiteral("Allow CLI and MCP clients to send commands to the device"),
        automationPage);
    allowExternalDeviceWritesCheck_->setChecked(preferences.allowExternalDeviceWrites);
    automationLayout->addWidget(allowExternalDeviceWritesCheck_);
    auto* automationHint = new QLabel(
        QStringLiteral("Read-only status, telemetry, and console access stays available. "
                       "This write permission is disabled by default and only applies while "
                       "RTE Studio is running."), automationPage);
    automationHint->setWordWrap(true);
    automationLayout->addWidget(automationHint);
    automationLayout->addStretch(1);
    tabs->addTab(automationPage, QStringLiteral("Automation"));

    auto* shortcutsPage = new QWidget(tabs);
    auto* shortcutsLayout = new QVBoxLayout(shortcutsPage);

    auto* mouseBindingsLayout = new QFormLayout;
    panMouseButtonCombo_ = new QComboBox(shortcutsPage);
    panMouseButtonCombo_->addItem(QStringLiteral("Middle Mouse"),
                                  static_cast<int>(Qt::MiddleButton));
    panMouseButtonCombo_->addItem(QStringLiteral("Left Mouse"),
                                  static_cast<int>(Qt::LeftButton));
    const int panButtonIndex =
        panMouseButtonCombo_->findData(static_cast<int>(preferences.panMouseButton));
    panMouseButtonCombo_->setCurrentIndex(std::max(0, panButtonIndex));
    panMouseButtonCombo_->setToolTip(
        QStringLiteral("Drag with this button to pan the node canvas"));
    mouseBindingsLayout->addRow(QStringLiteral("Pan canvas"), panMouseButtonCombo_);
    shortcutsLayout->addLayout(mouseBindingsLayout);

    auto* shortcutHint = new QLabel(
        QStringLiteral("Select a shortcut field and press the desired key combination. "
                       "Clear the field to leave an action unbound."),
        shortcutsPage);
    shortcutHint->setWordWrap(true);
    shortcutsLayout->addWidget(shortcutHint);

    auto* table = new QTableWidget(bindings_.size(), 4, shortcutsPage);
    table->setHorizontalHeaderLabels({
        QStringLiteral("Category"),
        QStringLiteral("Action"),
        QStringLiteral("Shortcut"),
        QStringLiteral("Default"),
    });
    table->verticalHeader()->hide();
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);

    for (int row = 0; row < bindings_.size(); ++row) {
        const ShortcutBinding& binding = bindings_.at(row);
        table->setItem(row, 0, new QTableWidgetItem(binding.category));
        table->setItem(row, 1, new QTableWidgetItem(binding.label));

        auto* editor = new QKeySequenceEdit(
            binding.action ? binding.action->shortcut() : QKeySequence{},
            table);
        table->setCellWidget(row, 2, editor);
        shortcutEditors_.insert(binding.id, editor);

        auto* defaultItem = new QTableWidgetItem(
            binding.defaultSequence.toString(QKeySequence::NativeText));
        defaultItem->setTextAlignment(Qt::AlignCenter);
        table->setItem(row, 3, defaultItem);
    }
    shortcutsLayout->addWidget(table, 1);

    auto* resetShortcuts =
        new QPushButton(QStringLiteral("Restore Input Defaults"), shortcutsPage);
    connect(resetShortcuts, &QPushButton::clicked,
            this, &PreferencesDialog::RestoreShortcutDefaults);
    shortcutsLayout->addWidget(resetShortcuts, 0, Qt::AlignLeft);

    shortcutErrorLabel_ = new QLabel(shortcutsPage);
    shortcutErrorLabel_->setStyleSheet(QStringLiteral("color: #e57373;"));
    shortcutErrorLabel_->setWordWrap(true);
    shortcutErrorLabel_->hide();
    shortcutsLayout->addWidget(shortcutErrorLabel_);
    tabs->addTab(shortcutsPage, QStringLiteral("Keybindings"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         this);
    connect(buttons, &QDialogButtonBox::accepted, this, &PreferencesDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &PreferencesDialog::reject);
    outerLayout->addWidget(buttons);
}

AppPreferences PreferencesDialog::Preferences() const {
    AppPreferences preferences;
    preferences.confirmNewGraph = confirmNewGraphCheck_->isChecked();
    preferences.automaticallyShowBuildLogs = showBuildLogsCheck_->isChecked();
    preferences.rememberWindowGeometry = rememberWindowGeometryCheck_->isChecked();
    preferences.allowExternalDeviceWrites = allowExternalDeviceWritesCheck_->isChecked();
    preferences.undoHistoryLimit = undoHistoryLimitSpin_->value();
    preferences.buildLogLineLimit = buildLogLineLimitSpin_->value();
    preferences.firmwareBuildType = buildTypeCombo_->currentText();
    preferences.serialPort = serialPortCombo_->currentText().trimmed();
    preferences.panMouseButton = static_cast<Qt::MouseButton>(
        panMouseButtonCombo_->currentData().toInt());
    return preferences;
}

QMap<QString, QKeySequence> PreferencesDialog::Shortcuts() const {
    QMap<QString, QKeySequence> result;
    for (auto it = shortcutEditors_.cbegin(); it != shortcutEditors_.cend(); ++it) {
        result.insert(it.key(), it.value()->keySequence());
    }
    return result;
}

void PreferencesDialog::accept() {
    if (serialPortCombo_->currentText().trimmed().isEmpty()) {
        QMessageBox::warning(this,
                             QStringLiteral("Device Port Required"),
                             QStringLiteral("Choose or enter a serial device port."));
        serialPortCombo_->setFocus();
        return;
    }

    QMap<QString, QString> ownerBySequence;
    for (const ShortcutBinding& binding : bindings_) {
        const QKeySequence sequence = shortcutEditors_.value(binding.id)->keySequence();
        const QString portable = sequence.toString(QKeySequence::PortableText);
        if (portable.isEmpty()) {
            continue;
        }
        const auto duplicate = ownerBySequence.constFind(portable);
        if (duplicate != ownerBySequence.cend()) {
            shortcutErrorLabel_->setText(
                QStringLiteral("Shortcut %1 is assigned to both “%2” and “%3”.")
                    .arg(sequence.toString(QKeySequence::NativeText),
                         duplicate.value(),
                         binding.label));
            shortcutErrorLabel_->show();
            return;
        }
        ownerBySequence.insert(portable, binding.label);
    }

    shortcutErrorLabel_->hide();
    QDialog::accept();
}

void PreferencesDialog::RestoreShortcutDefaults() {
    for (const ShortcutBinding& binding : bindings_) {
        if (QKeySequenceEdit* editor = shortcutEditors_.value(binding.id)) {
            editor->setKeySequence(binding.defaultSequence);
        }
    }
    panMouseButtonCombo_->setCurrentIndex(
        panMouseButtonCombo_->findData(static_cast<int>(Qt::MiddleButton)));
    shortcutErrorLabel_->hide();
}

}  // namespace NodeGUI
