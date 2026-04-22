#include "TimeSlicePlugin.h"
#include "common/Logger.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>

namespace DeepLux {

TimeSlicePlugin::TimeSlicePlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"sliceName", ""},
        {"mode", "Start"}
    };
    m_params = m_defaultParams;
}

TimeSlicePlugin::~TimeSlicePlugin()
{
}

bool TimeSlicePlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "TimeSlicePlugin initialized";
    return true;
}

void TimeSlicePlugin::shutdown()
{
    ModuleBase::shutdown();
}

bool TimeSlicePlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    QString mode = m_params["mode"].toString();
    m_sliceName = m_params["sliceName"].toString();

    if (mode == "Start") {
        m_startTime = QDateTime::currentMSecsSinceEpoch();
        output.setData("timeslice_started", true);
        output.setData("timeslice_name", m_sliceName);
        Logger::instance().debug(QString("时间片开始: %1").arg(m_sliceName), "TimeSlice");
    }
    else if (mode == "Stop") {
        qint64 endTime = QDateTime::currentMSecsSinceEpoch();

        // 获取之前的时间
        QVariant startTimeVar = input.data("timeslice_start_time");
        if (startTimeVar.isValid()) {
            m_startTime = startTimeVar.toLongLong();
        }

        m_elapsedMs = endTime - m_startTime;

        output.setData("timeslice_elapsed_ms", m_elapsedMs);
        output.setData("timeslice_elapsed_sec", m_elapsedMs / 1000.0);
        output.setData("timeslice_name", m_sliceName);

        Logger::instance().debug(QString("时间片结束: %1, 耗时: %2 ms").arg(m_sliceName).arg(m_elapsedMs), "TimeSlice");
    }
    else if (mode == "Reset") {
        m_startTime = 0;
        m_elapsedMs = 0;
        output.setData("timeslice_reset", true);
        Logger::instance().debug(QString("时间片重置: %1").arg(m_sliceName), "TimeSlice");
    }

    return true;
}

bool TimeSlicePlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    Q_UNUSED(params);
    error.clear();
    return true;
}

QWidget* TimeSlicePlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(tr("时间片名称:")));
    QLineEdit* nameEdit = new QLineEdit(m_params["sliceName"].toString());
    layout->addWidget(nameEdit);

    layout->addWidget(new QLabel(tr("模式: Start/Stop/Reset")));
    layout->addWidget(new QLabel(tr("(通过参数 mode 设置)")));

    layout->addStretch();

    connect(nameEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_params["sliceName"] = text;
    });

    return widget;
}

IModule* TimeSlicePlugin::cloneImpl() const
{
    TimeSlicePlugin* clone = new TimeSlicePlugin();
    return clone;
}

} // namespace DeepLux
