#include "DefectDetectionPlugin.h"
#include "core/common/Logger.h"
#include "core/common/ConfigWidgetHelper.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QPointer>

namespace DeepLux {

DefectDetectionPlugin::DefectDetectionPlugin(QObject* parent)
    : ModuleBase(parent)
    , m_hasDefects(false)
{
    m_moduleId = "com.deeplux.plugin.defectdetection";
    m_name = tr("缺陷检测");
    m_category = "hymson3d";
    m_version = "1.0.0";
    m_author = "DeepLux Team";
    m_description = tr("基于HymsonVision3D的点云缺陷检测");

    m_defaultParams = QJsonObject{
        {"longNormalDegree", 30.0},
        {"longCurvatureThreshold", 0.02},
        {"rcornerNormalDegree", 30.0},
        {"rcornerCurvatureThreshold", 0.02},
        {"heightThreshold", 0.0},
        {"radius", 0.08},
        {"minPoints", 5},
        {"minDefectsSize", 500},
        {"debugMode", false}
    };
    m_params = m_defaultParams;
}

DefectDetectionPlugin::~DefectDetectionPlugin()
{
}

bool DefectDetectionPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "DefectDetectionPlugin initialized";
    Logger::instance().info("缺陷检测插件已初始化", "DefectDetection");
    return true;
}

void DefectDetectionPlugin::shutdown()
{
    m_defectData.clear();
    m_hasDefects = false;
    ModuleBase::shutdown();
}

bool DefectDetectionPlugin::process(const ImageData& input, ImageData& output)
{
    Q_UNUSED(input);
    Q_UNUSED(output);

#ifdef DEEPLUX_HAS_HYMSON3D
    // 获取参数
    float longNormalDegree = m_params["longNormalDegree"].toDouble();
    float longCurvatureThreshold = m_params["longCurvatureThreshold"].toDouble();
    float rcornerNormalDegree = m_params["rcornerNormalDegree"].toDouble();
    float rcornerCurvatureThreshold = m_params["rcornerCurvatureThreshold"].toDouble();
    float heightThreshold = m_params["heightThreshold"].toDouble();
    float radius = m_params["radius"].toDouble();
    size_t minPoints = m_params["minPoints"].toInteger();
    size_t minDefectsSize = m_params["minDefectsSize"].toInteger();
    m_debugMode = m_params["debugMode"].toBool();

    // TODO: 从输入获取点云数据
    // 当前Demo: 创建演示用的点云数据
    PointCloudData inputCloud;

    // 生成一些测试点 (平面上的点)
    for (int i = 0; i < 1000; ++i) {
        double x = static_cast<double>(i % 50) * 0.01;
        double y = static_cast<double>(i / 50) * 0.01;
        double z = 0.0;
        inputCloud.points.emplace_back(x, y, z);
    }

    // 添加一些"缺陷"点 (高度变化)
    for (int i = 0; i < 100; ++i) {
        double x = static_cast<double>(i % 20) * 0.01 + 0.25;
        double y = static_cast<double>(i / 20) * 0.01 + 0.25;
        double z = 0.05;  // 凸起缺陷
        inputCloud.points.emplace_back(x, y, z);
    }

    // 转换为 HymsonVision3D 点云
    auto cloud = PointCloudConverter::toHymsonCloud(inputCloud);

    // 设置 KDTree 搜索参数
    hymson3d::geometry::KDTreeSearchParamRadius searchParam;
    searchParam.radius = radius;

    // 执行缺陷检测
    std::vector<hymson3d::geometry::PointCloud::Ptr> defectClouds;
    hymson3d::pipeline::DefectDetection::detect_smooth_surface_dll(
        cloud,
        searchParam,
        longNormalDegree,
        longCurvatureThreshold,
        minDefectsSize,
        heightThreshold,
        defectClouds,
        m_debugMode
    );

    // 合并缺陷点云
    m_defectData.clear();
    for (const auto& defectCloud : defectClouds) {
        auto data = PointCloudConverter::fromHymsonCloud(defectCloud);
        m_defectData.points.insert(m_defectData.points.end(), data.points.begin(), data.points.end());
        m_defectData.labels.insert(m_defectData.labels.end(), data.labels.begin(), data.labels.end());
    }

    // 为缺陷点添加标签
    for (size_t i = 0; i < m_defectData.points.size(); ++i) {
        m_defectData.labels.push_back(1);  // 1 = 缺陷
    }

    m_hasDefects = !m_defectData.isEmpty();

    if (m_hasDefects) {
        Logger::instance().info(QString("检测到 %1 个缺陷点").arg(m_defectData.size()), "DefectDetection");
    } else {
        Logger::instance().info("未检测到缺陷", "DefectDetection");
    }

    return true;
#else
    Logger::instance().warning("HymsonVision3D 未启用，缺陷检测不可用", "DefectDetection");
    return false;
#endif
}

bool DefectDetectionPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    Q_UNUSED(params);
    error.clear();
    return true;
}

QWidget* DefectDetectionPlugin::createConfigWidget()
{
    ConfigWidgetHelper factory(true);  // 默认深色主题

    QWidget* widget = new QWidget();
    factory.applyContainerStyle(widget);
    QVBoxLayout* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    // 长边缺陷参数
    QGroupBox* longEdgeGroup = new QGroupBox(tr("长边缺陷参数"));
    longEdgeGroup->setStyleSheet(R"(
        QGroupBox {
            font-weight: bold;
            color: #e8f4f8;
            border: 1px solid #2d3748;
            border-radius: 6px;
            margin-top: 12px;
            padding-top: 12px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 12px;
            padding: 0 4px;
        }
    )");
    QFormLayout* longLayout = new QFormLayout(longEdgeGroup);
    longLayout->setSpacing(8);
    longLayout->setContentsMargins(12, 8, 12, 12);

    QDoubleSpinBox* longNormalSpin = factory.createDoubleSpinBox(0, 90, m_params["longNormalDegree"].toDouble(), 2);
    longNormalSpin->setSuffix(" deg");
    longLayout->addRow(factory.createLabel(tr("法向量角度阈值:")), longNormalSpin);

    QDoubleSpinBox* longCurveSpin = factory.createDoubleSpinBox(0, 1, m_params["longCurvatureThreshold"].toDouble(), 4);
    longLayout->addRow(factory.createLabel(tr("曲率阈值:")), longCurveSpin);

    // 角落缺陷参数
    QGroupBox* cornerGroup = new QGroupBox(tr("角落缺陷参数"));
    cornerGroup->setStyleSheet(R"(
        QGroupBox {
            font-weight: bold;
            color: #e8f4f8;
            border: 1px solid #2d3748;
            border-radius: 6px;
            margin-top: 12px;
            padding-top: 12px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 12px;
            padding: 0 4px;
        }
    )");
    QFormLayout* cornerLayout = new QFormLayout(cornerGroup);
    cornerLayout->setSpacing(8);
    cornerLayout->setContentsMargins(12, 8, 12, 12);

    QDoubleSpinBox* cornerNormalSpin = factory.createDoubleSpinBox(0, 90, m_params["rcornerNormalDegree"].toDouble(), 2);
    cornerNormalSpin->setSuffix(" deg");
    cornerLayout->addRow(factory.createLabel(tr("法向量角度阈值:")), cornerNormalSpin);

    QDoubleSpinBox* cornerCurveSpin = factory.createDoubleSpinBox(0, 1, m_params["rcornerCurvatureThreshold"].toDouble(), 4);
    cornerLayout->addRow(factory.createLabel(tr("曲率阈值:")), cornerCurveSpin);

    // 通用参数
    QGroupBox* commonGroup = new QGroupBox(tr("通用参数"));
    commonGroup->setStyleSheet(R"(
        QGroupBox {
            font-weight: bold;
            color: #e8f4f8;
            border: 1px solid #2d3748;
            border-radius: 6px;
            margin-top: 12px;
            padding-top: 12px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 12px;
            padding: 0 4px;
        }
    )");
    QFormLayout* commonLayout = new QFormLayout(commonGroup);
    commonLayout->setSpacing(8);
    commonLayout->setContentsMargins(12, 8, 12, 12);

    QDoubleSpinBox* heightSpin = factory.createDoubleSpinBox(-1, 1, m_params["heightThreshold"].toDouble(), 4);
    heightSpin->setSuffix(" m");
    commonLayout->addRow(factory.createLabel(tr("高度阈值:")), heightSpin);

    QDoubleSpinBox* radiusSpin = factory.createDoubleSpinBox(0.001, 1, m_params["radius"].toDouble(), 3);
    radiusSpin->setSuffix(" m");
    commonLayout->addRow(factory.createLabel(tr("搜索半径:")), radiusSpin);

    QSpinBox* minPointsSpin = factory.createSpinBox(1, 1000, m_params["minPoints"].toInt());
    commonLayout->addRow(factory.createLabel(tr("最小点数:")), minPointsSpin);

    QSpinBox* minDefectsSpin = factory.createSpinBox(1, 10000, m_params["minDefectsSize"].toInt());
    commonLayout->addRow(factory.createLabel(tr("最小缺陷尺寸:")), minDefectsSpin);

    QCheckBox* debugCheck = factory.createCheckBox(tr("调试模式"), m_params["debugMode"].toBool());
    commonLayout->addRow(factory.createLabel(tr("")), debugCheck);

    layout->addWidget(longEdgeGroup);
    layout->addWidget(cornerGroup);
    layout->addWidget(commonGroup);
    layout->addStretch();

    // 信号连接
    QPointer<DefectDetectionPlugin> pluginPtr(this);

    connect(longNormalSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            [pluginPtr](double v) { if (pluginPtr) pluginPtr->m_params["longNormalDegree"] = v; });
    connect(longCurveSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            [pluginPtr](double v) { if (pluginPtr) pluginPtr->m_params["longCurvatureThreshold"] = v; });
    connect(cornerNormalSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            [pluginPtr](double v) { if (pluginPtr) pluginPtr->m_params["rcornerNormalDegree"] = v; });
    connect(cornerCurveSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            [pluginPtr](double v) { if (pluginPtr) pluginPtr->m_params["rcornerCurvatureThreshold"] = v; });
    connect(heightSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            [pluginPtr](double v) { if (pluginPtr) pluginPtr->m_params["heightThreshold"] = v; });
    connect(radiusSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            [pluginPtr](double v) { if (pluginPtr) pluginPtr->m_params["radius"] = v; });
    connect(minPointsSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            [pluginPtr](int v) { if (pluginPtr) pluginPtr->m_params["minPoints"] = v; });
    connect(minDefectsSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            [pluginPtr](int v) { if (pluginPtr) pluginPtr->m_params["minDefectsSize"] = v; });
    connect(debugCheck, &QCheckBox::toggled,
            [pluginPtr](bool v) { if (pluginPtr) pluginPtr->m_params["debugMode"] = v; });

    return widget;
}

IModule* DefectDetectionPlugin::cloneImpl() const
{
    DefectDetectionPlugin* clone = new DefectDetectionPlugin();
    clone->m_params = m_params;
    clone->m_longNormalDegree = m_longNormalDegree;
    clone->m_longCurvatureThreshold = m_longCurvatureThreshold;
    clone->m_rcornerNormalDegree = m_rcornerNormalDegree;
    clone->m_rcornerCurvatureThreshold = m_rcornerCurvatureThreshold;
    clone->m_heightThreshold = m_heightThreshold;
    clone->m_radius = m_radius;
    clone->m_minPoints = m_minPoints;
    clone->m_minDefectsSize = m_minDefectsSize;
    clone->m_debugMode = m_debugMode;
    return clone;
}

bool DefectDetectionPlugin::hasDisplayOutput() const
{
    return m_hasDefects;
}

DisplayData DefectDetectionPlugin::getDisplayData() const
{
    DisplayData data;
    if (m_hasDefects) {
        data.variant() = m_defectData;
        data.metadata()["plugin"] = QStringLiteral("DefectDetectionPlugin");
        data.metadata()["defectCount"] = static_cast<int>(m_defectData.size());
    }
    return data;
}

QString DefectDetectionPlugin::preferredViewport() const
{
    return QString();  // Empty means any available viewport
}

} // namespace DeepLux
