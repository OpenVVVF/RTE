#pragma once

#include <QWidget>

#include <QProcess>

#include <functional>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;

namespace NodeGUI::sim {

class SimTelemetryView;

// The Studio "Simulate" screen: a dock-less single tab that composes an
// `rte sim` command line from the header/scenario fields, streams the
// parent CLI's --format jsonl progress (the same Emit contract the Build
// path uses), and renders the produced v3 session in the telemetry region
// below. See docs/studio-sim-tab.md for the design intent.
class SimulateScreen : public QWidget {
    Q_OBJECT

public:
    explicit SimulateScreen(QWidget* parent = nullptr);
    ~SimulateScreen() override;

    // Parent wiring: push the document paths the run should use. Calling
    // SetSwGraphPath retires the "Use current document" helper (the parent
    // owns the value from then on).
    void SetSwGraphPath(const QString& path);
    void SetPlantGraphPath(const QString& path);
    QString SwGraphPath() const;
    QString PlantGraphPath() const;
    // rte binary used for Run; defaults to the RTE_CLI_DEVELOPMENT_PATH
    // compile-time define (falling back to bare "rte" on PATH).
    void SetRteBinary(const QString& path);

    // Optional: until SetSwGraphPath is called explicitly, a "Use current
    // document" button next to the picker pulls whatever this returns at
    // press time.
    void SetCurrentGraphProvider(std::function<QString()> provider);
    // Same for the Plant row, sourced from the Plant Editor's document
    // (parity with the Sw row; the document-change hook only pushes at
    // path-assignment time).
    void SetCurrentPlantGraphProvider(std::function<QString()> provider);

    // Offline seam ("Load session..."), also used when a run's artifact
    // event lands. Returns false (and shows the run log) on failure.
    bool LoadSessionFile(const QString& path);
    bool IsRunning() const;

private slots:
    void OnRun();
    void OnCancel();
    void OnSimStdout();
    void OnSimStderr();
    void OnSimFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    void AppendLog(const QString& text);
    void AppendProgress(const QString& phase, const QString& message);
    void SetStatus(const QString& text, const QString& color);
    void SetRunning(bool running);
    QString DefaultOutPath() const;
    void UpdateOutPathDefault();
    void RefreshEventsBadge();
    void RefreshSupplyBadge();
    // Honesty wave — run-validation warnings (status/log rows, never modals):
    // a) locked-rotor-by-default: empty dyno fields + no dyno-set event + the
    //    plant graph's Dyno.Rpm0 == 0 means the shaft never moves; says so.
    //    Returns the warning text, or "" when a mechanical source exists.
    QString LockedRotorWarningText() const;
    // b) duration clamp: the duration spin (0.1..300 s) silently clamps typed
    //    entries > max on commit; the badge + a one-shot [warn] log row name
    //    the clamp and the rte CLI alternative while the raw text is fresh.
    void RefreshDurationClampBadge();
    // Scenario form persistence (dossier J3-dead-end: every sim field
    // died per relaunch). Keys: sim/scenario/*; saved on each actual run,
    // restored at construction.
    void SaveScenarioState();
    void RestoreScenarioState();
    // Wrong-kind graph in a picker row (plant file in the Controller row or
    // vice versa): red border + tooltip + badge on the field, Run disabled,
    // and the status line naming the fix until the fields are right again.
    void RefreshGraphKindBadges();
    // sim/lastSessionDir + sim/recentSessions QSettings plumbing (mirrors
    // RuntimeTab's preset MRU): every successful session use - dialog pick,
    // MRU pick, or a run artifact - moves to the front of the list and the
    // next Load dialog / Runtime export lands in its directory.
    QString SessionStartDir() const;
    void RecordSessionUsed(const QString& path);
    void RefreshSessionMruCombo();
    // First failure reason, or "" when the current fields form a runnable
    // command. Also emits the argument list on success.
    bool BuildArguments(QStringList& arguments, QString& error) const;

    QString rteBinary_;
    std::function<QString()> currentGraphProvider_;
    std::function<QString()> currentPlantGraphProvider_;
    bool swPathSetExternally_ = false;

    QLineEdit* swEdit_ = nullptr;
    QLineEdit* plantEdit_ = nullptr;
    QLineEdit* baseEdit_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QLineEdit* outEdit_ = nullptr;
    QPushButton* swUseCurrentButton_ = nullptr;
    QPushButton* plantUseCurrentButton_ = nullptr;
    QLabel* swKindBadge_ = nullptr;
    QLabel* plantKindBadge_ = nullptr;
    QCheckBox* fallbackPlantCheck_ = nullptr;
    QComboBox* sessionMruCombo_ = nullptr;
    QString kindBlockReason_;

    QDoubleSpinBox* durationSpin_ = nullptr;
    QLabel* durationBadge_ = nullptr;
    QComboBox* dtSubCombo_ = nullptr;
    QLineEdit* dynoRpmEdit_ = nullptr;
    QLineEdit* dynoSeriesEdit_ = nullptr;
    QLineEdit* supplyEdit_ = nullptr;
    QLabel* supplyBadge_ = nullptr;
    QPlainTextEdit* eventsEdit_ = nullptr;
    QLabel* eventsBadge_ = nullptr;

    QPushButton* runButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QListWidget* progressList_ = nullptr;
    QTabWidget* bottomTabs_ = nullptr;
    QPlainTextEdit* logView_ = nullptr;
    SimTelemetryView* telemetryView_ = nullptr;

    QProcess* process_ = nullptr;
    QByteArray cliOutputBuffer_;
    bool outEditedManually_ = false;
    bool completedEventSeen_ = false;
    bool cancelledByUser_ = false;
    QString artifactPath_;
    // Honesty-wave run-warning state: last typed duration entry that exceeds
    // the spin's max (0.0 = none; consumed by OnRun for the transcript row),
    // and whether the current run launched under the locked-rotor warning
    // (re-surfaced on the Complete status line).
    double overMaxDurationEntry_ = 0.0;
    bool lockedRotorWarnedForRun_ = false;
};

}  // namespace NodeGUI::sim
