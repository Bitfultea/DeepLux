#include "LoopPlugin.h"

#include "core/common/Logger.h"

#include <QFormLayout>
#include <QSpinBox>
#include <QVBoxLayout>

namespace DeepLux {

LoopPlugin::LoopPlugin(QObject* parent) : ModuleBase(parent), m_currentIteration(0) {
    m_defaultParams = QJsonObject{{"loopCount", 1}};
    m_params = m_defaultParams;
}

bool LoopPlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "LoopPlugin initialized";
    return true;
}

bool LoopPlugin::process(const ImageData& input, ImageData& output) {
    output = input;

    QJsonObject params = currentParams();
    m_loopCount = params["loopCount"].toInt(1);

    if (m_loopCount < 0) {
        emit errorOccurred(tr("循环次数不能小于0"));
        return false;
    }

    m_currentIteration = 0;

    // Note: The actual loop execution is handled by the run engine
    // This plugin just sets up the loop parameters

    output.setData("loop_count", m_loopCount);
    output.setData("loop_current", m_currentIteration);
    output.setData("loop_active", m_loopCount > 0);

    Logger::instance().info(QString("Loop: Starting loop with %1 iterations").arg(m_loopCount), "Logic");

    return true;
}

bool LoopPlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    if (params["loopCount"].toInt() < 0) {
        error = QString("Loop count must not be negative");
        return false;
    }
    return true;
}

QWidget* LoopPlugin::createConfigWidget() {
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    QFormLayout* formLayout = new QFormLayout();

    QSpinBox* loopCountSpin = new QSpinBox();
    loopCountSpin->setRange(0, 10000);
    loopCountSpin->setValue(m_params["loopCount"].toInt(1));
    loopCountSpin->setPrefix(tr("循环次数: "));

    formLayout->addRow(tr("循环次数:"), loopCountSpin);

    layout->addLayout(formLayout);
    layout->addStretch();

    connect(loopCountSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [=](int value) { setParam("loopCount", value); });

    return widget;
}

IModule* LoopPlugin::cloneImpl() const {
    return new LoopPlugin();
}

} // namespace DeepLux
