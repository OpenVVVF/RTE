#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

namespace NodeGUI::simulation {

// Default IVP telemetry endpoint of HostSim --live (matches run_spwm_live.sh
// and rte's sim backend).
inline constexpr char kDefaultLiveHost[] = "127.0.0.1";
inline constexpr int kDefaultLivePort = 14608;

struct SimRunRequest {
    QString graphPath;      // absolute path to the saved .json node graph
    QString scenarioPath;   // empty = rte's automatic scenario selection
};

// Owns the `rte sim` child process for "Build & Run Simulation": emits the
// graph into the HostSim image, builds host_sim, and runs it in --live
// (foreground) mode. Non-UI: output is streamed line-buffered through the
// output() signal.
//
// The actual simulator (host_sim) is a grandchild process forked by rte; on
// Unix the child is placed in its own session so Stop() can signal the whole
// process group (rte AND host_sim) instead of orphaning the simulator.
class SimRunner : public QObject {
    Q_OBJECT

public:
    explicit SimRunner(QObject* parent = nullptr);
    ~SimRunner() override;

    bool IsRunning() const;

    // Resolves the rte CLI: $RTE_CLI, then the sibling of the running app
    // (build/bin layout), then the build-time development path, then PATH.
    // Returns an empty string and fills *error when nothing usable is found.
    static QString FindRteExecutable(QString* error);

    // The HostSim base image (contains scenarios/) for a graph file: walks up
    // from the graph, then from the application directory (installed layouts
    // use <prefix>/share/rte/Images/HostSim), then the compile-time project
    // root. Empty when not found.
    static QString HostSimBaseSourceForGraph(const QString& graphPath);

    // Directories scanned for scenarios: <base>/scenarios plus the emitted
    // tree for this graph's stem (build/hostsim_<stem>_emitted/scenarios).
    static QStringList ScenarioDirsForGraph(const QString& graphPath);
    static QStringList FindScenariosForGraph(const QString& graphPath);

    // Mirrors rte's rule: graph stem (minus a trailing _graph) + .json under
    // <base>/scenarios, else <base>/scenarios/default_motor.json.
    static QString AutoScenarioForGraph(const QString& graphPath);

    // Starts `rte sim --graph <graph> [--scenario <file>] --live`. On error
    // returns false and fills *error. Streams combined stdout/stderr via
    // output() once started.
    bool Start(const SimRunRequest& request, QString* error);

    // Graceful stop: SIGINT to the child's process group (Ctrl+C semantics),
    // escalating to SIGKILL after a grace period. finished() still fires.
    void Stop();

    // Blocking teardown for application shutdown: SIGINT, then SIGKILL after
    // the grace period, waiting for the process each time. Emits no signals
    // (the process is disconnected first), so it is safe to call while the
    // owning window is being destroyed.
    void Shutdown();

signals:
    void output(const QString& text);
    // Parsed from host_sim's "HostSim live: listening on <host>:<port>" line.
    void liveEndpoint(const QString& host, int port);
    void finished(int exitCode, QProcess::ExitStatus status);

private:
    void HandleReadyRead();
    void AttachLiveEndpoint(const QString& line);

    QProcess* process_ = nullptr;
    bool announcedEndpoint_ = false;
    // Unterminated output tail kept between chunks so a "listening on" line
    // split across reads is still matched.
    QString lineBuffer_;
};

}  // namespace NodeGUI::simulation
