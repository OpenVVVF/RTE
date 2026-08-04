#pragma once

#include "RuntimeController.h"

#include <QObject>
#include <QString>

namespace NodeGUI::runtime {

// Reads/writes FRAM-backed RteParamStore keys using the firmware's existing
// `config` shell commands.  Lives on the GUI thread and blocks briefly while
// waiting for command responses.
class FramKeysManager : public QObject {
    Q_OBJECT

public:
    explicit FramKeysManager(RuntimeController* controller,
                             QObject* parent = nullptr);

    // Read all keys from FRAM and save them as a JSON object to `path`.
    // Returns true on success and sets `error` otherwise.
    bool SaveToFile(const QString& path, QString& error);

    // Load keys from the JSON object at `path` and write them to FRAM.
    // If `clearFirst` is true, deletes all FRAM keys before loading.
    bool LoadFromFile(const QString& path, bool clearFirst, QString& error);

private:
    RuntimeController* controller_;
};

}  // namespace NodeGUI::runtime
