#include "SimulateScreen.h"

#include "GraphKind.h"
#include "SimSession.h"
#include "SimTelemetryView.h"

#include <RTEAutomation/CachePaths.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTextCursor>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>
#include <filesystem>

namespace NodeGUI::sim {

namespace {

// Badge palette. Neutral inherits the theme; pass/warn get small colored
// text like the build-status labels elsewhere in Studio.
const char* kBadgeOkStyle = "color: #58b368;";
const char* kBadgeBadStyle = "color: #ef5350;";
const char* kBadgeWarnStyle = "color: #e6a23c;";
const char* kBadgeNeutralStyle = "color: palette(mid);";
const char* kStatusIdleStyle = "";
const char* kStatusRunStyle = "color: #e6a23c;";
const char* kStatusOkStyle = "color: #58b368;";
const char* kStatusBadStyle = "color: #ef5350;";

QString DefaultRteBinary() {
#ifdef RTE_CLI_DEVELOPMENT_PATH
    if (std::filesystem::is_regular_file(RTE_CLI_DEVELOPMENT_PATH)) {
        return QString::fromUtf8(RTE_CLI_DEVELOPMENT_PATH);
    }
#endif
    return QStringLiteral("rte");
}

QString DefaultBaseSourceDir() {
#ifdef RTE_PROJECT_ROOT
    const std::filesystem::path fromRoot =
        std::filesystem::path(RTE_PROJECT_ROOT) / "Images" / "Gen6FW";
    if (std::filesystem::is_directory(fromRoot)) {
        return QString::fromStdString(fromRoot.lexically_normal().string());
    }
#endif
    return QString::fromStdString(
        (std::filesystem::current_path() / "Images" / "Gen6FW")
            .lexically_normal()
            .string());
}

QStringList EventLines(const QString& text) {
    QStringList lines;
    for (const QString& raw : text.split(u'\n')) {
        const QString line = raw.trimmed();
        if (!line.isEmpty() && !line.startsWith(u'#')) {
            lines.push_back(line);
        }
    }
    return lines;
}

// Structural check + per-kind arg grammar. The kind list and the arg rules
// below DUPLICATE RTESim::ParseEventSpec (Source/RTESim/src/Scenario.cpp:
// 66-108) so a typo fails at edit time instead of after a whole sim run —
// keep them in sync by hand until the verb table moves into a shared
// RTESim/EventVocabulary header (deliberately NOT extracting yet; one
// caller on each side of the module boundary is acceptable for now per the
// 2026-10-21 integration dossier).
QString ValidateEventLine(const QString& line) {
    static const QRegularExpression pattern(
        QStringLiteral("^t=(\\S+)\\s+kind=([a-z][a-z0-9-]*)(?:\\s+arg=(\\S+))?$"));
    const auto match = pattern.match(line);
    if (!match.hasMatch()) {
        return QStringLiteral("expected 't=.. kind=.. [arg=..]'");
    }
    bool ok = false;
    const double tsec = match.captured(1).toDouble(&ok);
    if (!ok || !std::isfinite(tsec) || tsec < 0.0) {
        return QStringLiteral("time '%1' is not a finite number >= 0")
            .arg(match.captured(1));
    }
    static const QStringList kinds = {
        QStringLiteral("reset"),        QStringLiteral("start"),
        QStringLiteral("stop"),         QStringLiteral("var-set"),
        QStringLiteral("plant-input"),  QStringLiteral("fault-inject"),
        QStringLiteral("fault-clear"),  QStringLiteral("dyno-set"),
        QStringLiteral("gate-ready"),
    };
    const QString kind = match.captured(2);
    if (!kinds.contains(kind)) {
        return QStringLiteral("unknown kind '%1'").arg(kind);
    }
    const QString arg = match.captured(3);  // "" when the arg=.. token is absent

    // reset / start / stop (the fault-reset and control start/stop verbs):
    // no operand.
    if (kind == QLatin1String("reset") || kind == QLatin1String("start")
        || kind == QLatin1String("stop")) {
        if (!arg.isEmpty()) {
            return QStringLiteral("%1 takes no arg (got '%2')").arg(kind, arg);
        }
        return QString();
    }
    // var-set / plant-input: arg=<Name>=<Value>, Value numeric (mirrors
    // parseNameValue in Scenario.cpp).
    if (kind == QLatin1String("var-set") || kind == QLatin1String("plant-input")) {
        const qsizetype eq = arg.indexOf(u'=');
        if (eq <= 0) {
            return QStringLiteral("%1 needs arg=<Name>=<Value>").arg(kind);
        }
        const QString valueText = arg.mid(eq + 1);
        bool valueOk = false;
        const double value = valueText.toDouble(&valueOk);
        if (!valueOk || !std::isfinite(value)) {
            return QStringLiteral("%1 value is not a number: '%2'").arg(kind, valueText);
        }
        return QString();
    }
    // dyno-set: arg=<rpm>, numeric.
    if (kind == QLatin1String("dyno-set")) {
        bool rpmOk = false;
        const double rpm = arg.toDouble(&rpmOk);
        if (!rpmOk || !std::isfinite(rpm)) {
            return QStringLiteral("dyno-set needs arg=<rpm>, got '%1'").arg(arg);
        }
        return QString();
    }
    // gate-ready: 0|1, exactly (twin harness gate_ready vocabulary).
    if (kind == QLatin1String("gate-ready")) {
        if (arg != QLatin1String("0") && arg != QLatin1String("1")) {
            return QStringLiteral("gate-ready needs arg=0|1, got '%1'").arg(arg);
        }
        return QString();
    }
    // fault-inject: the Plant.Fault.Point port name set; rte rejects a typo'd
    // injection name, so the GUI does too.
    if (kind == QLatin1String("fault-inject")) {
        static const QStringList faultNames = {
            QStringLiteral("OpenGates"), QStringLiteral("EncoderDisconnect"),
            QStringLiteral("SensorRail"),
        };
        if (!faultNames.contains(arg)) {
            return QStringLiteral(
                "unknown fault name: '%1' (valid: OpenGates|EncoderDisconnect|SensorRail)")
                    .arg(arg);
        }
        return QString();
    }
    // fault-clear: any name passes rte through (a typo'd clear only clears
    // nothing), but an empty operand is always an authoring slip.
    if (kind == QLatin1String("fault-clear")) {
        if (arg.isEmpty()) {
            return QStringLiteral("fault-clear needs arg=<fault name>");
        }
        return QString();
    }
    return QString();
}

// The 7 set_supply keys rte accepts (python spellings; unset keys keep the
// plant graph's DcBus values).
QString ValidateSupplySpec(const QString& spec) {
    if (spec.trimmed().isEmpty()) {
        return QString();
    }
    static const QStringList keys = {
        QStringLiteral("v_set"),         QStringLiteral("i_limit"),
        QStringLiteral("sink_limit"),    QStringLiteral("can_sink"),
        QStringLiteral("cv_clamp_v"),    QStringLiteral("cv_clamp_sink_a"),
        QStringLiteral("series_r_mohm"),
    };
    static const QRegularExpression itemPattern(
        QStringLiteral("^([a-z_0-9]+)=(\\S+)$"));
    for (const QString& rawItem : spec.split(u',')) {
        const QString item = rawItem.trimmed();
        const auto match = itemPattern.match(item);
        if (!match.hasMatch()) {
            return QStringLiteral("'%1' is not k=v").arg(item);
        }
        if (!keys.contains(match.captured(1))) {
            return QStringLiteral("unknown key '%1' (known: %2)")
                .arg(match.captured(1), keys.join(QStringLiteral(", ")));
        }
        bool ok = false;
        const double value = match.captured(2).toDouble(&ok);
        if (!ok || !std::isfinite(value)) {
            return QStringLiteral("value of '%1' is not a finite number")
                .arg(match.captured(1));
        }
    }
    return QString();
}

QString QuoteForLog(const QStringList& arguments) {
    QStringList quoted;
    for (const QString& arg : arguments) {
        if (arg.contains(u' ') || arg.contains(u'\'')) {
            quoted.push_back(u'\'' + QString(arg).replace(u'\'', QLatin1String("'\\''")) + u'\'');
        } else {
            quoted.push_back(arg);
        }
    }
    return quoted.join(u' ');
}

QLineEdit* MakePathEdit(QWidget* parent, const QString& name) {
    auto* edit = new QLineEdit(parent);
    edit->setObjectName(name);
    edit->setClearButtonEnabled(true);
    return edit;
}

QToolButton* MakeBrowseButton(QWidget* parent) {
    auto* button = new QToolButton(parent);
    button->setText(QStringLiteral("..."));
    button->setToolTip(QStringLiteral("Browse"));
    return button;
}

// Sessions MRU list cap (same as the runtime preset MRU).
constexpr int kMaxRecentSessions = 10;

QSettings MakeSimSettings() {
    return QSettings(QStringLiteral("RTE"), QStringLiteral("RTEStudio"));
}

// Relative graph/base paths must work no matter where the Studio was
// launched from: resolve against the process cwd first (explicit user
// intent), then against the project root from the build define.
QString ResolveProjectPath(const QString& raw) {
    const QString text = raw.trimmed();
    if (QFileInfo(text).exists()) {
        return QFileInfo(text).absoluteFilePath();
    }
    const QString rooted = QStringLiteral(RTE_PROJECT_ROOT) + QStringLiteral("/") + text;
    if (QFileInfo(rooted).exists()) {
        return QFileInfo(rooted).absoluteFilePath();
    }
    return text;  // leave the user's text intact for the error message
}

// A path field whose line still shows only its placeholder falls back to
// that placeholder (it is the documented default, e.g. the foc_demo
// example), never to validation silence.
QString EffectiveFieldText(const QLineEdit* edit) {
    const QString text = edit->text().trimmed();
    return text.isEmpty() ? edit->placeholderText().trimmed() : text;
}

// [defsim] The shipped example pair the placeholder fallback compiles when
// the bound editor holds no document. The row caption names these loudly
// whenever they are the effective source (the r2/r3 silent-placeholder bug
// pattern must not come back through this default).
const QString& ExampleSwGraphPath() {
    static const QString path = QStringLiteral("Assets/Examples/foc_demo.json");
    return path;
}
const QString& ExamplePlantGraphPath() {
    static const QString path =
        QStringLiteral("Assets/PlantExamples/gen7_bench_pmsm.json");
    return path;
}

// Directory a run stage-serializes the current-memory input pair into:
// <out>.work/inputs/ (the runner keeps <out>.work next to the session file;
// the wave-2a compile-cache fingerprint walks sw/plant/host only, so this
// subdir never disturbs the cache key).
QString ShadowInputDirForOut(const QString& outPath) {
    return outPath + QStringLiteral(".work/inputs");
}

QJsonDocument ParseGraphJsonText(const QString& text) {
    return QJsonDocument::fromJson(text.toUtf8());
}

// Locked-rotor probe (honesty wave): finds the plant graph's Dyno/shaft
// node initial-rpm default. Returns true when a node with "Dyno" in its
// type id exists (speed then has a mechanical authority whose default the
// host honors when no CLI source is given — host F28). *rpm0 out: the Rpm0
// parameter (missing -> 0.0, matching PlantGenerator's Plant.Shaft.Dyno
// default). Unparseable documents report "no Dyno" and let rte's parse
// error own the failure at Run time.
bool PlantDynoRpm0FromDoc(const QJsonDocument& doc, double* rpm0) {
    if (!doc.isObject()) {
        return false;
    }
    const QJsonArray nodes = doc.object().value(QStringLiteral("nodes")).toArray();
    for (const auto& nodeValue : nodes) {
        const QJsonObject node = nodeValue.toObject();
        if (!node.value(QStringLiteral("type")).toString().contains(
                QStringLiteral("Dyno"))) {
            continue;
        }
        if (rpm0) {
            *rpm0 = 0.0;
            const QJsonValue raw =
                node.value(QStringLiteral("parameters")).toObject()
                    .value(QStringLiteral("Rpm0"));
            bool ok = false;
            const double parsed = raw.isString()
                                      ? raw.toString().toDouble(&ok)
                                      : raw.toDouble(0.0);
            if (ok || raw.isDouble()) {
                *rpm0 = parsed;
            }
        }
        return true;
    }
    return false;
}

bool PlantDynoRpm0(const QString& plantPath, double* rpm0) {
    QFile file(plantPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    return PlantDynoRpm0FromDoc(QJsonDocument::fromJson(file.readAll()), rpm0);
}

}  // namespace

SimulateScreen::SimulateScreen(QWidget* parent)
    : QWidget(parent)
    , rteBinary_(DefaultRteBinary()) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    // ---- Scenario header -------------------------------------------------
    auto* scenarioBox = new QWidget(this);
    auto* grid = new QGridLayout(scenarioBox);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(4);

    int row = 0;
    grid->addWidget(new QLabel(QStringLiteral("Controller graph"), scenarioBox), row, 0);
    swEdit_ = MakePathEdit(scenarioBox, QStringLiteral("simSwGraph"));
    grid->addWidget(swEdit_, row, 1, 1, 3);
    auto* swBrowse = MakeBrowseButton(scenarioBox);
    connect(swBrowse, &QToolButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Controller Graph"), QString(),
            QStringLiteral("JSON (*.json)"));
        if (!path.isEmpty()) {
            SetSwSourceToFile(path);
        }
    });
    grid->addWidget(swBrowse, row, 4);
    swUseCurrentButton_ = new QPushButton(QStringLiteral("Use current document"), scenarioBox);
    swUseCurrentButton_->setToolTip(QStringLiteral(
        "Bind this row back to the Node Editor's open document: at Run the "
        "in-memory graph (unsaved edits included) is serialized and compiled "
        "— no save needed."));
    swUseCurrentButton_->setVisible(false);
    connect(swUseCurrentButton_, &QPushButton::clicked, this, [this] {
        swSourceIsFile_ = false;
        swEdit_->clear();
        RefreshSourceCaptions();
        RefreshGraphKindBadges();
    });
    grid->addWidget(swUseCurrentButton_, row, 5);
    swKindBadge_ = new QLabel(scenarioBox);
    swKindBadge_->setObjectName(QStringLiteral("simSwKindBadge"));
    grid->addWidget(swKindBadge_, row, 6);
    connect(swEdit_, &QLineEdit::textChanged, this, [this] {
        // Any text (typed, browsed, script-set) is an explicit-file override
        // of the current-document default.
        swSourceIsFile_ = true;
        RefreshSourceCaptions();
        RefreshGraphKindBadges();
    });
    ++row;

    swSourceCaption_ = new QLabel(scenarioBox);
    swSourceCaption_->setObjectName(QStringLiteral("simSwSourceCaption"));
    swSourceCaption_->setStyleSheet(
        QStringLiteral("color: palette(mid); font-size: 11px;"));
    swSourceCaption_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    grid->addWidget(swSourceCaption_, row, 1, 1, 6);
    ++row;

    grid->addWidget(new QLabel(QStringLiteral("Plant graph"), scenarioBox), row, 0);
    plantEdit_ = MakePathEdit(scenarioBox, QStringLiteral("simPlantGraph"));
    grid->addWidget(plantEdit_, row, 1, 1, 3);
    auto* plantBrowse = MakeBrowseButton(scenarioBox);
    connect(plantBrowse, &QToolButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Plant Graph"), QString(),
            QStringLiteral("JSON (*.json)"));
        if (!path.isEmpty()) {
            SetPlantSourceToFile(path);
        }
    });
    grid->addWidget(plantBrowse, row, 4);
    // Same contract as the Controller row: rebind to the Plant Editor's live
    // document (the default), serialized from memory at Run time.
    plantUseCurrentButton_ =
        new QPushButton(QStringLiteral("Use current document"), scenarioBox);
    plantUseCurrentButton_->setToolTip(QStringLiteral(
        "Bind this row back to the Plant Editor's open document: at Run the "
        "in-memory graph (unsaved edits included) is serialized and compiled "
        "— no save needed."));
    plantUseCurrentButton_->setVisible(false);
    connect(plantUseCurrentButton_, &QPushButton::clicked, this, [this] {
        plantSourceIsFile_ = false;
        plantEdit_->clear();
        RefreshSourceCaptions();
        RefreshGraphKindBadges();
    });
    grid->addWidget(plantUseCurrentButton_, row, 5);
    plantKindBadge_ = new QLabel(scenarioBox);
    plantKindBadge_->setObjectName(QStringLiteral("simPlantKindBadge"));
    grid->addWidget(plantKindBadge_, row, 6);
    connect(plantEdit_, &QLineEdit::textChanged, this, [this] {
        plantSourceIsFile_ = true;
        RefreshSourceCaptions();
        RefreshGraphKindBadges();
    });
    ++row;

    plantSourceCaption_ = new QLabel(scenarioBox);
    plantSourceCaption_->setObjectName(QStringLiteral("simPlantSourceCaption"));
    plantSourceCaption_->setStyleSheet(
        QStringLiteral("color: palette(mid); font-size: 11px;"));
    plantSourceCaption_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    grid->addWidget(plantSourceCaption_, row, 1, 1, 6);
    ++row;

    grid->addWidget(new QLabel(QStringLiteral("Base source"), scenarioBox), row, 0);
    baseEdit_ = MakePathEdit(scenarioBox, QStringLiteral("simBaseSource"));
    baseEdit_->setText(DefaultBaseSourceDir());
    grid->addWidget(baseEdit_, row, 1, 1, 3);
    auto* baseBrowse = MakeBrowseButton(scenarioBox);
    connect(baseBrowse, &QToolButton::clicked, this, [this] {
        const QString path = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Firmware Base Source"), baseEdit_->text());
        if (!path.isEmpty()) {
            baseEdit_->setText(path);
        }
    });
    grid->addWidget(baseBrowse, row, 4);
    ++row;

    grid->addWidget(new QLabel(QStringLiteral("Scenario name"), scenarioBox), row, 0);
    nameEdit_ = new QLineEdit(QStringLiteral("studio_sim"), scenarioBox);
    nameEdit_->setObjectName(QStringLiteral("simScenarioName"));
    nameEdit_->setMaximumWidth(180);
    connect(nameEdit_, &QLineEdit::textChanged, this,
            [this] { UpdateOutPathDefault(); });
    grid->addWidget(nameEdit_, row, 1);
    grid->addWidget(new QLabel(QStringLiteral("Session file"), scenarioBox), row, 2);
    outEdit_ = MakePathEdit(scenarioBox, QStringLiteral("simOutPath"));
    connect(outEdit_, &QLineEdit::textEdited, this,
            [this] { outEditedManually_ = true; });
    grid->addWidget(outEdit_, row, 3);
    auto* outBrowse = MakeBrowseButton(scenarioBox);
    connect(outBrowse, &QToolButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Session Output"), outEdit_->text(),
            QStringLiteral("Session JSONL (*.jsonl)"));
        if (!path.isEmpty()) {
            outEditedManually_ = true;
            outEdit_->setText(path);
        }
    });
    grid->addWidget(outBrowse, row, 4);
    auto* loadButton = new QPushButton(QStringLiteral("Load session..."), scenarioBox);
    loadButton->setToolTip(QStringLiteral(
        "Open an existing v3 session file in the telemetry view."));
    connect(loadButton, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Load Session"), SessionStartDir(),
            QStringLiteral("Session JSONL (*.jsonl)"));
        if (!path.isEmpty()) {
            LoadSessionFile(path);
        }
    });
    grid->addWidget(loadButton, row, 5);
    // Sessions MRU (mirrors RuntimeTab's preset MRU plumbing): most recent
    // first, deduped, capped at 10; the placeholder row does nothing.
    sessionMruCombo_ = new QComboBox(scenarioBox);
    sessionMruCombo_->setObjectName(QStringLiteral("simSessionMru"));
    sessionMruCombo_->setToolTip(QStringLiteral(
        "Recently run / loaded sessions — pick one to load it."));
    connect(sessionMruCombo_, &QComboBox::activated, this, [this](int index) {
        if (index <= 0) {
            return;
        }
        const QString path = sessionMruCombo_->itemText(index);
        if (!path.isEmpty()) {
            LoadSessionFile(path);
        }
        sessionMruCombo_->setCurrentIndex(0);
    });
    grid->addWidget(sessionMruCombo_, row, 6);
    ++row;

    grid->setColumnStretch(3, 1);
    layout->addWidget(scenarioBox);

    // ---- Run settings ----------------------------------------------------
    auto* settingsBox = new QWidget(this);
    auto* settingsGrid = new QGridLayout(settingsBox);
    settingsGrid->setContentsMargins(0, 0, 0, 0);
    settingsGrid->setHorizontalSpacing(6);
    settingsGrid->setVerticalSpacing(4);

    settingsGrid->addWidget(new QLabel(QStringLiteral("Duration"), settingsBox), 0, 0);
    durationSpin_ = new QDoubleSpinBox(settingsBox);
    durationSpin_->setObjectName(QStringLiteral("simDuration"));
    durationSpin_->setRange(0.1, 300.0);
    durationSpin_->setDecimals(2);
    durationSpin_->setSingleStep(0.5);
    durationSpin_->setValue(3.0);
    durationSpin_->setSuffix(QStringLiteral(" s"));
    settingsGrid->addWidget(durationSpin_, 0, 1);

    settingsGrid->addWidget(new QLabel(QStringLiteral("dt-sub"), settingsBox), 0, 2);
    dtSubCombo_ = new QComboBox(settingsBox);
    dtSubCombo_->setObjectName(QStringLiteral("simDtSub"));
    dtSubCombo_->setEditable(true);
    dtSubCombo_->addItems({QStringLiteral("0.0002"), QStringLiteral("0.0001"),
                           QStringLiteral("0.00005")});
    dtSubCombo_->setCurrentText(QStringLiteral("0.0002"));
    dtSubCombo_->setToolTip(QStringLiteral(
        "Plant substep (1e-6..1e-3 s). The stiff IM rig "
        "(teco_vhz_induction) is validated at 50 us only — do not run it at "
        "the 200 us default."));
    settingsGrid->addWidget(dtSubCombo_, 0, 3);

    settingsGrid->addWidget(new QLabel(QStringLiteral("Dyno rpm"), settingsBox), 0, 4);
    dynoRpmEdit_ = new QLineEdit(settingsBox);
    dynoRpmEdit_->setObjectName(QStringLiteral("simDynoRpm"));
    dynoRpmEdit_->setPlaceholderText(QStringLiteral("hold, e.g. 500"));
    dynoRpmEdit_->setMaximumWidth(110);
    settingsGrid->addWidget(dynoRpmEdit_, 0, 5);

    settingsGrid->addWidget(new QLabel(QStringLiteral("Dyno series"), settingsBox), 0, 6);
    dynoSeriesEdit_ = new QLineEdit(settingsBox);
    dynoSeriesEdit_->setObjectName(QStringLiteral("simDynoSeries"));
    dynoSeriesEdit_->setPlaceholderText(QStringLiteral("or 0=500,1.5=0"));
    settingsGrid->addWidget(dynoSeriesEdit_, 0, 7);
    // Honesty wave (fa_engineer #3): the spin's Qt-side clamp used to swallow
    // an over-range typed entry with zero complaint — this badge (and a
    // one-shot Run-log row) names the clamp while the entry is fresh.
    durationBadge_ = new QLabel(settingsBox);
    durationBadge_->setObjectName(QStringLiteral("simDurationClampBadge"));
    settingsGrid->addWidget(durationBadge_, 0, 8);
    connect(durationSpin_, &QDoubleSpinBox::textChanged, this,
            [this] { RefreshDurationClampBadge(); });
    settingsGrid->setColumnStretch(8, 1);

    settingsGrid->addWidget(new QLabel(QStringLiteral("Supply"), settingsBox), 1, 0);
    supplyEdit_ = new QLineEdit(settingsBox);
    supplyEdit_->setObjectName(QStringLiteral("simSupply"));
    supplyEdit_->setPlaceholderText(QStringLiteral(
        "optional: v_set=144.7,i_limit=40,... (unset keys keep the plant graph values)"));
    supplyEdit_->setToolTip(QStringLiteral(
        "Bench supply overrides, k=v[,k=v]. Keys: v_set, i_limit, "
        "sink_limit, can_sink, cv_clamp_v, cv_clamp_sink_a, series_r_mohm."));
    connect(supplyEdit_, &QLineEdit::textChanged, this,
            [this] { RefreshSupplyBadge(); });
    settingsGrid->addWidget(supplyEdit_, 1, 1, 1, 5);
    supplyBadge_ = new QLabel(settingsBox);
    settingsGrid->addWidget(supplyBadge_, 1, 6, 1, 2);

    fallbackPlantCheck_ = new QCheckBox(
        QStringLiteral("Allow fallback plant (fixed Gen7 physics substitute)"), settingsBox);
    fallbackPlantCheck_->setObjectName(QStringLiteral("simAllowFallbackPlant"));
    fallbackPlantCheck_->setToolTip(QStringLiteral(
        "Only for rte builds without PlantCodegenLib: rte otherwise refuses — "
        "'re-run with --allow-fallback-plant to opt into the built-in "
        "Gen7-bench fallback plant'. The substitute is fixed Gen7-bench "
        "physics, not the picked graph's physics, so it stays opt-in."));
    settingsGrid->addWidget(fallbackPlantCheck_, 1, 8);

    auto* eventsLabel = new QLabel(QStringLiteral("Events (one per line)"), settingsBox);
    settingsGrid->addWidget(eventsLabel, 2, 0);
    eventsBadge_ = new QLabel(settingsBox);
    eventsBadge_->setObjectName(QStringLiteral("simEventsBadge"));
    eventsBadge_->setToolTip(QStringLiteral(
        "Structure plus per-kind arg rules mirrored from rte's Scenario "
        "parser; rte still does the final parse at Run time."));
    settingsGrid->addWidget(eventsBadge_, 2, 1, 1, 7);
    eventsEdit_ = new QPlainTextEdit(settingsBox);
    eventsEdit_->setObjectName(QStringLiteral("simEvents"));
    eventsEdit_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    // Tab leaves the field instead of swallowing focus: a multi-line text
    // edit that traps Tab is a keyboard-navigation dead end for the whole
    // Simulate form (accessibility persona 2026-10-21, top fix row).
    eventsEdit_->setTabChangesFocus(true);
    eventsEdit_->setPlaceholderText(QStringLiteral(
        "t=0.1 kind=reset\nt=0.5 kind=start\nt=1.0 kind=var-set arg=IqVar=150"));
    eventsEdit_->setPlainText(QStringLiteral(
        "t=0.1 kind=reset\nt=0.5 kind=start\nt=1.0 kind=var-set arg=IqVar=150"));
    eventsEdit_->setMaximumHeight(80);
    connect(eventsEdit_, &QPlainTextEdit::textChanged, this,
            [this] { RefreshEventsBadge(); });
    settingsGrid->addWidget(eventsEdit_, 3, 0, 1, 8);

    layout->addWidget(settingsBox);

    // ---- Action row + compact progress -----------------------------------
    auto* actionRow = new QHBoxLayout;
    actionRow->setSpacing(6);
    runButton_ = new QPushButton(QStringLiteral("Run"), this);
    runButton_->setObjectName(QStringLiteral("simRun"));
    connect(runButton_, &QPushButton::clicked, this, &SimulateScreen::OnRun);
    actionRow->addWidget(runButton_);
    cancelButton_ = new QPushButton(QStringLiteral("Cancel"), this);
    cancelButton_->setObjectName(QStringLiteral("simCancel"));
    cancelButton_->setEnabled(false);
    connect(cancelButton_, &QPushButton::clicked, this, &SimulateScreen::OnCancel);
    actionRow->addWidget(cancelButton_);
    auto* examplePairButton = new QPushButton(QStringLiteral("Use example pair"), this);
    examplePairButton->setObjectName(QStringLiteral("simUseExamplePair"));
    examplePairButton->setToolTip(QStringLiteral(
        "Fill both rows with the worked example as EXPLICIT FILES (an "
        "override of the current-document default): foc_demo controller + "
        "gen7_bench_pmsm plant (resolved against the project root)."));
    connect(examplePairButton, &QPushButton::clicked, this, [this] {
        SetSwSourceToFile(ResolveProjectPath(ExampleSwGraphPath()));
        SetPlantSourceToFile(ResolveProjectPath(ExamplePlantGraphPath()));
    });
    actionRow->addWidget(examplePairButton);
    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName(QStringLiteral("simStatus"));
    statusLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    SetStatus(QStringLiteral("Idle — fill the scenario and press Run."),
              QLatin1String(kStatusIdleStyle));
    actionRow->addWidget(statusLabel_, 1);
    layout->addLayout(actionRow);

    progressList_ = new QListWidget(this);
    progressList_->setObjectName(QStringLiteral("simProgress"));
    progressList_->setMaximumHeight(96);
    progressList_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    layout->addWidget(progressList_);

    // ---- Telemetry / run log ---------------------------------------------
    bottomTabs_ = new QTabWidget(this);
    telemetryView_ = new SimTelemetryView(bottomTabs_);
    bottomTabs_->addTab(telemetryView_, QStringLiteral("Telemetry"));
    logView_ = new QPlainTextEdit(bottomTabs_);
    logView_->setObjectName(QStringLiteral("simLog"));
    logView_->setReadOnly(true);
    logView_->setTabChangesFocus(true);
    logView_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    logView_->setMaximumBlockCount(4000);
    bottomTabs_->addTab(logView_, QStringLiteral("Run log"));
    layout->addWidget(bottomTabs_, 1);

    // The CLI process (mirrors MainWindow's build runner: --format jsonl
    // stdout, buffered line split, log tail as the failure surface).
    process_ = new QProcess(this);
    connect(process_, &QProcess::readyReadStandardOutput, this,
            &SimulateScreen::OnSimStdout);
    connect(process_, &QProcess::readyReadStandardError, this,
            &SimulateScreen::OnSimStderr);
    connect(process_, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError) {
                if (!cancelledByUser_) {   // a user-initiated kill is not an error
                    AppendLog(QStringLiteral("[error] %1\n").arg(process_->errorString()));
                }
            });
    connect(process_, &QProcess::finished, this, &SimulateScreen::OnSimFinished);

    UpdateOutPathDefault();
    RefreshEventsBadge();
    RefreshSupplyBadge();
    RefreshGraphKindBadges();
    RefreshSessionMruCombo();
    RestoreScenarioState();
    RefreshSourceCaptions();
}

SimulateScreen::~SimulateScreen() {
    if (process_ && process_->state() != QProcess::NotRunning) {
        process_->kill();
        process_->waitForFinished(2000);
    }
}

void SimulateScreen::SaveScenarioState() {
    auto settings = MakeSimSettings();
    settings.beginGroup(QStringLiteral("sim/scenario"));
    settings.setValue(QStringLiteral("duration"), durationSpin_->value());
    settings.setValue(QStringLiteral("dtSub"), dtSubCombo_->currentText());
    settings.setValue(QStringLiteral("dynoRpm"), dynoRpmEdit_->text());
    settings.setValue(QStringLiteral("dynoSeries"), dynoSeriesEdit_->text());
    settings.setValue(QStringLiteral("supply"), supplyEdit_->text());
    settings.setValue(QStringLiteral("events"), eventsEdit_->toPlainText());
    settings.setValue(QStringLiteral("name"), nameEdit_->text());
    // [defsim] Row sourcing intent: current-document (the default) survives
    // relaunch; an explicit-file pick survives WITH its path, still labelled
    // "explicit file pick" on restore (never re-armed as "current", and a
    // missing file falls to the named placeholder at Run).
    settings.setValue(QStringLiteral("swSource"),
                      swSourceIsFile_ ? QStringLiteral("file")
                                      : QStringLiteral("current"));
    settings.setValue(QStringLiteral("plantSource"),
                      plantSourceIsFile_ ? QStringLiteral("file")
                                         : QStringLiteral("current"));
    settings.setValue(QStringLiteral("swPath"), swEdit_->text());
    settings.setValue(QStringLiteral("plantPath"), plantEdit_->text());
    settings.endGroup();
}

void SimulateScreen::RestoreScenarioState() {
    auto settings = MakeSimSettings();
    settings.beginGroup(QStringLiteral("sim/scenario"));
    if (!settings.contains(QStringLiteral("duration"))) {
        settings.endGroup();
        return;
    }
    durationSpin_->setValue(settings.value(QStringLiteral("duration")).toDouble());
    dtSubCombo_->setCurrentText(settings.value(QStringLiteral("dtSub")).toString());
    dynoRpmEdit_->setText(settings.value(QStringLiteral("dynoRpm")).toString());
    dynoSeriesEdit_->setText(settings.value(QStringLiteral("dynoSeries")).toString());
    supplyEdit_->setText(settings.value(QStringLiteral("supply")).toString());
    eventsEdit_->setPlainText(settings.value(QStringLiteral("events")).toString());
    nameEdit_->setText(settings.value(QStringLiteral("name")).toString());
    // Text first (the textChanged hook marks the row file-sourced), then the
    // persisted source intent re-asserts the mode on top.
    swEdit_->setText(settings.value(QStringLiteral("swPath")).toString());
    plantEdit_->setText(settings.value(QStringLiteral("plantPath")).toString());
    swSourceIsFile_ = settings.value(QStringLiteral("swSource")).toString()
                      == QStringLiteral("file");
    plantSourceIsFile_ = settings.value(QStringLiteral("plantSource")).toString()
                         == QStringLiteral("file");
    settings.endGroup();
    RefreshSourceCaptions();
    RefreshGraphKindBadges();
}

void SimulateScreen::SetSwSourceToFile(const QString& path) {
    swSourceIsFile_ = true;
    swEdit_->setText(path);   // textChanged marks the row file-sourced too
    RefreshSourceCaptions();
    RefreshGraphKindBadges();
}

void SimulateScreen::SetPlantSourceToFile(const QString& path) {
    plantSourceIsFile_ = true;
    plantEdit_->setText(path);
    RefreshSourceCaptions();
    RefreshGraphKindBadges();
}

void SimulateScreen::SetSwGraphPath(const QString& path) {
    SetSwSourceToFile(path);
}

void SimulateScreen::SetPlantGraphPath(const QString& path) {
    SetPlantSourceToFile(path);
}

QString SimulateScreen::SwGraphPath() const {
    if (!swSourceIsFile_ && currentDocInfoProvider_) {
        return currentDocInfoProvider_().documentPath;
    }
    return swEdit_->text().trimmed();
}

QString SimulateScreen::PlantGraphPath() const {
    if (!plantSourceIsFile_ && currentPlantDocInfoProvider_) {
        return currentPlantDocInfoProvider_().documentPath;
    }
    return plantEdit_->text().trimmed();
}

void SimulateScreen::SetRteBinary(const QString& path) {
    if (!path.trimmed().isEmpty()) {
        rteBinary_ = path.trimmed();
    }
}

void SimulateScreen::SetCurrentDocumentInfoProvider(
    std::function<CurrentDocumentInfo()> provider) {
    currentDocInfoProvider_ = std::move(provider);
    swUseCurrentButton_->setVisible(currentDocInfoProvider_ != nullptr);
    RefreshSourceCaptions();
}

void SimulateScreen::SetCurrentPlantDocumentInfoProvider(
    std::function<CurrentDocumentInfo()> provider) {
    currentPlantDocInfoProvider_ = std::move(provider);
    plantUseCurrentButton_->setVisible(currentPlantDocInfoProvider_ != nullptr);
    RefreshSourceCaptions();
}

void SimulateScreen::SetCurrentDocumentExportProvider(
    std::function<std::string()> provider) {
    currentDocExportProvider_ = std::move(provider);
}

void SimulateScreen::SetCurrentPlantDocumentExportProvider(
    std::function<std::string()> provider) {
    currentPlantDocExportProvider_ = std::move(provider);
}

void SimulateScreen::BindPlantRowToCurrentDocument() {
    plantSourceIsFile_ = false;
    {
        // The clear is not a user file-override; keep the textChanged hook
        // from flipping the row back to file mode.
        const QSignalBlocker blocker(plantEdit_);
        plantEdit_->clear();
    }
    RefreshSourceCaptions();
    RefreshGraphKindBadges();
}

void SimulateScreen::RefreshSourceCaptions() {
    // One row's caption + field placeholder, derived from the RESOLVED
    // source so the wording never drifts from what Run would actually do.
    // Placeholder fallbacks (no open document / emptied field) stay LOUD:
    // the daily_flow_r3 misery was a stock graph compiling silently under
    // the session's name; that pattern must not re-arm here.
    auto captionForRow = [](bool fileSource,
                            const std::function<CurrentDocumentInfo()>& infoProvider,
                            QLineEdit* edit, QLabel* caption,
                            const QString& editorName,
                            const QString& examplePath) {
        const QString exampleName = QFileInfo(examplePath).fileName();
        if (!fileSource) {
            CurrentDocumentInfo info;
            if (infoProvider) {
                info = infoProvider();
            }
            if (info.hasDocument) {
                caption->setText(QStringLiteral("current document (%1): %2%3")
                                     .arg(editorName, info.displayName,
                                          info.dirty ? QStringLiteral(" * (unsaved edits)")
                                                     : QString()));
                caption->setToolTip(QStringLiteral(
                    "Run compiles the in-memory document from the %1 "
                    "(serialized fresh at Run time, unsaved edits included; "
                    "no save happens). Type a path or use \"...\" for an "
                    "explicit file instead.").arg(editorName));
                caption->setStyleSheet(
                    QStringLiteral("color: palette(mid); font-size: 11px;"));
                edit->setPlaceholderText(QStringLiteral(
                    "current document (%1) — typing switches to an explicit file")
                        .arg(editorName));
                return;
            }
            caption->setText(QStringLiteral(
                "placeholder example: %1 (no document open in the %2)")
                    .arg(exampleName, editorName));
            caption->setToolTip(QStringLiteral(
                "The %1 has no document open, so Run falls back to the "
                "shipped example %2 — named here so it can never compile "
                "silently under your scenario's name.")
                    .arg(editorName, examplePath));
            caption->setStyleSheet(QStringLiteral("%1 font-size: 11px;")
                                            .arg(QLatin1String(kBadgeWarnStyle)));
            edit->setPlaceholderText(QStringLiteral(
                "placeholder example %1 — typing picks an explicit file").arg(examplePath));
            return;
        }
        const QString text = edit->text().trimmed();
        if (text.isEmpty()) {
            caption->setText(QStringLiteral(
                "placeholder example: %1 (row left empty)").arg(exampleName));
            caption->setToolTip(QStringLiteral(
                "The row's field is empty, so Run falls back to the shipped "
                "example %1.").arg(examplePath));
            caption->setStyleSheet(QStringLiteral("%1 font-size: 11px;")
                                            .arg(QLatin1String(kBadgeWarnStyle)));
        } else {
            caption->setText(QStringLiteral("explicit file pick: %1")
                                 .arg(QFileInfo(text).fileName()));
            caption->setToolTip(QStringLiteral(
                "Run compiles the on-disk file %1 (your override; in-memory "
                "editor edits do NOT affect the run until saved or "
                "\"Use current document\" is clicked).").arg(text));
            caption->setStyleSheet(
                QStringLiteral("color: palette(mid); font-size: 11px;"));
        }
        edit->setPlaceholderText(examplePath);
    };

    if (swSourceCaption_ && plantSourceCaption_) {
        captionForRow(swSourceIsFile_, currentDocInfoProvider_, swEdit_,
                      swSourceCaption_, QStringLiteral("Node Editor"),
                      ExampleSwGraphPath());
        captionForRow(plantSourceIsFile_, currentPlantDocInfoProvider_, plantEdit_,
                      plantSourceCaption_, QStringLiteral("Plant Editor"),
                      ExamplePlantGraphPath());
    }
}

QString SimulateScreen::SessionStartDir() const {
    const QString fromSettings =
        MakeSimSettings().value(QStringLiteral("sim/lastSessionDir")).toString();
    if (!fromSettings.isEmpty()) {
        return fromSettings;
    }
    return QString::fromStdString(
        (RTEAutomation::DefaultCacheRoot() / "sim").string());
}

void SimulateScreen::RecordSessionUsed(const QString& path) {
    auto settings = MakeSimSettings();
    // The next Load dialog (and the Runtime export suggestion) lands where
    // the last used session lives.
    settings.setValue(QStringLiteral("sim/lastSessionDir"),
                      QFileInfo(path).absolutePath());
    QStringList recent =
        settings.value(QStringLiteral("sim/recentSessions")).toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > kMaxRecentSessions) {
        recent.removeLast();
    }
    settings.setValue(QStringLiteral("sim/recentSessions"), recent);
    RefreshSessionMruCombo();
    sessionMruCombo_->setCurrentIndex(0);
}

void SimulateScreen::RefreshSessionMruCombo() {
    const QStringList recent =
        MakeSimSettings().value(QStringLiteral("sim/recentSessions")).toStringList();
    sessionMruCombo_->clear();
    sessionMruCombo_->addItem(QStringLiteral("recent sessions..."));
    sessionMruCombo_->addItems(recent);
}

bool SimulateScreen::LoadSessionFile(const QString& path) {
    QString error;
    if (!telemetryView_->LoadSession(path, error)) {
        AppendLog(QStringLiteral("[error] could not load session: %1\n").arg(error));
        SetStatus(QStringLiteral("Session load failed — see Run log."),
                  QLatin1String(kStatusBadStyle));
        bottomTabs_->setCurrentWidget(logView_);
        return false;
    }
    RecordSessionUsed(path);
    const SimSessionMeta& meta = telemetryView_->Session()->Meta();
    AppendLog(QStringLiteral("[session] %1 (%2 frames, %3 channels, plant: %4)\n")
                  .arg(path)
                  .arg(meta.telemetryFrames)
                  .arg(telemetryView_->Session()->ChannelNames().size())
                  .arg(meta.plant.isEmpty() ? QStringLiteral("(no stamp)")
                                            : meta.plant));
    bottomTabs_->setCurrentWidget(telemetryView_);
    return true;
}

bool SimulateScreen::IsRunning() const {
    return process_ && process_->state() != QProcess::NotRunning;
}

QString SimulateScreen::DefaultOutPath() const {
    QString name = nameEdit_->text().trimmed();
    if (name.isEmpty()) {
        name = QStringLiteral("studio_sim");
    }
    return QString::fromStdString(
        (RTEAutomation::DefaultCacheRoot() / "sim" / (name.toStdString() + ".jsonl"))
            .string());
}

void SimulateScreen::UpdateOutPathDefault() {
    if (!outEditedManually_) {
        outEdit_->setText(DefaultOutPath());
    }
}

void SimulateScreen::RefreshEventsBadge() {
    const QStringList lines = EventLines(eventsEdit_->toPlainText());
    for (int i = 0; i < lines.size(); ++i) {
        const QString issue = ValidateEventLine(lines[i]);
        if (!issue.isEmpty()) {
            eventsBadge_->setStyleSheet(QLatin1String(kBadgeBadStyle));
            eventsBadge_->setText(
                QStringLiteral("line %1: %2").arg(i + 1).arg(issue));
            return;
        }
    }
    if (lines.isEmpty()) {
        eventsBadge_->setStyleSheet(QLatin1String(kBadgeNeutralStyle));
        eventsBadge_->setText(QStringLiteral("no events"));
    } else {
        eventsBadge_->setStyleSheet(QLatin1String(kBadgeOkStyle));
        eventsBadge_->setText(QStringLiteral(
            "%1 %2 · sane (kind+arg grammar; rte does the final parse)")
                .arg(lines.size())
                .arg(lines.size() == 1 ? QStringLiteral("event")
                                       : QStringLiteral("events")));
    }
}

void SimulateScreen::RefreshSupplyBadge() {
    const QString issue = ValidateSupplySpec(supplyEdit_->text());
    if (!issue.isEmpty()) {
        supplyBadge_->setStyleSheet(QLatin1String(kBadgeBadStyle));
        supplyBadge_->setText(issue);
    } else if (supplyEdit_->text().trimmed().isEmpty()) {
        supplyBadge_->setStyleSheet(QLatin1String(kBadgeNeutralStyle));
        supplyBadge_->setText(QStringLiteral("optional"));
    } else {
        supplyBadge_->setStyleSheet(QLatin1String(kBadgeOkStyle));
        supplyBadge_->setText(QStringLiteral("sane (regex-level only)"));
    }
}

QString SimulateScreen::LockedRotorWarningText(const QString& plantJsonText,
                                               const QString& plantDiskPath) const {
    // Mechanical-source census: any one of these spins or schedules the shaft.
    if (!dynoRpmEdit_->text().trimmed().isEmpty()) {
        return QString();   // --dyno-rpm hold
    }
    if (!dynoSeriesEdit_->text().trimmed().isEmpty()) {
        return QString();   // --dyno-series schedule
    }
    for (const QString& line : EventLines(eventsEdit_->toPlainText())) {
        if (line.contains(QLatin1String("kind=dyno-set"))) {
            return QString();   // timeline assigns mechanical speed mid-run
        }
    }
    // No side-band source: the host stands on the graph's Dyno.Rpm0 (host
    // F28). A current-memory plant row probes the JUST-STAGED serialization
    // (unsaved edits count), otherwise the resolved disk path.
    double rpm0 = 0.0;
    bool hasDyno;
    if (!plantJsonText.isEmpty()) {
        hasDyno = PlantDynoRpm0FromDoc(ParseGraphJsonText(plantJsonText), &rpm0);
    } else {
        hasDyno = PlantDynoRpm0(plantDiskPath, &rpm0);
    }
    if (!hasDyno) {
        return QString();   // no Dyno/shaft node: speed is plant-physics-driven
    }
    if (std::abs(rpm0) > 1e-12) {
        return QString();   // the graph itself spins the shaft
    }
    return QStringLiteral(
        "⚠ mechanical speed static at 0 (Dyno rpm source unset) — locked-rotor "
        "bench: the rotor will not turn for the whole run; add a Dyno rpm hold, "
        "a dyno series, or a dyno-set event for a spinning bench");
}

void SimulateScreen::RefreshDurationClampBadge() {
    // cleanText is the editor's raw digits (no " s" suffix) — the ONLY place
    // an over-range typed entry is visible; value() is already clamped by
    // then. The post-clamp reformat reports exactly maximum() and must not
    // clear a pending warning (it is the clamp committing, not a new intent).
    bool numericOk = false;
    const double entered = durationSpin_->cleanText().trimmed().toDouble(&numericOk);
    const double maxDuration = durationSpin_->maximum();
    if (numericOk && std::isfinite(entered)) {
        if (entered > maxDuration) {
            if (overMaxDurationEntry_ != entered) {
                overMaxDurationEntry_ = entered;
                AppendLog(QStringLiteral(
                    "\n[warn] duration %1 s exceeds the Studio limit %2 s — this field "
                    "clamps to %2 s on commit and runs %2 s, not %1 s; longer scenarios "
                    "via CLI: rte sim --duration %1 ... --out <session.jsonl> (the CLI "
                    "has no session-length cap)\n")
                        .arg(entered).arg(maxDuration));
            }
        } else if (entered < maxDuration) {
            overMaxDurationEntry_ = 0.0;   // a genuine in-range edit
        }
    }
    if (overMaxDurationEntry_ > 0.0) {
        durationBadge_->setStyleSheet(QLatin1String(kBadgeWarnStyle));
        durationBadge_->setText(QStringLiteral(
            "⚠ %1 s clamps to %2 s — longer: rte sim --duration %1 ...")
                .arg(overMaxDurationEntry_).arg(maxDuration));
    } else {
        durationBadge_->setStyleSheet(QLatin1String(kBadgeNeutralStyle));
        durationBadge_->clear();
    }
}

void SimulateScreen::RefreshGraphKindBadges() {
    // Same check for both rows, mirrored expectations: the Controller row
    // must not hold a Plant graph and vice versa. Current-document rows skip
    // the badge (the editor kind is right by construction; the Run-time
    // check in BuildArguments re-validates the staged shadow as a graph).
    // Files that do not exist (yet) are left to the existence errors in
    // BuildArguments; files that exist but are not recognizable graph JSON
    // take the rte parse error at Run — neither draws a badge here.
    auto checkRow = [](QLineEdit* edit, QLabel* badge, bool fileSource,
                       GraphKind wrongKind, const QString& reason) {
        bool blocked = false;
        if (fileSource) {
            const QString text = EffectiveFieldText(edit);
            if (!text.isEmpty()) {
                const QString resolved = ResolveProjectPath(text);
                if (QFileInfo(resolved).isFile()
                    && GraphKindOf(resolved.toStdString()) == wrongKind) {
                    blocked = true;
                }
            }
        }
        if (blocked) {
            edit->setStyleSheet(QStringLiteral("QLineEdit { border: 1px solid #ef5350; }"));
            edit->setToolTip(reason);
            badge->setStyleSheet(QLatin1String(kBadgeBadStyle));
            badge->setText(reason);
        } else {
            edit->setStyleSheet(QString());
            edit->setToolTip(QString());
            badge->setStyleSheet(QLatin1String(kBadgeNeutralStyle));
            badge->setText(QString());
        }
        return blocked;
    };

    const QString swReason = QStringLiteral(
        "plant graph in Controller row — swap or pick another file");
    const QString plantReason = QStringLiteral(
        "controller graph in Plant row — swap or pick another file");
    const bool swBlocked =
        checkRow(swEdit_, swKindBadge_, swSourceIsFile_, GraphKind::Plant, swReason);
    const bool plantBlocked =
        checkRow(plantEdit_, plantKindBadge_, plantSourceIsFile_,
                 GraphKind::Controller, plantReason);

    QString reason;
    if (swBlocked && plantBlocked) {
        reason = swReason + QStringLiteral("; ") + plantReason;
    } else if (swBlocked) {
        reason = swReason;
    } else if (plantBlocked) {
        reason = plantReason;
    }

    if (!reason.isEmpty()) {
        kindBlockReason_ = reason;
        SetStatus(reason, QLatin1String(kStatusBadStyle));
    } else {
        // Restore the idle prompt only when the status line still shows the
        // kind block (never stomp a run result).
        if (!kindBlockReason_.isEmpty() && statusLabel_->text() == kindBlockReason_) {
            SetStatus(QStringLiteral("Idle — fill the scenario and press Run."),
                      QLatin1String(kStatusIdleStyle));
        }
        kindBlockReason_.clear();
    }
    runButton_->setEnabled(!IsRunning() && kindBlockReason_.isEmpty());
}

bool SimulateScreen::StageRunInputs(StagedRunInputs& staged, QString& error) {
    RefreshSourceCaptions();   // the run's wording mirrors the live captions
    const QString out = outEdit_->text().trimmed();

    struct RowStage {
        GraphSourceKind kind;
        QString path;
        QString exportText;   // only for CurrentDocument rows
    };
    auto stageRow = [&](bool fileSource, QLineEdit* edit,
                        const std::function<CurrentDocumentInfo()>& infoProvider,
                        const std::function<std::string()>& exportProvider,
                        const QString& editorName, const QString& rowName,
                        const QString& examplePath, const QString& shadowFileName,
                        RowStage& result, QStringList& lines) -> bool {
        const QString resolvedExample = ResolveProjectPath(examplePath);
        if (!fileSource) {
            const QString serialized =
                exportProvider ? QString::fromStdString(exportProvider())
                               : QString();
            if (!serialized.isEmpty()) {
                // Current-memory source: shadow-serialize FRESH into this
                // run's dir (every run; last run's export never re-arms).
                const QString dirPath = ShadowInputDirForOut(out);
                if (!QDir().mkpath(dirPath)) {
                    error = QStringLiteral(
                        "could not create run-input directory %1").arg(dirPath);
                    return false;
                }
                const QString shadowPath =
                    QStringLiteral("%1/%2").arg(dirPath, shadowFileName);
                QFile shadow(shadowPath);
                if (!shadow.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    error = QStringLiteral("could not write the %1 serialization to %2")
                                .arg(editorName, shadowPath);
                    return false;
                }
                shadow.write(serialized.toUtf8());
                if (!shadow.flush()) {
                    error = QStringLiteral("could not flush the %1 serialization to %2")
                                .arg(editorName, shadowPath);
                    return false;
                }
                shadow.close();
                result.kind = GraphSourceKind::CurrentDocument;
                result.path = shadowPath;
                result.exportText = serialized;
                QString docNote;
                if (infoProvider) {
                    const CurrentDocumentInfo info = infoProvider();
                    docNote = info.hasDocument
                        ? QStringLiteral(" '%1'%2")
                              .arg(info.displayName,
                                   info.dirty ? QStringLiteral(" (with unsaved edits)")
                                              : QString())
                        : QStringLiteral(" (document identity unread)");
                }
                lines.push_back(QStringLiteral(
                    "[inputs] %1 = current document (%2)%3 — serialized from "
                    "memory (no save) to %4")
                        .arg(rowName, editorName, docNote, shadowPath));
                return true;
            }
            if (exportProvider && infoProvider && infoProvider().hasDocument) {
                // A document is open but its serialization came back empty:
                // NEVER quietly downgrade to the placeholder here.
                error = QStringLiteral(
                    "the %1 document could not be serialized from memory — "
                    "refusing to run (no silent fallback)")
                        .arg(editorName);
                return false;
            }
            // No document open in the bound editor: NAMED placeholder.
            result.kind = GraphSourceKind::PlaceholderExample;
            result.path = resolvedExample;
            result.exportText.clear();
            lines.push_back(QStringLiteral(
                "[inputs] %1 = placeholder example %2 (no document open in "
                "the %3 — open one, or pick a file to override)")
                    .arg(rowName, resolvedExample, editorName));
            return true;
        }
        const QString text = edit->text().trimmed();
        if (text.isEmpty()) {
            result.kind = GraphSourceKind::PlaceholderExample;
            result.path = resolvedExample;
            result.exportText.clear();
            lines.push_back(QStringLiteral(
                "[inputs] %1 = placeholder example %2 (row left empty)")
                    .arg(rowName, resolvedExample));
            return true;
        }
        result.kind = GraphSourceKind::ExplicitFile;
        result.path = ResolveProjectPath(text);
        result.exportText.clear();
        lines.push_back(QStringLiteral(
            "[inputs] %1 = explicit file %2 (compiled from disk; in-memory "
            "edits not included)")
                .arg(rowName, result.path));
        return true;
    };

    RowStage sw, plant;
    QStringList lines;
    if (!stageRow(swSourceIsFile_, swEdit_, currentDocInfoProvider_,
                  currentDocExportProvider_, QStringLiteral("Node Editor"),
                  QStringLiteral("controller graph"), ExampleSwGraphPath(),
                  QStringLiteral("current_controller_graph.json"), sw, lines)) {
        return false;
    }
    if (!stageRow(plantSourceIsFile_, plantEdit_, currentPlantDocInfoProvider_,
                  currentPlantDocExportProvider_, QStringLiteral("Plant Editor"),
                  QStringLiteral("plant graph"), ExamplePlantGraphPath(),
                  QStringLiteral("current_plant_graph.json"), plant, lines)) {
        return false;
    }

    staged.swPath = sw.path;
    staged.plantPath = plant.path;
    staged.swKind = sw.kind;
    staged.plantKind = plant.kind;
    staged.lines = lines;
    staged.plantExportText = plant.exportText;
    if (sw.kind == GraphSourceKind::CurrentDocument
        && plant.kind == GraphSourceKind::CurrentDocument) {
        staged.sourceToken = QStringLiteral("current documents");
    } else if (sw.kind == GraphSourceKind::ExplicitFile
               && plant.kind == GraphSourceKind::ExplicitFile) {
        staged.sourceToken = QStringLiteral("explicit files");
    } else if (sw.kind == GraphSourceKind::PlaceholderExample
               && plant.kind == GraphSourceKind::PlaceholderExample) {
        staged.sourceToken = QStringLiteral("placeholder examples");
    } else {
        staged.sourceToken = QStringLiteral("mixed (see [inputs] lines)");
    }
    error.clear();
    return true;
}

bool SimulateScreen::BuildArguments(const QString& swPath, const QString& plantPath,
                                    QStringList& arguments, QString& error) const {
    const QString sw = swPath;
    const QString plant = plantPath;
    const QString base = ResolveProjectPath(EffectiveFieldText(baseEdit_));
    const QString name = nameEdit_->text().trimmed();
    const QString out = outEdit_->text().trimmed();
    if (sw.isEmpty() || !QFileInfo(sw).isFile()) {
        error = QStringLiteral("controller graph file does not exist: %1").arg(sw);
        return false;
    }
    if (plant.isEmpty() || !QFileInfo(plant).isFile()) {
        error = QStringLiteral("plant graph file does not exist: %1").arg(plant);
        return false;
    }
    // Kind checks ride on top of the existence checks (same rule the live
    // row badges use): a swapped pair must name the fix here too. Applies
    // uniformly — a staged current-memory shadow is re-validated as a graph.
    if (GraphKindOf(sw.toStdString()) == GraphKind::Plant) {
        error = QStringLiteral("plant graph in Controller row — swap or pick another file: %1")
                    .arg(sw);
        return false;
    }
    if (GraphKindOf(plant.toStdString()) == GraphKind::Controller) {
        error = QStringLiteral("controller graph in Plant row — swap or pick another file: %1")
                    .arg(plant);
        return false;
    }
    if (base.isEmpty() || !QFileInfo(base).isDir()) {
        error = QStringLiteral("base source directory does not exist: %1").arg(base);
        return false;
    }
    if (out.isEmpty()) {
        error = QStringLiteral("session output path is empty");
        return false;
    }

    bool dtOk = false;
    const double dtSub = dtSubCombo_->currentText().trimmed().toDouble(&dtOk);
    if (!dtOk || dtSub < 1e-6 || dtSub > 1e-3) {
        error = QStringLiteral("dt-sub must be a number in 1e-6..1e-3 s (got '%1')")
                    .arg(dtSubCombo_->currentText());
        return false;
    }

    const QString dynoRpm = dynoRpmEdit_->text().trimmed();
    const QString dynoSeries = dynoSeriesEdit_->text().trimmed();
    if (!dynoRpm.isEmpty() && !dynoSeries.isEmpty()) {
        error = QStringLiteral(
            "dyno rpm and dyno series are mutually exclusive — clear one");
        return false;
    }
    if (!dynoRpm.isEmpty()) {
        bool ok = false;
        dynoRpm.toDouble(&ok);
        if (!ok) {
            error = QStringLiteral("dyno rpm is not a number: %1").arg(dynoRpm);
            return false;
        }
    } else if (!dynoSeries.isEmpty()) {
        // Same shape as RTESim::ParseDynoSeries: t=rpm pairs, comma-joined.
        static const QRegularExpression seriesPattern(QStringLiteral(
            "^(-?\\d+(?:\\.\\d+)?=-?\\d+(?:\\.\\d+)?)"
            "(?:,-?\\d+(?:\\.\\d+)?=-?\\d+(?:\\.\\d+)?)*$"));
        if (!seriesPattern.match(dynoSeries).hasMatch()) {
            error = QStringLiteral(
                "dyno series must look like 0=500,1.5=0 (t=rpm pairs)");
            return false;
        }
    }

    const QString supplyIssue = ValidateSupplySpec(supplyEdit_->text());
    if (!supplyIssue.isEmpty()) {
        error = QStringLiteral("supply: %1").arg(supplyIssue);
        return false;
    }
    const QStringList events = EventLines(eventsEdit_->toPlainText());
    for (int i = 0; i < events.size(); ++i) {
        const QString issue = ValidateEventLine(events[i]);
        if (!issue.isEmpty()) {
            error = QStringLiteral("event line %1: %2").arg(i + 1).arg(issue);
            return false;
        }
    }

    arguments = {QStringLiteral("--format"), QStringLiteral("jsonl"),
                 QStringLiteral("sim"),
                 QStringLiteral("--sw-graph"), sw,
                 QStringLiteral("--plant-graph"), plant,
                 QStringLiteral("--base-source"), base,
                 QStringLiteral("--name"),
                 name.isEmpty() ? QStringLiteral("studio_sim") : name,
                 QStringLiteral("--duration"),
                 QString::number(durationSpin_->value()),
                 QStringLiteral("--dt-sub"), QString::number(dtSub, 'g', 12)};
    if (!dynoRpm.isEmpty()) {
        arguments << QStringLiteral("--dyno-rpm") << dynoRpm;
    } else if (!dynoSeries.isEmpty()) {
        arguments << QStringLiteral("--dyno-series") << dynoSeries;
    }
    if (!supplyEdit_->text().trimmed().isEmpty()) {
        arguments << QStringLiteral("--supply") << supplyEdit_->text().trimmed();
    }
    if (fallbackPlantCheck_->isChecked()) {
        arguments << QStringLiteral("--allow-fallback-plant");
    }
    for (const QString& event : events) {
        arguments << QStringLiteral("--event") << event;
    }
    arguments << QStringLiteral("--out") << out;
    error.clear();
    return true;
}

void SimulateScreen::OnRun() {
    if (IsRunning()) {
        AppendLog(QStringLiteral("\n[warning] A simulation is already running.\n"));
        return;
    }

    // [defsim] Resolve + stage the input pair FIRST: current-document rows
    // serialize the in-memory editor content (dirty '*' included) into
    // <out>.work/inputs/ — fresh every run, never a last-export re-arm.
    StagedRunInputs staged;
    QString stageError;
    if (!StageRunInputs(staged, stageError)) {
        AppendLog(QStringLiteral("\n[refused] %1\n").arg(stageError));
        SetStatus(stageError, QLatin1String(kStatusBadStyle));
        bottomTabs_->setCurrentWidget(logView_);
        return;
    }

    QStringList arguments;
    QString error;
    if (!BuildArguments(staged.swPath, staged.plantPath, arguments, error)) {
        AppendLog(QStringLiteral("\n[refused] %1\n").arg(error));
        SetStatus(error, QLatin1String(kStatusBadStyle));
        bottomTabs_->setCurrentWidget(logView_);
        return;
    }
    // Persist the scenario that actually runs (J3 dead-end fix: relaunch
    // restores exactly what you last ran with).
    SaveScenarioState();

    // The runner keeps <out>.work next to the session file; make sure its
    // parent exists first.
    const QString outPath = outEdit_->text().trimmed();
    if (!QDir().mkpath(QFileInfo(outPath).absolutePath())) {
        AppendLog(QStringLiteral("\n[refused] could not create output directory for %1\n")
                      .arg(outPath));
        return;
    }

    lastRunSourceToken_ = staged.sourceToken;
    AppendLog(QStringLiteral(
        "\n============================================================\n"
        "Run simulation: %1 — source=%2\n"
        "%3\n"
        "%4 %5\n"
        "============================================================\n")
                  .arg(nameEdit_->text().trimmed(), staged.sourceToken,
                       staged.lines.join(QStringLiteral("\n")), rteBinary_,
                       QuoteForLog(arguments)));

    progressList_->clear();
    artifactPath_.clear();
    completedEventSeen_ = false;
    cancelledByUser_ = false;
    cliOutputBuffer_.clear();

    // Honesty-wave run warnings (status/log rows, never modals; P1-r2 round):
    if (overMaxDurationEntry_ > 0.0) {
        const QString row = QStringLiteral(
            "⚠ duration run at %1 s — typed %2 s exceeds the Studio limit and the "
            "field clamped; longer scenarios via CLI: rte sim --duration %2 ... "
            "--out <session.jsonl> (no session-length cap)")
                .arg(durationSpin_->maximum()).arg(overMaxDurationEntry_);
        AppendLog(QStringLiteral("\n[warn] %1\n").arg(row));
        AppendProgress(QStringLiteral("warn"), row);
        overMaxDurationEntry_ = 0.0;
        RefreshDurationClampBadge();
    }
    lockedRotorWarnedForRun_ = false;
    const QString lockedRotor =
        LockedRotorWarningText(staged.plantExportText, staged.plantPath);
    if (!lockedRotor.isEmpty()) {
        lockedRotorWarnedForRun_ = true;
        AppendLog(QStringLiteral("\n[warn] %1\n").arg(lockedRotor));
        AppendProgress(QStringLiteral("warn"), lockedRotor);
    }

    SetRunning(true);
    SetStatus(QStringLiteral("Running %1 (source=%2)...")
                  .arg(nameEdit_->text().trimmed(), staged.sourceToken),
              QLatin1String(kStatusRunStyle));
    process_->setWorkingDirectory(QFileInfo(staged.swPath).absolutePath());
    process_->start(rteBinary_, arguments);
}

void SimulateScreen::OnCancel() {
    if (!IsRunning()) {
        return;
    }
    cancelledByUser_ = true;
    AppendLog(QStringLiteral("\n[cancelled] run aborted by the user.\n"));
    process_->kill();
}

void SimulateScreen::AppendProgress(const QString& phase, const QString& message) {
    auto* item = new QListWidgetItem(
        QStringLiteral("[%1] %2").arg(phase, message), progressList_);
    progressList_->addItem(item);
    while (progressList_->count() > 200) {
        delete progressList_->takeItem(0);
    }
    progressList_->setCurrentItem(item);
    progressList_->scrollToBottom();
}

void SimulateScreen::OnSimStdout() {
    if (!process_) {
        return;
    }
    cliOutputBuffer_ += process_->readAllStandardOutput();
    for (;;) {
        const qsizetype newline = cliOutputBuffer_.indexOf('\n');
        if (newline < 0) {
            break;
        }
        QByteArray line = cliOutputBuffer_.left(newline).trimmed();
        cliOutputBuffer_.remove(0, newline + 1);
        if (line.isEmpty()) {
            continue;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            // Not an Emit line: surface it raw (compiler spill, hostmain
            // chatter, etc.) instead of dropping it.
            AppendLog(QString::fromUtf8(line) + u'\n');
            continue;
        }
        const QJsonObject object = document.object();
        const QString event = object.value(QStringLiteral("event")).toString();
        const QString message = object.value(QStringLiteral("message")).toString();
        const QString phase =
            object.value(QStringLiteral("phase")).toString(event);
        // 'error' events are logged exactly once (dedicated branch below) so
        // they do not also flow through the generic progress/message path
        if (!message.isEmpty() && event != QLatin1String("error")) {
            AppendProgress(phase, message);
            AppendLog(QStringLiteral("[%1] %2\n").arg(phase, message));
        }
        if (event == QLatin1String("artifact")
            && object.value(QStringLiteral("kind")).toString()
                   == QLatin1String("session-jsonl")) {
            artifactPath_ = object.value(QStringLiteral("path")).toString();
            AppendProgress(QStringLiteral("artifact"), artifactPath_);
            // The telemetry region auto-shows as soon as the file exists; a
            // load failure here is reported again on finish, not thrown.
            QString loadError;
            if (!telemetryView_->LoadSession(artifactPath_, loadError)) {
                AppendLog(QStringLiteral("[error] artifact would not load: %1\n")
                              .arg(loadError));
                artifactPath_.clear();
            } else {
                RecordSessionUsed(artifactPath_);
                bottomTabs_->setCurrentWidget(telemetryView_);
            }
        } else if (event == QLatin1String("complete")) {
            completedEventSeen_ = object.value(QStringLiteral("success")).toBool();
        } else if (event == QLatin1String("error") && !message.isEmpty()) {
            AppendLog(QStringLiteral("[error] %1\n").arg(message));
        }
    }
}

void SimulateScreen::OnSimStderr() {
    if (!process_) {
        return;
    }
    const QByteArray text = process_->readAllStandardError();
    if (!text.isEmpty()) {
        AppendLog(QString::fromUtf8(text));
    }
}

void SimulateScreen::OnSimFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    OnSimStdout();  // drain anything still buffered
    if (!cliOutputBuffer_.trimmed().isEmpty()) {
        AppendLog(QString::fromUtf8(cliOutputBuffer_.trimmed()) + u'\n');
        cliOutputBuffer_.clear();
    }

    const bool cleanExit =
        exitStatus == QProcess::NormalExit && exitCode == 0 && completedEventSeen_;
    SetRunning(false);
    if (cancelledByUser_) {
        AppendLog(QStringLiteral("\n[finished] Run cancelled.\n"));
        SetStatus(QStringLiteral("Cancelled."), QLatin1String(kStatusBadStyle));
        bottomTabs_->setCurrentWidget(logView_);
        return;
    }
    if (cleanExit && !artifactPath_.isEmpty()) {
        const SimSession* session = telemetryView_->Session();
        AppendLog(QStringLiteral("\n[finished] Run completed: %1\n").arg(artifactPath_));
        QString statusText =
            QStringLiteral("Complete — %1 (%2 frames, %3 channels, plant: %4, source: %5)")
                .arg(QFileInfo(artifactPath_).fileName())
                .arg(session ? session->Meta().telemetryFrames : 0)
                .arg(session ? session->ChannelNames().size() : 0)
                .arg(session && !session->Meta().plant.isEmpty()
                         ? session->Meta().plant
                         : QStringLiteral("(no stamp)"))
                .arg(lastRunSourceToken_.isEmpty()
                         ? QStringLiteral("unknown")
                         : lastRunSourceToken_);
        // Honesty wave: the warn row scrolled by with the run; keep its fact
        // visible on the line the green status would otherwise close alone.
        if (lockedRotorWarnedForRun_) {
            lockedRotorWarnedForRun_ = false;
            statusText += QStringLiteral(
                "  ·  ⚠ locked-rotor bench (dyno source unset)");
            SetStatus(statusText, QLatin1String(kStatusRunStyle));
            return;
        }
        SetStatus(statusText, QLatin1String(kStatusOkStyle));
        return;
    }
    // Failure surface: no exception, no modal — the log tail gets the focus.
    AppendLog(QStringLiteral("\n[finished] Run failed (exit code %1).\n")
                  .arg(exitCode));
    SetStatus(QStringLiteral("Failed (exit code %1) — see Run log.").arg(exitCode),
              QLatin1String(kStatusBadStyle));
    bottomTabs_->setCurrentWidget(logView_);
}

void SimulateScreen::SetStatus(const QString& text, const QString& color) {
    statusLabel_->setStyleSheet(color);
    statusLabel_->setText(text);
}

void SimulateScreen::SetRunning(bool running) {
    runButton_->setEnabled(!running && kindBlockReason_.isEmpty());
    cancelButton_->setEnabled(running);
}

void SimulateScreen::AppendLog(const QString& text) {
    if (!logView_ || text.isEmpty()) {
        return;
    }
    QTextCursor cursor = logView_->textCursor();
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);
    logView_->setTextCursor(cursor);
    logView_->verticalScrollBar()->setValue(
        logView_->verticalScrollBar()->maximum());
}

}  // namespace NodeGUI::sim
