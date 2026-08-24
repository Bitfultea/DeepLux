#include "TimeSlicePlugin.h"

#include "common/Logger.h"

#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>
#include <chrono>

namespace DeepLux {

namespace {
// G3-fix1: 单调时钟（steady_clock）毫秒计数，不受 NTP/人工调时影响
qint64 steadyNowMs() {
    return static_cast<qint64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
} // namespace

TimeSlicePlugin::TimeSlicePlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{{"sliceName", ""}, {"mode", "Start"}};
    m_params = m_defaultParams;
}

TimeSlicePlugin::~TimeSlicePlugin() {}

bool TimeSlicePlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "TimeSlicePlugin initialized";
    return true;
}

void TimeSlicePlugin::shutdown() {
    ModuleBase::shutdown();
}

bool TimeSlicePlugin::process(const ImageData& input, ImageData& output) {
    output = input;

    QJsonObject params = currentParams();
    QString mode = params["mode"].toString();
    m_sliceName = params["sliceName"].toString();

    // G2-fix1: 校验 mode 枚举，未知值不能静默成功
    if (mode != "Start" && mode != "Stop" && mode != "Reset") {
        emit errorOccurred(tr("无效的 mode：%1（必须为 Start/Stop/Reset）").arg(mode));
        return false;
    }

    if (mode == "Start") {
        // G3-fix1: 单调时钟，不受 NTP/人工调时影响
        m_startTime = steadyNowMs();
        output.setData("timeslice_start_time", m_startTime);
        output.setData("timeslice_started", true);
        output.setData("timeslice_name", m_sliceName);
        Logger::instance().debug(QString("时间片开始: %1").arg(m_sliceName), "TimeSlice");
    } else if (mode == "Stop") {
        // G3-fix1: 单调时钟
        qint64 endTime = steadyNowMs();

        // G-fix1: 起始时间必须由上游 Start 通过数据边提供；缺失时结构化报错
        QVariant startTimeVar = input.data("timeslice_start_time");
        if (!startTimeVar.isValid()) {
            emit errorOccurred(tr("Stop 缺少 timeslice_start_time 输入：请确认上游 Start 节点已连接"));
            return false;
        }

        // G2-fix1: 校验转换结果——"abc" 等非数值不能静默转为 0
        bool convOk = false;
        qint64 startValue = startTimeVar.toLongLong(&convOk);
        if (!convOk) {
            emit errorOccurred(tr("timeslice_start_time 不是有效整数：%1").arg(startTimeVar.toString()));
            return false;
        }

        // G3-fix1: 拒绝非正起始时间（单调时钟计数必 > 0；0/负值说明数据损坏）
        if (startValue <= 0) {
            emit errorOccurred(tr("timeslice_start_time 必须为正数，当前为：%1").arg(startValue));
            return false;
        }

        // G2-fix1: 起始时间不能晚于当前（单调时钟下正常不应发生，防御数据错乱）
        if (startValue > endTime) {
            emit errorOccurred(
                tr("timeslice_start_time(%1) 晚于当前单调时钟(%2)，耗时将为负").arg(startValue).arg(endTime));
            return false;
        }

        m_startTime = startValue;
        m_elapsedMs = endTime - m_startTime;

        output.setData("timeslice_elapsed_ms", m_elapsedMs);
        output.setData("timeslice_elapsed_sec", m_elapsedMs / 1000.0);
        output.setData("timeslice_name", m_sliceName);

        Logger::instance().debug(QString("时间片结束: %1, 耗时: %2 ms").arg(m_sliceName).arg(m_elapsedMs), "TimeSlice");
    } else if (mode == "Reset") {
        m_startTime = 0;
        m_elapsedMs = 0;
        output.setData("timeslice_reset", true);
        Logger::instance().debug(QString("时间片重置: %1").arg(m_sliceName), "TimeSlice");
    }

    return true;
}

bool TimeSlicePlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    // G2-fix1: mode 必须为 Start/Stop/Reset 之一
    const QString mode = params["mode"].toString();
    if (mode != "Start" && mode != "Stop" && mode != "Reset") {
        error = tr("mode 必须为 Start/Stop/Reset，当前为：%1").arg(mode);
        return false;
    }
    error.clear();
    return true;
}

QWidget* TimeSlicePlugin::createConfigWidget() {
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(tr("时间片名称:")));
    QLineEdit* nameEdit = new QLineEdit(m_params["sliceName"].toString());
    layout->addWidget(nameEdit);

    layout->addWidget(new QLabel(tr("模式: Start/Stop/Reset")));
    layout->addWidget(new QLabel(tr("(通过参数 mode 设置)")));

    layout->addStretch();

    connect(nameEdit, &QLineEdit::textChanged, this, [this](const QString& text) { setParam("sliceName", text); });

    return widget;
}

IModule* TimeSlicePlugin::cloneImpl() const {
    TimeSlicePlugin* clone = new TimeSlicePlugin();
    return clone;
}

} // namespace DeepLux
