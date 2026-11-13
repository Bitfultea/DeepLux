#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QStringList>

namespace ProjectJsonTestHelpers {

inline QString describe(const QJsonValue& value) {
    const QByteArray wrapped = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(wrapped.mid(1, wrapped.size() - 2));
}

inline void collectDifferences(const QJsonValue& expected, const QJsonValue& actual, const QString& path,
                               QStringList* differences, int limit = 8) {
    if (differences->size() >= limit)
        return;
    if (expected.type() != actual.type()) {
        differences->append(
            QStringLiteral("%1: expected %2, actual %3").arg(path, describe(expected), describe(actual)));
        return;
    }
    if (expected.isObject()) {
        const QJsonObject expectedObject = expected.toObject();
        const QJsonObject actualObject = actual.toObject();
        QStringList keys;
        for (auto it = expectedObject.constBegin(); it != expectedObject.constEnd(); ++it)
            keys.append(it.key());
        for (auto it = actualObject.constBegin(); it != actualObject.constEnd(); ++it) {
            if (!expectedObject.contains(it.key()))
                keys.append(it.key());
        }
        keys.removeDuplicates();
        keys.sort();
        for (const QString& key : keys) {
            if (!expectedObject.contains(key) || !actualObject.contains(key)) {
                differences->append(QStringLiteral("%1.%2: expected %3, actual %4")
                                        .arg(path, key,
                                             expectedObject.contains(key) ? describe(expectedObject.value(key))
                                                                          : QStringLiteral("<missing>"),
                                             actualObject.contains(key) ? describe(actualObject.value(key))
                                                                        : QStringLiteral("<missing>")));
            } else {
                collectDifferences(expectedObject.value(key), actualObject.value(key), path + QLatin1Char('.') + key,
                                   differences, limit);
            }
            if (differences->size() >= limit)
                return;
        }
        return;
    }
    if (expected.isArray()) {
        const QJsonArray expectedArray = expected.toArray();
        const QJsonArray actualArray = actual.toArray();
        if (expectedArray.size() != actualArray.size())
            differences->append(QStringLiteral("%1.length: expected %2, actual %3")
                                    .arg(path)
                                    .arg(expectedArray.size())
                                    .arg(actualArray.size()));
        const int commonSize = qMin(expectedArray.size(), actualArray.size());
        for (int i = 0; i < commonSize && differences->size() < limit; ++i)
            collectDifferences(expectedArray.at(i), actualArray.at(i), path + QStringLiteral("[%1]").arg(i),
                               differences, limit);
        return;
    }
    if (expected != actual)
        differences->append(
            QStringLiteral("%1: expected %2, actual %3").arg(path, describe(expected), describe(actual)));
}

inline QStringList differences(const QJsonObject& expected, const QJsonObject& actual, int limit = 8) {
    QStringList result;
    collectDifferences(expected, actual, QStringLiteral("$"), &result, limit);
    return result;
}

inline QJsonObject withoutModifiedTimestamp(QJsonObject json) {
    json.remove(QStringLiteral("modified"));
    return json;
}

} // namespace ProjectJsonTestHelpers
