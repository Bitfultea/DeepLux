#include "WriteTextPlugin.h"

#include "agent/AgentFileOutputAuthorization.h"
#include "common/Logger.h"

#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QTextEdit>
#include <QVBoxLayout>

namespace DeepLux {

WriteTextPlugin::WriteTextPlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{{"filePath", ""}, {"textContent", ""}, {"appendMode", false}};
    m_params = m_defaultParams;
}

WriteTextPlugin::~WriteTextPlugin() {}

bool WriteTextPlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "WriteTextPlugin initialized";
    return true;
}

void WriteTextPlugin::shutdown() {
    ModuleBase::shutdown();
}

ExecutionResult WriteTextPlugin::execute(const PortValueMap& inputs, PortValueMap& outputs, ExecutionContext& context) {
    m_writeUnconfirmed = false;
    m_writeDiagnostics.clear();
    m_writeResult = false;
    const QString authorizationError = agentFileOutputAuthorizationError(
        property(AgentFileOutputAuthorizationProperty).toJsonObject(), instanceName(), moduleId(), currentParams());
    if (!authorizationError.isEmpty())
        return ExecutionResult::fail(ExecError::ConfirmedTargetChanged, tr("已批准的文件输出配置或目标已变化"),
                                     authorizationError);
    const ExecutionResult result = ModuleBase::execute(inputs, outputs, context);
    if (!result.success && m_writeUnconfirmed)
        return ExecutionResult::fail(ExecError::SideEffectUncertain, result.userMessage, m_writeDiagnostics);
    return result;
}

bool WriteTextPlugin::process(const ImageData& input, ImageData& output) {
    Q_UNUSED(output);
    output = input;

    QJsonObject params = currentParams();
    QString filePath = params["filePath"].toString();
    if (filePath.isEmpty()) {
        filePath = input.data("file_path").toString();
    }

    QString textContent = params["textContent"].toString();
    if (textContent.isEmpty()) {
        textContent = input.data("text_content").toString();
    }

    bool appendMode = params["appendMode"].toBool();

    if (filePath.isEmpty()) {
        emit errorOccurred(tr("未提供文件路径"));
        return false;
    }

    QFile file(filePath);
    QIODevice::OpenMode mode = appendMode ? QIODevice::Append : QIODevice::WriteOnly;

    if (file.open(mode)) {
        m_writeUnconfirmed = true;
        QTextStream out(&file);
        out << textContent;
        if (!textContent.endsWith("\n")) {
            out << "\n";
        }
        out.flush();
        const QTextStream::Status streamStatus = out.status();
        const bool flushOk = file.flush();
        const QFileDevice::FileError fileErrorBeforeClose = file.error();
        file.close();
        const QFileDevice::FileError fileErrorAfterClose = file.error();
        const QFileDevice::FileError fileError =
            fileErrorBeforeClose != QFileDevice::NoError ? fileErrorBeforeClose : fileErrorAfterClose;
        m_writeResult = streamStatus == QTextStream::Ok && flushOk && fileError == QFileDevice::NoError;
        if (m_writeResult) {
            m_writeUnconfirmed = false;
        } else {
            m_writeDiagnostics =
                QStringLiteral("writeAttempted=true streamStatus=%1 flushOk=%2 fileErrorBeforeClose=%3 "
                               "fileErrorAfterClose=%4")
                    .arg(static_cast<int>(streamStatus))
                    .arg(flushOk ? QStringLiteral("true") : QStringLiteral("false"))
                    .arg(static_cast<int>(fileErrorBeforeClose))
                    .arg(static_cast<int>(fileErrorAfterClose));
        }
        if (!m_writeResult)
            emit errorOccurred(QString("无法确认文件写入结果: %1").arg(filePath));
    } else {
        m_writeResult = false;
        emit errorOccurred(QString("无法写入文件: %1").arg(filePath));
    }

    output.setData("write_result", m_writeResult);
    output.setData("write_path", filePath);
    output.setData("write_length", textContent.length());

    Logger::instance().debug(QString("写入文本: %1, 长度: %2, 结果: %3")
                                 .arg(filePath)
                                 .arg(textContent.length())
                                 .arg(m_writeResult ? "成功" : "失败"),
                             "WriteText");

    return m_writeResult;
}

bool WriteTextPlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    // 阶段 2: filePath 等外部资源允许暂时为空，由 process() 在运行时报告"未配置"
    Q_UNUSED(params)
    error.clear();
    return true;
}

QWidget* WriteTextPlugin::createConfigWidget() {
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(tr("文件路径:")));
    QLineEdit* pathEdit = new QLineEdit(m_params["filePath"].toString());
    layout->addWidget(pathEdit);

    layout->addWidget(new QLabel(tr("文本内容:")));
    QTextEdit* textEdit = new QTextEdit();
    textEdit->setPlainText(m_params["textContent"].toString());
    textEdit->setMaximumHeight(100);
    layout->addWidget(textEdit);

    layout->addStretch();

    connect(pathEdit, &QLineEdit::textChanged, this, [this](const QString& text) { setParam("filePath", text); });

    connect(textEdit, &QTextEdit::textChanged, this,
            [this, textEdit]() { setParam("textContent", textEdit->toPlainText()); });

    return widget;
}

IModule* WriteTextPlugin::cloneImpl() const {
    WriteTextPlugin* clone = new WriteTextPlugin();
    return clone;
}

} // namespace DeepLux
