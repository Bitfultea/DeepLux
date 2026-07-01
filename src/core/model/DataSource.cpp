#include "DataSource.h"

namespace DeepLux {

QJsonObject DataSource::toJson() const
{
    QJsonObject json;
    json["id"] = id;
    json["name"] = name;
    json["filePath"] = filePath;
    json["type"] = type;

    QJsonObject metaJson;
    for (auto it = metadata.begin(); it != metadata.end(); ++it) {
        metaJson[it.key()] = QJsonValue::fromVariant(it.value());
    }
    json["metadata"] = metaJson;
    json["importTime"] = importTime;
    return json;
}

DataSource DataSource::fromJson(const QJsonObject& json)
{
    DataSource ds;
    ds.id = json["id"].toString();
    ds.name = json["name"].toString();
    ds.filePath = json["filePath"].toString();
    ds.type = json["type"].toString();

    QJsonObject metaJson = json["metadata"].toObject();
    for (auto it = metaJson.begin(); it != metaJson.end(); ++it) {
        ds.metadata[it.key()] = it.value().toVariant();
    }
    ds.importTime = json["importTime"].toVariant().toLongLong();
    return ds;
}

} // namespace DeepLux
