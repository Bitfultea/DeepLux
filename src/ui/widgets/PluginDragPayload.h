#pragma once

#include <QDataStream>
#include <QMap>
#include <QMimeData>
#include <QVariant>

namespace DeepLux::PluginDragPayload {

inline QString pluginName(const QMimeData* mimeData) {
    static constexpr char kItemModelData[] = "application/x-qabstractitemmodeldatalist";

    if (!mimeData || !mimeData->hasFormat(kItemModelData)) {
        return {};
    }

    QDataStream stream(mimeData->data(kItemModelData));
    while (!stream.atEnd()) {
        int row = -1;
        int column = -1;
        QMap<int, QVariant> roleData;
        stream >> row >> column >> roleData;
        if (stream.status() != QDataStream::Ok) {
            return {};
        }

        if (column != 0 || roleData.value(Qt::UserRole).toString() != QStringLiteral("plugin")) {
            continue;
        }

        const QString id = roleData.value(Qt::UserRole + 1).toString().trimmed();
        if (!id.isEmpty()) {
            return id;
        }
    }

    return {};
}

} // namespace DeepLux::PluginDragPayload
