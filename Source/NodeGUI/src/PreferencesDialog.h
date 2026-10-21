#pragma once

#include <QDialog>
#include <QKeySequence>
#include <QMap>
#include <QString>
#include <QVector>
#include <Qt>

class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QKeySequenceEdit;
class QSpinBox;
class QTabWidget;

namespace NodeGUI {

struct AppPreferences {
    bool confirmNewGraph = true;
    bool automaticallyShowBuildLogs = true;
    bool rememberWindowGeometry = true;
    bool allowExternalDeviceWrites = false;
    // Snap node origins, port rows and wire bends to the scene grid
    // (WireRoute::kGridPitch). Disabling restores free-form placement.
    bool gridSnap = true;
    int undoHistoryLimit = 100;
    int buildLogLineLimit = 5000;
    QString firmwareBuildType = QStringLiteral("Release");
    Qt::MouseButton panMouseButton = Qt::MiddleButton;
    // 0 = follow the system palette (previous behavior), 1 = forced light,
    // 2 = forced dark (single-document dark palette applied app-wide; the
    // XCB/XWayland platform does not pick up the KDE/portal dark scheme,
    // so an explicit mode is the deterministic answer on that path).
    int themeMode = 0;
};

struct ShortcutBinding {
    QString id;
    QString category;
    QString label;
    QKeySequence defaultSequence;
    QAction* action = nullptr;
};

AppPreferences LoadAppPreferences();
void SaveAppPreferences(const AppPreferences& preferences);
QKeySequence LoadShortcutPreference(const QString& id,
                                    const QKeySequence& defaultSequence);
void SaveShortcutPreferences(const QMap<QString, QKeySequence>& shortcuts);

class PreferencesDialog final : public QDialog {
public:
    PreferencesDialog(const AppPreferences& preferences,
                      const QVector<ShortcutBinding>& bindings,
                      QWidget* parent = nullptr);

    AppPreferences Preferences() const;
    QMap<QString, QKeySequence> Shortcuts() const;

public slots:
    void accept() override;

private:
    void RestoreShortcutDefaults();

    QVector<ShortcutBinding> bindings_;
    QMap<QString, QKeySequenceEdit*> shortcutEditors_;
    QTabWidget* tabsWidget_ = nullptr;
    QWidget* shortcutsPage_ = nullptr;

    QCheckBox* confirmNewGraphCheck_ = nullptr;
    QCheckBox* showBuildLogsCheck_ = nullptr;
    QCheckBox* rememberWindowGeometryCheck_ = nullptr;
    QCheckBox* allowExternalDeviceWritesCheck_ = nullptr;
    QCheckBox* gridSnapCheck_ = nullptr;
    QSpinBox* undoHistoryLimitSpin_ = nullptr;
    QSpinBox* buildLogLineLimitSpin_ = nullptr;
    QComboBox* buildTypeCombo_ = nullptr;
    QComboBox* panMouseButtonCombo_ = nullptr;
    QComboBox* themeCombo_ = nullptr;
    QLabel* shortcutErrorLabel_ = nullptr;
};

}  // namespace NodeGUI
