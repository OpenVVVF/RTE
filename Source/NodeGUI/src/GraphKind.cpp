#include "GraphKind.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace NodeGUI {

GraphKind GraphKindOf(const std::string& path) {
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::ReadOnly)) {
        return GraphKind::Unknown;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        return GraphKind::Unknown;
    }
    const QJsonArray nodes = document.object().value(QStringLiteral("nodes")).toArray();
    for (const QJsonValue& value : nodes) {
        const QString type = value.toObject().value(QStringLiteral("type")).toString();
        if (type.startsWith(QStringLiteral("Plant."))) {
            return GraphKind::Plant;
        }
    }
    return GraphKind::Controller;
}

}  // namespace NodeGUI
