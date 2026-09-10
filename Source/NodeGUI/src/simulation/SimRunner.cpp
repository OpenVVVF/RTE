#include "SimRunner.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#include <cstdlib>
#include <filesystem>

#include <RTEAutomation/Platform.h>

#ifdef Q_OS_UNIX
#include <signal.h>
#include <unistd.h>
#endif

namespace NodeGUI::simulation {

namespace {

constexpr int kGracefulStopMs = 3000;

// rte's stdout (and host_sim's, relayed through it) is fully buffered when the
// parent is a pipe, so the console would only update in 4 KiB chunks. Wrapping
// the process in GNU stdbuf forces line buffering; the preload propagates to
// the host_sim grandchild. Linux/BSD only; elsewhere we accept the buffering.
QStringList WrapLineBuffered(const QString& program, const QStringList& args,
                             QString* programOut) {
    static const QString stdbuf = QStandardPaths::findExecutable(QStringLiteral("stdbuf"));
    if (stdbuf.isEmpty()) {
        *programOut = program;
        return args;
    }
    *programOut = stdbuf;
    return QStringList{QStringLiteral("-oL"), QStringLiteral("-eL"), program} + args;
}

}  // namespace

SimRunner::SimRunner(QObject* parent)
    : QObject(parent) {
    process_ = new QProcess(this);
    process_->setProcessChannelMode(QProcess::MergedChannels);
    connect(process_, &QProcess::readyReadStandardOutput,
            this, &SimRunner::HandleReadyRead);
    connect(process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus status) {
                HandleReadyRead();
                emit finished(exitCode, status);
            });
}

SimRunner::~SimRunner() {
    if (process_->state() == QProcess::NotRunning) {
        return;
    }
#ifdef Q_OS_UNIX
    const qint64 pid = process_->processId();
    if (pid > 0) {
        ::kill(static_cast<pid_t>(-pid), SIGKILL);
    }
#endif
    process_->kill();
    process_->waitForFinished(3000);
}

bool SimRunner::IsRunning() const {
    return process_->state() != QProcess::NotRunning;
}

QString SimRunner::FindRteExecutable(QString* error) {
    const QByteArray envOverride = qgetenv("RTE_CLI");
    if (!envOverride.isEmpty()) {
        const QString overridePath = QString::fromLocal8Bit(envOverride);
        if (QFileInfo(overridePath).isFile()) {
            return QFileInfo(overridePath).absoluteFilePath();
        }
        if (error) {
            *error = QStringLiteral("RTE_CLI points at a missing file: %1").arg(overridePath);
        }
        return {};
    }

    const std::filesystem::path sibling =
        std::filesystem::path(QCoreApplication::applicationDirPath().toStdString())
        / RTEAutomation::ExecutableName("rte");
    std::error_code ec;
    if (std::filesystem::is_regular_file(sibling, ec)) {
        return QString::fromStdString(sibling.lexically_normal().string());
    }

#ifdef RTE_CLI_DEVELOPMENT_PATH
    if (std::filesystem::is_regular_file(RTE_CLI_DEVELOPMENT_PATH, ec)) {
        return QString::fromUtf8(RTE_CLI_DEVELOPMENT_PATH);
    }
#endif

    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("rte"));
    if (!onPath.isEmpty()) {
        return onPath;
    }

    if (error) {
        *error = QStringLiteral(
            "The rte CLI was not found next to RTE Studio, on PATH, or via RTE_CLI. "
            "Build it (target 'rte' lands in build/bin) or set the RTE_CLI environment "
            "variable to its full path.");
    }
    return {};
}

QString SimRunner::HostSimBaseSourceForGraph(const QString& graphPath) {
    std::error_code ec;
    auto hostSimAt = [&ec](std::filesystem::path dir) -> std::filesystem::path {
        for (int level = 0; level < 8 && !dir.empty(); ++level) {
            const std::filesystem::path candidate = dir / "Images" / "HostSim";
            if (std::filesystem::is_directory(candidate, ec)) {
                return candidate.lexically_normal();
            }
            dir = dir.parent_path();
        }
        return {};
    };

    if (!graphPath.isEmpty()) {
        const std::filesystem::path dir =
            std::filesystem::path(graphPath.toStdString()).parent_path();
        if (const auto found = hostSimAt(dir); !found.empty()) {
            return QString::fromStdString(found.string());
        }
    }

    // Installed layout: <prefix>/share/rte/Images/HostSim.
    std::filesystem::path dir(QCoreApplication::applicationDirPath().toStdString());
    for (int level = 0; level < 6 && !dir.empty(); ++level) {
        const std::filesystem::path candidate =
            dir / "share" / "rte" / "Images" / "HostSim";
        if (std::filesystem::is_directory(candidate, ec)) {
            return QString::fromStdString(candidate.lexically_normal().string());
        }
        dir = dir.parent_path();
    }

#ifdef RTE_PROJECT_ROOT
    const std::filesystem::path candidate =
        std::filesystem::path(RTE_PROJECT_ROOT) / "Images" / "HostSim";
    if (std::filesystem::is_directory(candidate, ec)) {
        return QString::fromStdString(candidate.lexically_normal().string());
    }
#endif
    return {};
}

QStringList SimRunner::ScenarioDirsForGraph(const QString& graphPath) {
    QStringList dirs;
    const QString base = HostSimBaseSourceForGraph(graphPath);
    if (!base.isEmpty()) {
        const QString scenarios = base + QStringLiteral("/scenarios");
        if (QDir(scenarios).exists()) {
            dirs << scenarios;
        }
    }
    // The emitted tree for this graph's stem may carry its own scenario
    // copies (rte places it next to the executable's build root).
    if (!graphPath.isEmpty()) {
        const QString stem = QFileInfo(graphPath).completeBaseName();
        std::filesystem::path appDir(QCoreApplication::applicationDirPath().toStdString());
        std::error_code ec;
        const std::filesystem::path buildRoot =
            appDir.filename() == "bin" ? appDir.parent_path() : appDir;
        const std::filesystem::path emitted =
            buildRoot / ("hostsim_" + stem.toStdString() + "_emitted") / "scenarios";
        if (std::filesystem::is_directory(emitted, ec)) {
            const QString path = QString::fromStdString(emitted.lexically_normal().string());
            if (!dirs.contains(path)) {
                dirs << path;
            }
        }
    }
    return dirs;
}

QStringList SimRunner::FindScenariosForGraph(const QString& graphPath) {
    QStringList files;
    for (const QString& dirPath : ScenarioDirsForGraph(graphPath)) {
        const QDir dir(dirPath);
        const QStringList names =
            dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
        for (const QString& name : names) {
            const QString absolute = dir.absoluteFilePath(name);
            if (!files.contains(absolute)) {
                files << absolute;
            }
        }
    }
    return files;
}

QString SimRunner::AutoScenarioForGraph(const QString& graphPath) {
    const QString base = HostSimBaseSourceForGraph(graphPath);
    if (base.isEmpty()) {
        return {};
    }
    QString stem = QFileInfo(graphPath).completeBaseName();
    constexpr char suffix[] = "_graph";
    if (stem.endsWith(QLatin1String(suffix))) {
        stem.chop(static_cast<int>(sizeof(suffix)) - 1);
    }
    const QString candidate =
        QStringLiteral("%1/scenarios/%2.json").arg(base, stem);
    if (QFileInfo(candidate).isFile()) {
        return candidate;
    }
    const QString fallback = base + QStringLiteral("/scenarios/default_motor.json");
    return QFileInfo(fallback).isFile() ? fallback : QString{};
}

bool SimRunner::Start(const SimRunRequest& request, QString* error) {
    if (IsRunning()) {
        if (error) {
            *error = QStringLiteral("a simulation is already running");
        }
        return false;
    }

    QString rteError;
    const QString rte = FindRteExecutable(&rteError);
    if (rte.isEmpty()) {
        if (error) {
            *error = rteError;
        }
        return false;
    }

    QStringList arguments{
        QStringLiteral("sim"),
        QStringLiteral("--graph"), request.graphPath,
        QStringLiteral("--live"),
    };
    if (!request.scenarioPath.isEmpty()) {
        arguments << QStringLiteral("--scenario") << request.scenarioPath;
    }

    QString program;
    const QStringList wrapped = WrapLineBuffered(rte, arguments, &program);

    announcedEndpoint_ = false;
    lineBuffer_.clear();
    process_->setWorkingDirectory(QFileInfo(request.graphPath).absolutePath());
#ifdef Q_OS_UNIX
    // Own session: rte forks host_sim without a group of its own, so the
    // group anchored at rte's PID covers both. Stop() signals that group.
    process_->setChildProcessModifier([] { ::setsid(); });
#endif
    emit output(QStringLiteral("$ %1 %2\n").arg(program, wrapped.join(u' ')));
    process_->start(program, wrapped);
    if (!process_->waitForStarted(5000)) {
        if (error) {
            *error = QStringLiteral("could not start %1: %2")
                         .arg(program, process_->errorString());
        }
        return false;
    }
    return true;
}

void SimRunner::Stop() {
    if (process_->state() == QProcess::NotRunning) {
        return;
    }
    emit output(QStringLiteral("[sim] stopping (SIGINT to the process group)...\n"));
#ifdef Q_OS_UNIX
    // Start() runs rte in its own session, so signalling its process group
    // reaches the host_sim grandchild too.
    const qint64 pid = process_->processId();
    if (pid > 0) {
        ::kill(static_cast<pid_t>(-pid), SIGINT);
    }
    QTimer::singleShot(kGracefulStopMs, this, [this, pid] {
        // A new run may have started inside the grace period after the old
        // process exited; only escalate against the process Stop() targeted.
        if (process_->state() == QProcess::NotRunning || process_->processId() != pid) {
            return;
        }
        emit output(QStringLiteral("[sim] still running after SIGINT; killing\n"));
        if (pid > 0) {
            ::kill(static_cast<pid_t>(-pid), SIGKILL);
        }
        process_->kill();
    });
#else
    process_->kill();
#endif
}

void SimRunner::Shutdown() {
    // No signals from this path: receivers of output()/finished() may already
    // be mid-destruction when the application exits.
    process_->disconnect(this);
    if (process_->state() == QProcess::NotRunning) {
        return;
    }
#ifdef Q_OS_UNIX
    const qint64 pid = process_->processId();
    if (pid > 0) {
        ::kill(static_cast<pid_t>(-pid), SIGINT);
    }
    if (!process_->waitForFinished(kGracefulStopMs)) {
        if (pid > 0 && process_->processId() == pid) {
            ::kill(static_cast<pid_t>(-pid), SIGKILL);
        }
        process_->kill();
        process_->waitForFinished(kGracefulStopMs);
    }
#else
    process_->kill();
    process_->waitForFinished(kGracefulStopMs);
#endif
}

void SimRunner::HandleReadyRead() {
    const QByteArray chunk = process_->readAllStandardOutput();
    if (chunk.isEmpty()) {
        return;
    }
    const QString text = QString::fromLocal8Bit(chunk);
    if (!announcedEndpoint_) {
        // Scan complete lines only and hold back the unterminated tail, so a
        // listening announcement split across two chunks is still matched.
        lineBuffer_ += text;
        qsizetype start = 0;
        for (;;) {
            const qsizetype newline = lineBuffer_.indexOf(u'\n', start);
            if (newline < 0) {
                break;
            }
            AttachLiveEndpoint(lineBuffer_.mid(start, newline - start));
            start = newline + 1;
        }
        lineBuffer_.remove(0, start);
        if (announcedEndpoint_) {
            lineBuffer_.clear();
        } else if (lineBuffer_.size() > 4096) {
            // A pathological line with no newline cannot be the announcement;
            // do not let it grow the buffer without bound.
            lineBuffer_.clear();
        }
    }
    emit output(text);
}

void SimRunner::AttachLiveEndpoint(const QString& line) {
    if (announcedEndpoint_) {
        return;
    }
    static const QRegularExpression pattern(
        QStringLiteral("HostSim live: listening on ([\\w.\\-]+):(\\d+)"));
    const QRegularExpressionMatch match = pattern.match(line);
    if (!match.hasMatch()) {
        return;
    }
    announcedEndpoint_ = true;
    emit liveEndpoint(match.captured(1), match.captured(2).toInt());
}

}  // namespace NodeGUI::simulation
