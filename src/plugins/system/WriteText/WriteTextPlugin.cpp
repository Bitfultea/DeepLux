#include "WriteTextPlugin.h"
#include "common/Logger.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QTextEdit>
#include <QCheckBox>

namespace DeepLux {

WriteTextPlugin::WriteTextPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"filePath", ""},
        {"textContent", ""},
        {"appendMode", false}
    };
    m_params = m_defaultParams;
}

WriteTextPlugin::~WriteTextPlugin()
{
}

bool WriteTextPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "WriteTextPlugin initialized";
    return true;
}

void WriteTextPlugin::shutdown()
{
    ModuleBase::shutdown();
}

bool WriteTextPlugin::process(const ImageData& input, ImageData& output)
{
    Q_UNUSED(output);
    output = input;

    QString filePath = m_params["filePath"].toString();
    if (filePath.isEmpty()) {
        filePath = input.data("file_path").toString();
    }

    QString textContent = m_params["textContent"].toString();
    if (textContent.isEmpty()) {
        textContent = input.data("text_content").toString();
    }

    bool appendMode = m_params["appendMode"].toBool();

    if (filePath.isEmpty()) {
        emit errorOccurred(tr("未提供文件路径"));
        return false;
    }

    QFile file(filePath);
    QIODevice::OpenMode mode = appendMode ? QIODevice::Append : QIODevice::WriteOnly;

    if (file.open(mode | QIODevice::Text)) {
        QTextStream out(&file);
        out << textContent;
        if (!textContent.endsWith("\n")) {
            out << "\n";
        }
        file.close();
        m_writeResult = true;
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
                                .arg(m_writeResult ? "成功" : "失败"), "WriteText");

    return true;
}

bool WriteTextPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    QString filePath = params["filePath"].toString();
    if (filePath.isEmpty()) {
        error = tr("文件路径不能为空");
        return false;
    }
    return true;
}

QWidget* WriteTextPlugin::createConfigWidget()
{
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

    connect(pathEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_params["filePath"] = text;
    });

    connect(textEdit, &QTextEdit::textChanged, this, [this, textEdit]() {
        m_params["textContent"] = textEdit->toPlainText();
    });

    return widget;
}

IModule* WriteTextPlugin::cloneImpl() const
{
    WriteTextPlugin* clone = new WriteTextPlugin();
    return clone;
}

} // namespace DeepLux
