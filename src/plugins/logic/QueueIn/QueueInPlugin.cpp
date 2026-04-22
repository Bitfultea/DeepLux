#include "QueueInPlugin.h"
#include "core/common/Logger.h"
#include "core/manager/GlobalVarManager.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QLineEdit>

namespace DeepLux {

QueueInPlugin::QueueInPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"queueName", "defaultQueue"},
        {"dataVariable", ""}
    };
    m_params = m_defaultParams;
}

bool QueueInPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "QueueInPlugin initialized";
    return true;
}

bool QueueInPlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    m_queueName = m_params["queueName"].toString("defaultQueue");
    m_dataVariable = m_params["dataVariable"].toString();

    if (m_queueName.isEmpty()) {
        emit errorOccurred(tr("队列名称不能为空"));
        return false;
    }

    QString dataToQueue;
    if (!m_dataVariable.isEmpty()) {
        dataToQueue = input.data(m_dataVariable).toString();
    } else {
        dataToQueue = input.data("item_data").toString();
    }

    if (dataToQueue.isEmpty()) {
        emit errorOccurred(tr("队列数据为空"));
        return false;
    }

    GlobalVarManager::instance().enqueue(m_queueName, dataToQueue);

    int queueSize = GlobalVarManager::instance().queueSize(m_queueName);
    output.setData(QString("queue_%1_size").arg(m_queueName), queueSize);

    Logger::instance().info(QString("QueueIn: Added item to queue '%1', size now %2")
        .arg(m_queueName).arg(queueSize), "Logic");

    return true;
}

bool QueueInPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    if (params["queueName"].toString().isEmpty()) {
        error = QString("Queue name cannot be empty");
        return false;
    }
    return true;
}

QWidget* QueueInPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    QFormLayout* formLayout = new QFormLayout();

    QLineEdit* queueNameEdit = new QLineEdit(m_params["queueName"].toString("defaultQueue"));
    QLineEdit* dataVarEdit = new QLineEdit(m_params["dataVariable"].toString());

    formLayout->addRow(tr("队列名称:"), queueNameEdit);
    formLayout->addRow(tr("数据变量:"), dataVarEdit);

    layout->addLayout(formLayout);
    layout->addStretch();

    connect(queueNameEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["queueName"] = text;
    });

    connect(dataVarEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["dataVariable"] = text;
    });

    return widget;
}

} // namespace DeepLux
