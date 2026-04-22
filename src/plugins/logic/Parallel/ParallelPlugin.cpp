#include "ParallelPlugin.h"
#include "core/engine/RunEngine.h"
#include "core/common/Logger.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QFormLayout>
#include <QSpinBox>

namespace DeepLux {

ParallelPlugin::ParallelPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"parallelCount", 2}
    };
    m_params = m_defaultParams;
    m_name = "并行开始";
    m_moduleId = "ParallelPlugin";
    m_category = "logic";
    m_description = "并行执行多个分支";
    m_parallelCount = 2;
    m_branchNames = QStringList() << "Branch1" << "Branch2";
}

bool ParallelPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "ParallelPlugin initialized";
    return true;
}

bool ParallelPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    int count = params["parallelCount"].toInt(2);
    if (count < 2 || count > 10) {
        error = QString("Parallel count must be between 2 and 10");
        return false;
    }
    return true;
}

bool ParallelPlugin::process(const ImageData& input, ImageData& output)
{
    output = input;
    m_parallelCount = m_params["parallelCount"].toInt(2);

    output.setData("parallel_count", m_parallelCount);
    output.setData("parallel_started", true);

    Logger::instance().info(QString("Parallel: Starting %1 parallel branches").arg(m_parallelCount), "Logic");

    return true;
}

QWidget* ParallelPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    QFormLayout* formLayout = new QFormLayout();

    QSpinBox* countSpin = new QSpinBox();
    countSpin->setRange(2, 10);
    countSpin->setValue(m_params["parallelCount"].toInt(2));
    countSpin->setPrefix(tr("分支数量: "));

    formLayout->addRow(tr("并行分支数:"), countSpin);

    layout->addLayout(formLayout);
    layout->addStretch();

    connect(countSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int value) {
        m_params["parallelCount"] = value;
        m_parallelCount = value;
    });

    return widget;
}

bool ParallelEndPlugin::process(const ImageData& input, ImageData& output)
{
    output = input;
    output.setData("parallel_ended", true);
    Logger::instance().info("Parallel: All branches completed", "Logic");
    return true;
}

QWidget* ParallelEndPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);
    layout->addWidget(new QLabel(tr("(并行结束，无需配置)")));
    return widget;
}

} // namespace DeepLux
