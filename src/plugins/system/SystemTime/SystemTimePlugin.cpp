#include "SystemTimePlugin.h"
#include "common/Logger.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>

namespace DeepLux {

SystemTimePlugin::SystemTimePlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"timeFormat", "yyyy-MM-dd HH:mm:ss"}
    };
    m_params = m_defaultParams;
}

SystemTimePlugin::~SystemTimePlugin()
{
}

bool SystemTimePlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "SystemTimePlugin initialized";
    return true;
}

void SystemTimePlugin::shutdown()
{
    ModuleBase::shutdown();
}

bool SystemTimePlugin::process(const ImageData& input, ImageData& output)
{
    Q_UNUSED(input);
    output = input;

    m_timeFormat = m_params["timeFormat"].toString();

    QDateTime now = QDateTime::currentDateTime();
    QString timeString = now.toString(m_timeFormat);
    qint64 timeStamp = now.toMSecsSinceEpoch();

    output.setData("time_string", timeString);
    output.setData("time_stamp", timeStamp);
    output.setData("time_year", now.date().year());
    output.setData("time_month", now.date().month());
    output.setData("time_day", now.date().day());
    output.setData("time_hour", now.time().hour());
    output.setData("time_minute", now.time().minute());
    output.setData("time_second", now.time().second());

    Logger::instance().debug(QString("系统时间: %1").arg(timeString), "SystemTime");

    return true;
}

bool SystemTimePlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    Q_UNUSED(params);
    error.clear();
    return true;
}

QWidget* SystemTimePlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(tr("时间格式:")));
    QLineEdit* formatEdit = new QLineEdit(m_params["timeFormat"].toString());
    layout->addWidget(formatEdit);

    layout->addWidget(new QLabel(tr("格式示例: yyyy-MM-dd HH:mm:ss")));
    layout->addStretch();

    connect(formatEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_params["timeFormat"] = text;
    });

    return widget;
}

IModule* SystemTimePlugin::cloneImpl() const
{
    SystemTimePlugin* clone = new SystemTimePlugin();
    return clone;
}

} // namespace DeepLux
