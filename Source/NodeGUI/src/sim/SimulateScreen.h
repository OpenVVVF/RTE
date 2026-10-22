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
//
// Graph-row sourcing (defsim wave): each of the Controller/Plant rows holds
// one of three sources, always NAMED on the row caption below the field:
//   * current document — the open in-memory document of the Node Editor /
//     Plant Editor (the DEFAULT). At Run time the document is serialized
//     from memory (dirty '*' content included — no save dialog, no file
//     dirtying) into <out>.work/inputs/ and compiled from that shadow, so a
//     re-run always compiles what is on screen.
//   * placeholder example — the shipped example file, used only while the
//     bound editor holds no document; the caption names the placeholder
//     explicitly (never a silent stock-graph compile).
//   * explicit file — a typed/browsed/example-pair path (the OVERRIDE),
//     compiled straight from disk as before this wave.
class SimulateScreen : public QWidget {
    Q_OBJECT

public:
    explicit SimulateScreen(QWidget* parent = nullptr);
    ~SimulateScreen() override;

    // Identity of the editor document a "current document" row binds to,
    // read at caption-refresh time (cheap; no serialization).
    struct CurrentDocumentInfo {
        bool hasDocument = false;   // a document is open (path or content)
        bool dirty = false;         // unsaved edits pending ('*' state)
        QString documentPath;       // absolute path, "" for untitled
        QString displayName;        // file basename, "(untitled)", "" when none
    };

    // Explicit file override for a row (browse dialogs, the example-pair
    // button, and the legacy parent push): compiles from the given disk file.
    void SetSwGraphPath(const QString& path);
    void SetPlantGraphPath(const QString& path);
    // Effective display path of the row: the field text in explicit-file
    // mode ("" when still empty), the bound document's path in
    // current-document mode ("" for untitled/none).
    QString SwGraphPath() const;
    QString PlantGraphPath() const;
    // rte binary used for Run; defaults to the RTE_CLI_DEVELOPMENT_PATH
    // compile-time define (falling back to bare "rte" on PATH).
    void SetRteBinary(const QString& path);

    // Parent wiring for the current-document default: the info provider is a
    // cheap identity read for the row caption; the export provider returns
    // the document's in-memory serialization ("" = no document open) at Run
    // time, freshly per run. Read-only both — never a save, never a dirty.
    void SetCurrentDocumentInfoProvider(
        std::function<CurrentDocumentInfo()> provider);
    void SetCurrentPlantDocumentInfoProvider(
        std::function<CurrentDocumentInfo()> provider);
    void SetCurrentDocumentExportProvider(std::function<std::string()> provider);
    void SetCurrentPlantDocumentExportProvider(
        std::function<std::string()> provider);
    // Recompute both row captions from the current provider reads (called by
    // the parent's document-change hook and at every Run).
    void RefreshSourceCaptions();
    // The plant document just got a (new) path assigned (open / save-as):
    // the Plant row follows the open document again (rebinds from any
    // explicit-file override, replacing this wave's predecessor's stale-path
    // push).
    void BindPlantRowToCurrentDocument();

    // Offline seam ("Load session..."), also used when a run's artifact
    // event lands. Returns false (and shows the run log) on failure.
    bool LoadSessionFile(const QString& path);
    bool IsRunning() const;
    // The hosted telemetry region (session, retained store, cursor
    // machinery); read-only access for console-side followers.
    SimTelemetryView* TelemetryView() const { return telemetryView_; }

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
    // One graph row flips to explicit-file mode (typed text, browse pick,
    // example pair) or back to the current-document default ("Use current
    // document" button); both refresh the caption + kind badges.
    void SetSwSourceToFile(const QString& path);
    void SetPlantSourceToFile(const QString& path);
    // Honesty wave — run-validation warnings (status/log rows, never modals):
    // a) locked-rotor-by-default: empty dyno fields + no dyno-set event + the
    //    plant graph's Dyno.Rpm0 == 0 means the shaft never moves; says so.
    //    `plantJsonText` carries the current-memory serialization of a
    //    current-document row at Run time ("" = probe the disk path instead,
    //    for explicit-file/placeholder rows). Returns the warning text, or ""
    //    when a mechanical source exists.
    QString LockedRotorWarningText(const QString& plantJsonText,
                                   const QString& plantDiskPath) const;
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
    // command. Also emits the argument list on success. The sw/plant pair is
    // already resolved by StageRunInputs (shadow paths for current-document
    // rows, disk paths for the rest).
    bool BuildArguments(const QString& swPath, const QString& plantPath,
                        QStringList& arguments, QString& error) const;

    // What a graph row feeds a run with.
    enum class GraphSourceKind { CurrentDocument, ExplicitFile, PlaceholderExample };
    struct StagedRunInputs {
        QString swPath;
        QString plantPath;
        GraphSourceKind swKind = GraphSourceKind::PlaceholderExample;
        GraphSourceKind plantKind = GraphSourceKind::PlaceholderExample;
        // "current documents" / "explicit files" / "placeholder examples" /
        // "mixed" — the one-token summary in the run header + status line.
        QString sourceToken;
        // Per-row "[inputs] ..." log lines naming the exact source resolved
        // for THIS run (written fresh per run by StageRunInputs).
        QStringList lines;
        // Current-memory serialization of the plant row ("" when the row
        // resolved to a disk file) — the locked-rotor probe's honest read.
        QString plantExportText;
    };
    // Resolves both rows for this run and, for current-document rows,
    // serializes the in-memory documents into <out>.work/inputs/ (freshly
    // every run — an edit between runs always re-exports). Fills `staged`;
    // returns false with `error` on a serialization/write failure.
    bool StageRunInputs(StagedRunInputs& staged, QString& error);

    QString rteBinary_;
    // Current-document providers [defsim]: identity reads for the captions,
    // per-run live serialization for the compile. Null in the standalone
    // preview harness, where rows resolve to the placeholder example.
    std::function<CurrentDocumentInfo()> currentDocInfoProvider_;
    std::function<CurrentDocumentInfo()> currentPlantDocInfoProvider_;
    std::function<std::string()> currentDocExportProvider_;
    std::function<std::string()> currentPlantDocExportProvider_;
    // Per-row user intent: CurrentDocument (default) vs ExplicitFile.
    bool swSourceIsFile_ = false;
    bool plantSourceIsFile_ = false;

    QLineEdit* swEdit_ = nullptr;
    QLineEdit* plantEdit_ = nullptr;
    QLineEdit* baseEdit_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QLineEdit* outEdit_ = nullptr;
    QPushButton* swUseCurrentButton_ = nullptr;
    QPushButton* plantUseCurrentButton_ = nullptr;
    QLabel* swSourceCaption_ = nullptr;
    QLabel* plantSourceCaption_ = nullptr;
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
    // [defsim] the sourceToken of the in-flight/last run ("current
    // documents" / "explicit files" / ...), echoed on the Complete status
    // line so the run's provenance survives the scroll.
    QString lastRunSourceToken_;
    // Honesty-wave run-warning state: last typed duration entry that exceeds
    // the spin's max (0.0 = none; consumed by OnRun for the transcript row),
    // and whether the current run launched under the locked-rotor warning
    // (re-surfaced on the Complete status line).
    double overMaxDurationEntry_ = 0.0;
    bool lockedRotorWarnedForRun_ = false;
};

}  // namespace NodeGUI::sim
