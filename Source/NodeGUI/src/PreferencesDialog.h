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

namespace NodeGUI {

struct AppPreferences {
    bool confirmNewGraph = true;
    bool automaticallyShowBuildLogs = true;
    bool rememberWindowGeometry = true;
    bool allowExternalDeviceWrites = false;
    int undoHistoryLimit = 100;
    int buildLogLineLimit = 5000;
    QString firmwareBuildType = QStringLiteral("Release");
    Qt::MouseButton panMouseButton = Qt::MiddleButton;
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

    QCheckBox* confirmNewGraphCheck_ = nullptr;
    QCheckBox* showBuildLogsCheck_ = nullptr;
    QCheckBox* rememberWindowGeometryCheck_ = nullptr;
    QCheckBox* allowExternalDeviceWritesCheck_ = nullptr;
    QSpinBox* undoHistoryLimitSpin_ = nullptr;
    QSpinBox* buildLogLineLimitSpin_ = nullptr;
    QComboBox* buildTypeCombo_ = nullptr;
    QComboBox* panMouseButtonCombo_ = nullptr;
    QLabel* shortcutErrorLabel_ = nullptr;
};

}  // namespace NodeGUI
