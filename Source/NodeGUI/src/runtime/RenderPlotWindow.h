#pragma once

#include "TelemetryStore.h"

#include <QDialog>
#include <QString>

#include <vector>

class QLabel;

namespace NodeGUI::runtime {

class SignalPlotWidget;

// Static full-resolution view of a HostSim SPICE render burst, loaded from
// the render CSV recorded by the "render" console command. Owns a private
// TelemetryStore filled from the CSV and reuses the GPU plot widget with a
// frozen time axis spanning the whole burst, so the result stays inspectable
// (cursor readout, rubber-band zoom) while the live view keeps scrolling.
class RenderPlotWindow : public QDialog {
    Q_OBJECT

public:
    explicit RenderPlotWindow(const QString& csvPath, QWidget* parent = nullptr);

private:
    // Fills store_ from the CSV. Returns false and sets error on failure.
    bool LoadCsv(const QString& csvPath, QString& error);

    TelemetryStore store_;
    std::vector<SignalPlotWidget*> plots_;
};

}  // namespace NodeGUI::runtime
