#include "SaveDataPlugin.h"

#include "agent/AgentFileOutputAuthorization.h"
#include "common/Logger.h"

#include <QComboBox>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QTextStream>
#include <QVBoxLayout>

namespace DeepLux {

SaveDataPlugin::SaveDataPlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{{"filePath", ""}, {"fileFormat", "json"}, {"appendMode", false}};
    m_params = m_defaultParams;
}

SaveDataPlugin::~SaveDataPlugin() {}

bool SaveDataPlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "SaveDataPlugin initialized";
    return true;
}

void SaveDataPlugin::shutdown() {
    ModuleBase::shutdown();
}

ExecutionResult SaveDataPlugin::execute(const PortValueMap& inputs, PortValueMap& outputs, ExecutionContext& context) {
    m_writeUnconfirmed = false;
    m_writeDiagnostics.clear();
    m_saveResult = false;
    const QString authorizationError = agentFileOutputAuthorizationError(
        property(AgentFileOutputAuthorizationProperty).toJsonObject(), instanceName(), moduleId(), currentParams());
    if (!authorizationError.isEmpty())
        return ExecutionResult::fail(ExecError::ConfirmedTargetChanged, tr("已批准的数据输出配置或目标已变化"),
                                     authorizationError);
    const ExecutionResult result = ModuleBase::execute(inputs, outputs, context);
    if (!result.success && m_writeUnconfirmed) {
        return ExecutionResult::fail(ExecError::SideEffectUncertain, result.userMessage, m_writeDiagnostics);
    }
    return result;
}

bool SaveDataPlugin::process(const ImageData& input, ImageData& output) {
    Q_UNUSED(output);
    output = input;

    QJsonObject params = currentParams();
    QString filePath = params["filePath"].toString();
    if (filePath.isEmpty()) {
        filePath = input.data("file_path").toString();
    }

    if (filePath.isEmpty()) {
        emit errorOccurred(tr("未提供文件路径"));
        return false;
    }

    QString format = params["fileFormat"].toString();
    bool appendMode = params["appendMode"].toBool();
    m_saveResult = false;

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
        if (file.open(mode)) {
            m_writeUnconfirmed = true;
            QTextStream out(&file);
            for (auto it = dataMap.constBegin(); it != dataMap.constEnd(); ++it) {
                out << it.key() << ": " << it.value().toString() << "\n";
            }
            m_saveResult = finishTextWrite(file, out, QStringLiteral("text"), dataMap);
        }
    }

    output.setData("save_result", m_saveResult);
    output.setData("save_path", filePath);

    if (!m_saveResult) {
        emit errorOccurred(QString("保存数据失败: %1").arg(filePath));
    }

    Logger::instance().debug(QString("保存数据: %1, 结果: %2").arg(filePath).arg(m_saveResult ? "成功" : "失败"),
                             "SaveData");

    return m_saveResult;
}

bool SaveDataPlugin::saveToJson(const QString& filePath, const QVariantMap& data) {
    QFile file(filePath);

    // 检查是否追加模式
    QJsonObject params = currentParams();
    QJsonObject jsonObj;
    if (params["appendMode"].toBool() && file.exists()) {
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
    if (file.open(QIODevice::WriteOnly)) {
        const QByteArray payload = QJsonDocument(jsonObj).toJson(QJsonDocument::Indented);
        m_writeUnconfirmed = true;
        const qint64 bytesWritten = file.write(payload);
        const bool flushOk = file.flush();
        file.close();
        const QFileDevice::FileError fileError = file.error();
        if (bytesWritten == payload.size() && flushOk && fileError == QFileDevice::NoError) {
            m_writeUnconfirmed = false;
            return true;
        }
        m_writeDiagnostics = QStringLiteral("fileFormat=json bytesWritten=%1 expectedBytes=%2 flushOk=%3 fileError=%4")
                                 .arg(bytesWritten)
                                 .arg(payload.size())
                                 .arg(flushOk ? QStringLiteral("true") : QStringLiteral("false"))
                                 .arg(static_cast<int>(fileError));
    }

    return false;
}

bool SaveDataPlugin::saveToCsv(const QString& filePath, const QVariantMap& data) {
    QFile file(filePath);
    QJsonObject params = currentParams();
    QIODevice::OpenMode mode = params["appendMode"].toBool() ? QIODevice::Append : QIODevice::WriteOnly;

    if (file.open(mode)) {
        m_writeUnconfirmed = true;
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
        return finishTextWrite(file, out, QStringLiteral("csv"), data);
    }

    return false;
}

bool SaveDataPlugin::finishTextWrite(QFile& file, QTextStream& stream, const QString& format, const QVariantMap& data) {
    stream.flush();
    const QTextStream::Status streamStatus = stream.status();
    const bool flushOk = file.flush();
    const QFileDevice::FileError fileErrorBeforeClose = file.error();
    file.close();
    const QFileDevice::FileError fileErrorAfterClose = file.error();
    if (streamStatus == QTextStream::Ok && flushOk && fileErrorBeforeClose == QFileDevice::NoError &&
        fileErrorAfterClose == QFileDevice::NoError) {
        m_writeUnconfirmed = false;
        return true;
    }

    QStringList keys = data.keys();
    keys.sort();
    m_writeDiagnostics = QStringLiteral("fileFormat=%1 writeAttempted=true dataKeys=%2 streamStatus=%3 flushOk=%4 "
                                        "fileErrorBeforeClose=%5 fileErrorAfterClose=%6")
                             .arg(format, keys.join(QLatin1Char(',')))
                             .arg(static_cast<int>(streamStatus))
                             .arg(flushOk ? QStringLiteral("true") : QStringLiteral("false"))
                             .arg(static_cast<int>(fileErrorBeforeClose))
                             .arg(static_cast<int>(fileErrorAfterClose));
    return false;
}

bool SaveDataPlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    const QString fileFormat = params["fileFormat"].toString("json");
    if (fileFormat != "json" && fileFormat != "csv" && fileFormat != "text") {
        error = tr("文件格式无效");
        return false;
    }

    error.clear();
    return true;
}

QWidget* SaveDataPlugin::createConfigWidget() {
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
    if (idx >= 0)
        formatCombo->setCurrentIndex(idx);
    layout->addWidget(formatCombo);

    layout->addStretch();

    connect(pathEdit, &QLineEdit::textChanged, this, [this](const QString& text) { setParam("filePath", text); });

    connect(formatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, formatCombo](int) { setParam("fileFormat", formatCombo->currentData().toString()); });

    return widget;
}

IModule* SaveDataPlugin::cloneImpl() const {
    SaveDataPlugin* clone = new SaveDataPlugin();
    clone->setParams(currentParams());
    return clone;
}

} // namespace DeepLux
