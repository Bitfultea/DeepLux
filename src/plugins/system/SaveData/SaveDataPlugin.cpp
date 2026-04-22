#include "SaveDataPlugin.h"
#include "common/Logger.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QLineEdit>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

namespace DeepLux {

SaveDataPlugin::SaveDataPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"filePath", ""},
        {"fileFormat", "json"},
        {"appendMode", false}
    };
    m_params = m_defaultParams;
}

SaveDataPlugin::~SaveDataPlugin()
{
}

bool SaveDataPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "SaveDataPlugin initialized";
    return true;
}

void SaveDataPlugin::shutdown()
{
    ModuleBase::shutdown();
}

bool SaveDataPlugin::process(const ImageData& input, ImageData& output)
{
    Q_UNUSED(output);
    output = input;

    QString filePath = m_params["filePath"].toString();
    if (filePath.isEmpty()) {
        filePath = input.data("file_path").toString();
    }

    if (filePath.isEmpty()) {
        emit errorOccurred(tr("未提供文件路径"));
        return false;
    }

    QString format = m_params["fileFormat"].toString();
    bool appendMode = m_params["appendMode"].toBool();

    // 收集数据
    QVariantMap dataMap;
    QStringList keys = input.allData().keys();
    for (const QString& key : keys) {
        dataMap[key] = input.data(key);
    }

    // 保存数据
    if (format == "json") {
        m_saveResult = saveToJson(filePath, dataMap);
    } else if (format == "csv") {
        m_saveResult = saveToCsv(filePath, dataMap);
    } else {
        // 文本格式
        QFile file(filePath);
        QIODevice::OpenMode mode = appendMode ? QIODevice::Append : QIODevice::WriteOnly;
        if (file.open(mode | QIODevice::Text)) {
            QTextStream out(&file);
            for (auto it = dataMap.constBegin(); it != dataMap.constEnd(); ++it) {
                out << it.key() << ": " << it.value().toString() << "\n";
            }
            file.close();
            m_saveResult = true;
        }
    }

    output.setData("save_result", m_saveResult);
    output.setData("save_path", filePath);

    if (!m_saveResult) {
        emit errorOccurred(QString("保存数据失败: %1").arg(filePath));
    }

    Logger::instance().debug(QString("保存数据: %1, 结果: %2").arg(filePath).arg(m_saveResult ? "成功" : "失败"), "SaveData");

    return true;
}

bool SaveDataPlugin::saveToJson(const QString& filePath, const QVariantMap& data)
{
    QFile file(filePath);
    QIODevice::OpenMode mode = QIODevice::WriteOnly;

    // 检查是否追加模式
    QJsonObject jsonObj;
    if (m_params["appendMode"].toBool() && file.exists()) {
        if (file.open(QIODevice::ReadOnly)) {
            QByteArray content = file.readAll();
            file.close();
            QJsonDocument doc = QJsonDocument::fromJson(content);
            if (doc.isObject()) {
                jsonObj = doc.object();
            }
        }
    }

    // 添加新数据
    for (auto it = data.constBegin(); it != data.constEnd(); ++it) {
        jsonObj[it.key()] = QJsonValue::fromVariant(it.value());
    }

    // 写入文件
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        file.write(QJsonDocument(jsonObj).toJson(QJsonDocument::Indented));
        file.close();
        return true;
    }

    return false;
}

bool SaveDataPlugin::saveToCsv(const QString& filePath, const QVariantMap& data)
{
    QFile file(filePath);
    QIODevice::OpenMode mode = m_params["appendMode"].toBool() ? QIODevice::Append : QIODevice::WriteOnly;

    if (file.open(mode | QIODevice::Text)) {
        QTextStream out(&file);

        // 如果是追加模式，先检查文件是否存在内容
        if (mode == QIODevice::Append && file.size() > 0) {
            // CSV追加模式，只添加值
        } else {
            // 写入表头
            QStringList keys = data.keys();
            out << keys.join(",") << "\n";
        }

        // 写入值
        QStringList values;
        for (const QString& key : data.keys()) {
            values << data[key].toString().replace(",", ";");
        }
        out << values.join(",") << "\n";

        file.close();
        return true;
    }

    return false;
}

bool SaveDataPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    QString filePath = params["filePath"].toString();
    if (filePath.isEmpty()) {
        error = tr("文件路径不能为空");
        return false;
    }
    return true;
}

QWidget* SaveDataPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(tr("文件路径:")));
    QLineEdit* pathEdit = new QLineEdit(m_params["filePath"].toString());
    layout->addWidget(pathEdit);

    layout->addWidget(new QLabel(tr("文件格式:")));
    QComboBox* formatCombo = new QComboBox();
    formatCombo->addItem("JSON", "json");
    formatCombo->addItem("CSV", "csv");
    formatCombo->addItem("文本", "text");

    int idx = formatCombo->findData(m_params["fileFormat"].toString());
    if (idx >= 0) formatCombo->setCurrentIndex(idx);
    layout->addWidget(formatCombo);

    layout->addStretch();

    connect(pathEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_params["filePath"] = text;
    });

    connect(formatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, formatCombo](int) {
        m_params["fileFormat"] = formatCombo->currentData().toString();
    });

    return widget;
}

IModule* SaveDataPlugin::cloneImpl() const
{
    SaveDataPlugin* clone = new SaveDataPlugin();
    return clone;
}

} // namespace DeepLux
