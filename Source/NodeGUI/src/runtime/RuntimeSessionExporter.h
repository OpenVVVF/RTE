#pragma once

#include "TelemetryStore.h"

#include <QString>

namespace NodeGUI::runtime {

struct RuntimeSessionMetadata {
    QString port;
    QString mode;
    QString protocol;
};

// Writes one complete runtime session as an atomic, chronological JSONL event
// stream. Returns false and fills error when the destination cannot be written.
bool ExportRuntimeSession(const QString& path,
                          const RuntimeSessionSnapshot& session,
                          const RuntimeSessionMetadata& metadata,
                          QString& error);

}  // namespace NodeGUI::runtime
