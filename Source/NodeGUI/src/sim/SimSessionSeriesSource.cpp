#include "SimSessionSeriesSource.h"

#include "runtime/TelemetryStore.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace NodeGUI::sim {

bool SimSessionSeriesSource::Load(const QString& path, QString& error) {
    if (!session_.Load(path, error)) {
        names_.clear();
        return false;
    }
    names_.clear();
    for (const QString& name : session_.ChannelNames()) {
        if (session_.ChannelStats(name).finiteSamples > 0) {
            names_.push_back(name);
        }
    }
    return true;
}

double SimSessionSeriesSource::SpanSeconds() const {
    double span = 0.0;
    std::vector<float> t;
    std::vector<float> y;
    for (const QString& name : names_) {
        if (session_.ChannelHistory(name, t, y) && !t.empty()) {
            span = std::max(span, static_cast<double>(t.back() - t.front()));
        }
    }
    return span;
}

void SimSessionSeriesSource::RegisterWith(
    NodeGUI::runtime::TelemetryStore& store) const {
    std::unordered_map<std::string, NodeGUI::runtime::OverlayChannelHistory>
        channels;
    channels.reserve(static_cast<std::size_t>(names_.size()));
    for (const QString& name : names_) {
        std::vector<float> t;
        std::vector<float> y;
        if (!session_.ChannelHistory(name, t, y)) {
            continue;
        }
        // Drop null (NaN) samples exactly like PopulateStore does for the
        // primary path: overlay curves and overlay ValueAt only ever see
        // finite points.
        NodeGUI::runtime::OverlayChannelHistory history;
        history.t.reserve(t.size());
        history.y.reserve(y.size());
        for (std::size_t i = 0; i < t.size() && i < y.size(); ++i) {
            if (std::isfinite(y[i])) {
                history.t.push_back(t[i]);
                history.y.push_back(y[i]);
            }
        }
        if (!history.t.empty()) {
            channels.emplace(name.toStdString(), std::move(history));
        }
    }
    store.SetOverlayChannels(std::move(channels));
}

}  // namespace NodeGUI::sim
