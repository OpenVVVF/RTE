#include "PreferencesDialog.h"

#include <QAction>
#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
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
    preferences.gridSnap =
        settings.value(QStringLiteral("gridSnap"), true).toBool();
    preferences.showGraphValues =
        settings.value(QStringLiteral("showGraphValues"), true).toBool();
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
    preferences.panMouseButton = static_cast<Qt::MouseButton>(
        settings.value(QStringLiteral("panMouseButton"),
                       static_cast<int>(Qt::MiddleButton)).toInt());
    preferences.themeMode = std::clamp(
        settings.value(QStringLiteral("themeMode"), 0).toInt(), 0, 2);
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
    settings.setValue(QStringLiteral("gridSnap"), preferences.gridSnap);
    settings.setValue(QStringLiteral("showGraphValues"),
                      preferences.showGraphValues);
    settings.setValue(QStringLiteral("undoHistoryLimit"), preferences.undoHistoryLimit);
    settings.setValue(QStringLiteral("buildLogLineLimit"),
                      preferences.buildLogLineLimit);
    settings.setValue(QStringLiteral("firmwareBuildType"),
                      preferences.firmwareBuildType);
    settings.setValue(QStringLiteral("panMouseButton"),
                      static_cast<int>(preferences.panMouseButton));
    settings.setValue(QStringLiteral("themeMode"), preferences.themeMode);
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

    // Live filter across all tabs ("searchable in prefs"): narrows rows to
    // entries whose label text matches (see ApplyFilter).
    filterEdit_ = new QLineEdit(this);
    filterEdit_->setObjectName(QStringLiteral("prefSearch"));
    filterEdit_->setPlaceholderText(QStringLiteral("Filter settings\u2026"));
    filterEdit_->setClearButtonEnabled(true);
    connect(filterEdit_, &QLineEdit::textChanged,
            this, &PreferencesDialog::ApplyFilter);
    outerLayout->addWidget(filterEdit_);

    auto* tabs = new QTabWidget(this);
    tabsWidget_ = tabs;
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

    themeCombo_ = new QComboBox(generalPage);
    themeCombo_->setObjectName(QStringLiteral("pref.themeMode"));
    themeCombo_->addItems({QStringLiteral("System default"),
                           QStringLiteral("Light"),
                           QStringLiteral("Dark")});
    themeCombo_->setCurrentIndex(preferences.themeMode);
    generalLayout->addRow(QStringLiteral("Theme"), themeCombo_);

    gridSnapCheck_ =
        new QCheckBox(QStringLiteral("Snap nodes and wires to the scene grid"),
                      generalPage);
    gridSnapCheck_->setObjectName(QStringLiteral("pref.gridSnap"));
    gridSnapCheck_->setChecked(preferences.gridSnap);
    gridSnapCheck_->setToolTip(
        QStringLiteral("Node origins, port rows and wire bends snap to the "
                       "15-unit scene grid (the background texture pitch); turn off for free-form placement"));
    generalLayout->addRow(QStringLiteral("Editor grid"), gridSnapCheck_);

    showGraphValuesCheck_ = new QCheckBox(
        QStringLiteral("Show sim signal values on the editor graphs"),
        generalPage);
    showGraphValuesCheck_->setObjectName(QStringLiteral("pref.showGraphValues"));
    showGraphValuesCheck_->setChecked(preferences.showGraphValues);
    showGraphValuesCheck_->setToolTip(QStringLiteral(
        "With a sim session on the Simulate screen, each telemetry channel's "
        "value is drawn at the ports that publish it (value-at-cursor while "
        "the playback cursor is pinned). The same toggle lives under "
        "View > Graph Signal Values."));
    generalLayout->addRow(QStringLiteral("Graph values"), showGraphValuesCheck_);

    undoHistoryLimitSpin_ = new QSpinBox(generalPage);
    undoHistoryLimitSpin_->setRange(kMinimumHistoryLimit, kMaximumHistoryLimit);
    undoHistoryLimitSpin_->setValue(preferences.undoHistoryLimit);
    undoHistoryLimitSpin_->setSuffix(QStringLiteral(" changes"));
    undoHistoryLimitSpin_->setToolTip(
        QStringLiteral("Maximum number of graph changes retained for Undo"));
    generalLayout->addRow(QStringLiteral("Undo history"), undoHistoryLimitSpin_);
    tabs->addTab(generalPage, QStringLiteral("General"));

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
    shortcutTable_ = table;
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
    shortcutsPage_ = shortcutsPage;

    // ---- Filterable rows (the prefSearch line) ----------------------------
    // Form pages contribute one entry per label+field row; Automation is one
    // entry for its checkbox+hint pair; Keybindings contributes the pan row
    // and one entry per shortcut table row.
    auto collectFormPage = [this](QWidget* page, QFormLayout* form) {
        for (int row = 0; row < form->rowCount(); ++row) {
            QLayoutItem* labelItem = form->itemAt(row, QFormLayout::LabelRole);
            QLayoutItem* fieldItem = form->itemAt(row, QFormLayout::FieldRole);
            QWidget* labelWidget = labelItem ? labelItem->widget() : nullptr;
            QWidget* fieldWidget = fieldItem ? fieldItem->widget() : nullptr;
            QString haystack;
            if (auto* label = qobject_cast<QLabel*>(labelWidget)) {
                haystack += label->text();
            }
            if (auto* button = qobject_cast<QAbstractButton*>(fieldWidget)) {
                haystack += QLatin1Char(' ') + button->text();
            }
            FilterEntry entry;
            entry.page = page;
            entry.haystack = haystack.trimmed();
            if (labelWidget) entry.widgets.push_back(labelWidget);
            if (fieldWidget) entry.widgets.push_back(fieldWidget);
            if (!entry.widgets.isEmpty()) {
                filterEntries_.push_back(std::move(entry));
            }
        }
    };
    collectFormPage(generalPage, generalLayout);
    collectFormPage(buildPage, buildLayout);
    {
        FilterEntry entry;
        entry.page = automationPage;
        entry.haystack = allowExternalDeviceWritesCheck_->text()
            + QLatin1Char(' ') + automationHint->text();
        entry.widgets = {allowExternalDeviceWritesCheck_, automationHint};
        filterEntries_.push_back(std::move(entry));
    }
    {
        FilterEntry entry;
        entry.page = shortcutsPage;
        entry.haystack = QStringLiteral("Pan canvas mouse button");
        QWidget* panLabel = nullptr;
        if (QLayoutItem* item = mouseBindingsLayout->itemAt(0, QFormLayout::LabelRole)) {
            panLabel = item->widget();
        }
        if (panLabel) entry.widgets.push_back(panLabel);
        entry.widgets.push_back(panMouseButtonCombo_);
        filterEntries_.push_back(std::move(entry));
    }
    for (int row = 0; row < bindings_.size(); ++row) {
        FilterEntry entry;
        entry.page = shortcutsPage;
        entry.haystack = bindings_.at(row).category
            + QLatin1Char(' ') + bindings_.at(row).label;
        entry.tableRow = row;
        filterEntries_.push_back(std::move(entry));
    }

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
    preferences.gridSnap = gridSnapCheck_->isChecked();
    preferences.showGraphValues = showGraphValuesCheck_->isChecked();
    preferences.undoHistoryLimit = undoHistoryLimitSpin_->value();
    preferences.buildLogLineLimit = buildLogLineLimitSpin_->value();
    preferences.firmwareBuildType = buildTypeCombo_->currentText();
    preferences.panMouseButton = static_cast<Qt::MouseButton>(
        panMouseButtonCombo_->currentData().toInt());
    preferences.themeMode = themeCombo_->currentIndex();
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
            // Bring the user to the error instead of silently refusing on
            // whatever other tab OK was pressed from (clunk audit below-cut).
            if (tabsWidget_ && shortcutsPage_) {
                tabsWidget_->setCurrentWidget(shortcutsPage_);
            }
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

void PreferencesDialog::ApplyFilter(const QString& term) {
    const QString needle = term.trimmed();
    auto entryMatch = [&needle](const FilterEntry& entry) {
        return needle.isEmpty()
            || entry.haystack.contains(needle, Qt::CaseInsensitive);
    };
    int visibleCount = 0;
    QWidget* firstPageWithMatch = nullptr;
    for (const FilterEntry& entry : filterEntries_) {
        const bool match = entryMatch(entry);
        if (match) {
            ++visibleCount;
            if (!firstPageWithMatch && entry.page) {
                firstPageWithMatch = entry.page.data();
            }
        }
    }
    // A term that matches nothing restores the full dialog instead of
    // leaving the user on blank tabs.
    const bool forceAllVisible = visibleCount == 0;
    for (const FilterEntry& entry : filterEntries_) {
        const bool visible = forceAllVisible || entryMatch(entry);
        for (const auto& widget : entry.widgets) {
            if (widget) {
                widget->setVisible(visible);
            }
        }
        if (entry.tableRow >= 0 && shortcutTable_) {
            shortcutTable_->setRowHidden(entry.tableRow, !visible);
        }
    }
    if (!needle.isEmpty() && !forceAllVisible && firstPageWithMatch
        && tabsWidget_) {
        tabsWidget_->setCurrentWidget(firstPageWithMatch);
    }
}

}  // namespace NodeGUI
