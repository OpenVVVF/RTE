#include "RenderPlotWindow.h"

#include "SignalPlotWidget.h"

#include <QFile>
#include <QLabel>
#include <QSplitter>
#include <QVBoxLayout>

#include <tuple>

namespace NodeGUI::runtime {

namespace {

// Keep each signal under TelemetryStore::kMaxSamples after decimation.
constexpr qint64 kMaxLoadRows = 100000;

}  // namespace

RenderPlotWindow::RenderPlotWindow(const QString& csvPath, QWidget* parent)
    : QDialog(parent) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("SPICE Render — %1").arg(csvPath));
    resize(1000, 720);

    auto* layout = new QVBoxLayout(this);

    QString error;
    if (!LoadCsv(csvPath, error)) {
        auto* label = new QLabel(QStringLiteral("Failed to load render trace:\n%1").arg(error), this);
        label->setWordWrap(true);
        layout->addWidget(label, 1);
        return;
    }

    double t0 = 0.0;
    double t1 = 0.0;
    std::vector<float> tmpT, tmpY;
    if (store_.CopyHistoryInto("i_a", tmpT, tmpY) && !tmpT.empty()) {
        t0 = tmpT.front();
        t1 = tmpT.back();
    }
    const double burst = std::max(0.01, t1 - t0);

    auto* info = new QLabel(
        QStringLiteral("%1   |   %2 s window @ %3 rows — drag to zoom, double-click to reset")
            .arg(csvPath)
            .arg(burst, 0, 'f', 4)
            .arg(tmpT.size()),
        this);
    layout->addWidget(info);

    const QColor accents[] = {
        QColor(255, 176, 77),
        QColor(102, 204, 255),
        QColor(140, 235, 120),
    };
    const QStringList signalSets[] = {
        {QStringLiteral("i_a"), QStringLiteral("i_b"), QStringLiteral("i_c")},
        {QStringLiteral("duty_u"), QStringLiteral("duty_v"), QStringLiteral("duty_w")},
        {QStringLiteral("theta_e"), QStringLiteral("omega_e")},
    };
    const QString titles[] = {
        QStringLiteral("Phase currents"),
        QStringLiteral("Duties"),
        QStringLiteral("Angle / speed"),
    };

    auto* split = new QSplitter(Qt::Vertical, this);
    for (int i = 0; i < 3; ++i) {
        auto* plot = new SignalPlotWidget(titles[i], split);
        plot->SetStore(&store_);
        plot->SetAccentColor(accents[i]);
        plot->SetSignals(signalSets[i]);
        plot->SetViewSeconds(burst);
        // Frozen axis anchored so the window covers the whole burst.
        plot->SetTimeFrozen(true, t0 + burst);
        plot->Refresh();
        split->addWidget(plot);
        plots_.push_back(plot);
    }
    split->setSizes({240, 240, 240});
    layout->addWidget(split, 1);
}

bool RenderPlotWindow::LoadCsv(const QString& csvPath, QString& error) {
    QFile file(csvPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        error = QStringLiteral("cannot open %1").arg(csvPath);
        return false;
    }

    const QString header = QString::fromUtf8(file.readLine()).trimmed();
    const QStringList columns = header.split(u',', Qt::SkipEmptyParts);
    if (columns.size() < 2 || columns[0] != QLatin1String("time_us")) {
        error = QStringLiteral("%1 is not a HostSim render trace (bad header)")
                    .arg(csvPath);
        return false;
    }

    // Count rows first so an oversized render can be decimated to stay under
    // the store's per-signal sample cap.
    qint64 rows = 0;
    while (!file.atEnd()) {
        if (!file.readLine().trimmed().isEmpty()) {
            ++rows;
        }
    }
    if (rows == 0) {
        error = QStringLiteral("%1 has no data rows (render still running?)").arg(csvPath);
        return false;
    }
    const qint64 stride = (rows + kMaxLoadRows - 1) / kMaxLoadRows;
    file.seek(0);
    file.readLine();  // skip header

    std::vector<std::tuple<std::string, float, float>> batch;
    batch.reserve(static_cast<std::size_t>(std::min(rows, kMaxLoadRows)) *
                  static_cast<std::size_t>(columns.size() - 1));

    qint64 row = 0;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if ((row++ % stride) != 0) {
            continue;
        }
        const QStringList fields = line.split(u',');
        if (fields.size() != columns.size()) {
            continue;
        }
        bool ok = false;
        const float t = static_cast<float>(fields[0].toDouble(&ok) * 1.0e-6);
        if (!ok) {
            continue;
        }
        for (int c = 1; c < fields.size(); ++c) {
            const float v = fields[c].toFloat(&ok);
            if (ok) {
                batch.emplace_back(columns[c].toStdString(), v, t);
            }
        }
    }

    if (batch.empty()) {
        error = QStringLiteral("%1 has no parseable samples").arg(csvPath);
        return false;
    }
    store_.AddF32Batch(batch);
    return true;
}

}  // namespace NodeGUI::runtime
