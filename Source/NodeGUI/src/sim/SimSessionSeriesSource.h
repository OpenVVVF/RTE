#pragma once

#include "SimSession.h"

#include <QString>
#include <QStringList>

namespace NodeGUI::runtime {
class TelemetryStore;
}

namespace NodeGUI::sim {

// Recorded-session-to-overlay bridge (backlog P1-8): parses one v3 jsonl
// session through SimSession and registers its finite float channels as an
// overlay set in a runtime::TelemetryStore, so the plot leaf can draw the
// reference alongside live/loaded data without the leaf ever learning jsonl.
//
// One source instance = one overlay set. The source keeps the parsed session
// so the owning surface can report channel count / span without re-reading
// the file; the store takes its own copy of the (compact, NaN-stripped)
// channel vectors — a second 63-channel x ~7.4k float set, bounded and billed
// deliberately against the overlay feature, never through the live ring.
class SimSessionSeriesSource {
public:
    // Same tolerance rules as SimSession::Load: unreadable/empty files fail,
    // unparseable records are skipped. All-null channels are dropped from the
    // overlay set (nothing drawable to register).
    bool Load(const QString& path, QString& error);

    QStringList ChannelNames() const;  // registered (finite) channel names
    double SpanSeconds() const;        // longest channel span in the set
    int ChannelCount() const { return static_cast<int>(names_.size()); }

    // Replaces the store's overlay set with this session's channels (offset
    // preserved — the store owns it; the surface re-pushes after a swap).
    void RegisterWith(NodeGUI::runtime::TelemetryStore& store) const;

private:
    SimSession session_;
    QStringList names_;  // finite channels, sorted
};

}  // namespace NodeGUI::sim
