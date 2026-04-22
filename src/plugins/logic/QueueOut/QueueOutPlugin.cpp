#include "QueueOutPlugin.h"
#include "core/common/Logger.h"
#include "core/manager/GlobalVarManager.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QCheckBox>

namespace DeepLux {

QueueOutPlugin::QueueOutPlugin(QObject* parent)
    : ModuleBase(parent)
    , m_peekOnly(false)
{
    m_defaultParams = QJsonObject{
        {"queueName", "defaultQueue"},
        {"outputVariable", "queue_item"},
        {"peekOnly", false}
    };
    m_params = m_defaultParams;
}

bool QueueOutPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "QueueOutPlugin initialized";
    return true;
}

bool QueueOutPlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    m_queueName = m_params["queueName"].toString("defaultQueue");
    m_outputVariable = m_params["outputVariable"].toString("queue_item");
    m_peekOnly = m_params["peekOnly"].toBool(false);

    if (m_queueName.isEmpty()) {
        emit errorOccurred(tr("队列名称不能为空"));
        return false;
    }

    int queueSize = GlobalVarManager::instance().queueSize(m_queueName);
    if (queueSize == 0) {
        output.setData("queue_empty", true);
        output.setData("queue_item", QString());
        Logger::instance().warning(QString("QueueOut: Queue '%1' is empty").arg(m_queueName), "Logic");
        return true; // Not an error, just empty
    }

    QString item;
    if (m_peekOnly) {
        item = GlobalVarManager::instance().peekQueue(m_queueName).toString();
    } else {
        item = GlobalVarManager::instance().dequeue(m_queueName).toString();
    }

    output.setData(m_outputVariable, item);
    output.setData(QString("queue_%1_size").arg(m_queueName), GlobalVarManager::instance().queueSize(m_queueName));
    output.setData("queue_empty", false);

    Logger::instance().info(QString("QueueOut: Got item from queue '%1', remaining %2")
        .arg(m_queueName).arg(GlobalVarManager::instance().queueSize(m_queueName)), "Logic");

    return true;
}

bool QueueOutPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    if (params["queueName"].toString().isEmpty()) {
        error = QString("Queue name cannot be empty");
        return false;
    }
    return true;
}

QWidget* QueueOutPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    QFormLayout* formLayout = new QFormLayout();

    QLineEdit* queueNameEdit = new QLineEdit(m_params["queueName"].toString("defaultQueue"));
    QLineEdit* outputVarEdit = new QLineEdit(m_params["outputVariable"].toString("queue_item"));
    QCheckBox* peekCheck = new QCheckBox(tr("仅查看(不移除)"));
    peekCheck->setChecked(m_params["peekOnly"].toBool(false));

    formLayout->addRow(tr("队列名称:"), queueNameEdit);
    formLayout->addRow(tr("输出变量:"), outputVarEdit);
    formLayout->addRow(tr(""), peekCheck);

    layout->addLayout(formLayout);
    layout->addStretch();

    connect(queueNameEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["queueName"] = text;
    });

    connect(outputVarEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["outputVariable"] = text;
    });

    connect(peekCheck, &QCheckBox::toggled, this, [=](bool checked) {
        m_params["peekOnly"] = checked;
    });

    return widget;
}

} // namespace DeepLux
