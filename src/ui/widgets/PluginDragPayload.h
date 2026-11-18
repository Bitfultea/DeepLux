#pragma once

#include <QDataStream>
#include <QMap>
#include <QMimeData>
#include <QVariant>

namespace DeepLux::PluginDragPayload {

inline constexpr char kPluginIdMimeType[] = "application/x-deeplux-plugin-id-v1";

inline void setPluginId(QMimeData* mimeData, const QString& pluginId) {
    if (mimeData && !pluginId.trimmed().isEmpty()) {
        mimeData->setData(kPluginIdMimeType, pluginId.trimmed().toUtf8());
    }
}

inline QString pluginId(const QMimeData* mimeData) {
    if (!mimeData || !mimeData->hasFormat(kPluginIdMimeType)) {
        return {};
    }
    const QByteArray data = mimeData->data(kPluginIdMimeType);
    const QString id = QString::fromUtf8(data).trimmed();
    return id.toUtf8() == data ? id : QString();
}

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

inline QString flowInstanceId(const QMimeData* mimeData) {
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

        if (column != 0 || roleData.value(Qt::UserRole).toString() != QStringLiteral("flow_item")) {
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
