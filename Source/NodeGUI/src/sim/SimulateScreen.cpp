#include "SimulateScreen.h"

#include "SimSession.h"
#include "SimTelemetryView.h"

#include <RTEAutomation/CachePaths.h>

#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGridLayout>
#include <QHBoxLayout>
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

// Regex-level sanity only — rte does the real parse; the badge says so.
// Grammar mirrors RTESim::ParseEventSpec: whitespace-separated t=.., kind=..
// and an optional arg=.. token without whitespace.
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
    if (!kinds.contains(match.captured(2))) {
        return QStringLiteral("unknown kind '%1'").arg(match.captured(2));
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
    swEdit_->setPlaceholderText(QStringLiteral("Assets/Examples/foc_demo.json"));
    grid->addWidget(swEdit_, row, 1, 1, 3);
    auto* swBrowse = MakeBrowseButton(scenarioBox);
    connect(swBrowse, &QToolButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Controller Graph"), QString(),
            QStringLiteral("JSON (*.json)"));
        if (!path.isEmpty()) {
            swEdit_->setText(path);
        }
    });
    grid->addWidget(swBrowse, row, 4);
    swUseCurrentButton_ = new QPushButton(QStringLiteral("Use current document"), scenarioBox);
    swUseCurrentButton_->setToolTip(QStringLiteral(
        "Pull the Node Editor's open document at press time."));
    swUseCurrentButton_->setVisible(false);
    connect(swUseCurrentButton_, &QPushButton::clicked, this, [this] {
        const QString path =
            currentGraphProvider_ ? currentGraphProvider_() : QString();
        if (path.isEmpty()) {
            AppendLog(QStringLiteral("[warn] the Node Editor has no saved document yet.\n"));
            return;
        }
        swEdit_->setText(path);
    });
    grid->addWidget(swUseCurrentButton_, row, 5);
    ++row;

    grid->addWidget(new QLabel(QStringLiteral("Plant graph"), scenarioBox), row, 0);
    plantEdit_ = MakePathEdit(scenarioBox, QStringLiteral("simPlantGraph"));
    plantEdit_->setPlaceholderText(
        QStringLiteral("Assets/PlantExamples/gen7_bench_pmsm.json"));
    grid->addWidget(plantEdit_, row, 1, 1, 3);
    auto* plantBrowse = MakeBrowseButton(scenarioBox);
    connect(plantBrowse, &QToolButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Plant Graph"), QString(),
            QStringLiteral("JSON (*.json)"));
        if (!path.isEmpty()) {
            plantEdit_->setText(path);
        }
    });
    grid->addWidget(plantBrowse, row, 4);
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
    connect(nameEdit_, &QLineEdit::textEdited, this,
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
            this, QStringLiteral("Load Session"), QString(),
            QStringLiteral("Session JSONL (*.jsonl)"));
        if (!path.isEmpty()) {
            LoadSessionFile(path);
        }
    });
    grid->addWidget(loadButton, row, 5);
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

    auto* eventsLabel = new QLabel(QStringLiteral("Events (one per line)"), settingsBox);
    settingsGrid->addWidget(eventsLabel, 2, 0);
    eventsBadge_ = new QLabel(settingsBox);
    eventsBadge_->setToolTip(QStringLiteral(
        "Regex-level sanity only — rte does the real parse at Run time."));
    settingsGrid->addWidget(eventsBadge_, 2, 1, 1, 7);
    eventsEdit_ = new QPlainTextEdit(settingsBox);
    eventsEdit_->setObjectName(QStringLiteral("simEvents"));
    eventsEdit_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
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
                AppendLog(QStringLiteral("[error] %1\n").arg(process_->errorString()));
            });
    connect(process_, &QProcess::finished, this, &SimulateScreen::OnSimFinished);

    UpdateOutPathDefault();
    RefreshEventsBadge();
    RefreshSupplyBadge();
}

SimulateScreen::~SimulateScreen() {
    if (process_ && process_->state() != QProcess::NotRunning) {
        process_->kill();
        process_->waitForFinished(2000);
    }
}

void SimulateScreen::SetSwGraphPath(const QString& path) {
    swPathSetExternally_ = true;
    swUseCurrentButton_->setVisible(false);
    swEdit_->setText(path);
}

void SimulateScreen::SetPlantGraphPath(const QString& path) {
    plantEdit_->setText(path);
}

QString SimulateScreen::SwGraphPath() const {
    return swEdit_->text().trimmed();
}

QString SimulateScreen::PlantGraphPath() const {
    return plantEdit_->text().trimmed();
}

void SimulateScreen::SetRteBinary(const QString& path) {
    if (!path.trimmed().isEmpty()) {
        rteBinary_ = path.trimmed();
    }
}

void SimulateScreen::SetCurrentGraphProvider(std::function<QString()> provider) {
    currentGraphProvider_ = std::move(provider);
    swUseCurrentButton_->setVisible(currentGraphProvider_ && !swPathSetExternally_);
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
            "%1 %2 · sane (regex-level only — rte does the real parse)")
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

namespace {

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

}  // namespace

bool SimulateScreen::BuildArguments(QStringList& arguments, QString& error) const {
    const QString sw = ResolveProjectPath(EffectiveFieldText(swEdit_));
    const QString plant = ResolveProjectPath(EffectiveFieldText(plantEdit_));
    const QString base = ResolveProjectPath(EffectiveFieldText(baseEdit_));
    const QString name = nameEdit_->text().trimmed();
    const QString out = outEdit_->text().trimmed();
    // reflect the resolution back into the fields once composing succeeded
    if (sw != swEdit_->text().trimmed()) swEdit_->setText(sw);
    if (plant != plantEdit_->text().trimmed()) plantEdit_->setText(plant);
    if (base != baseEdit_->text().trimmed()) baseEdit_->setText(base);
    if (sw.isEmpty() || !QFileInfo(sw).isFile()) {
        error = QStringLiteral("controller graph file does not exist: %1").arg(sw);
        return false;
    }
    if (plant.isEmpty() || !QFileInfo(plant).isFile()) {
        error = QStringLiteral("plant graph file does not exist: %1").arg(plant);
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

    QStringList arguments;
    QString error;
    if (!BuildArguments(arguments, error)) {
        AppendLog(QStringLiteral("\n[cancelled] %1\n").arg(error));
        SetStatus(error, QLatin1String(kStatusBadStyle));
        bottomTabs_->setCurrentWidget(logView_);
        return;
    }

    // The runner keeps <out>.work next to the session file; make sure its
    // parent exists first.
    const QString outPath = outEdit_->text().trimmed();
    if (!QDir().mkpath(QFileInfo(outPath).absolutePath())) {
        AppendLog(QStringLiteral("\n[cancelled] could not create output directory for %1\n")
                      .arg(outPath));
        return;
    }

    AppendLog(QStringLiteral(
        "\n============================================================\n"
        "Run simulation: %1\n"
        "%2 %3\n"
        "============================================================\n")
                  .arg(nameEdit_->text().trimmed(), rteBinary_,
                       QuoteForLog(arguments)));

    progressList_->clear();
    artifactPath_.clear();
    completedEventSeen_ = false;
    cancelledByUser_ = false;
    cliOutputBuffer_.clear();
    SetRunning(true);
    SetStatus(QStringLiteral("Running %1...").arg(nameEdit_->text().trimmed()),
              QLatin1String(kStatusRunStyle));
    process_->setWorkingDirectory(QFileInfo(swEdit_->text().trimmed()).absolutePath());
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
        if (!message.isEmpty()) {
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
        SetStatus(QStringLiteral("Complete — %1 (%2 frames, %3 channels, plant: %4)")
                      .arg(QFileInfo(artifactPath_).fileName())
                      .arg(session ? session->Meta().telemetryFrames : 0)
                      .arg(session ? session->ChannelNames().size() : 0)
                      .arg(session && !session->Meta().plant.isEmpty()
                               ? session->Meta().plant
                               : QStringLiteral("(no stamp)")),
                  QLatin1String(kStatusOkStyle));
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
    runButton_->setEnabled(!running);
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
