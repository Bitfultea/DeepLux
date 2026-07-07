#include "LoadPointCloudPlugin.h"
#include "common/Logger.h"
#include "core/common/ConfigWidgetHelper.h"
#include "core/geometry/MeasurementData.h"
#include "core/io/PlyLoader.h"
#include "core/io/TiffLoader.h"
#include <QFileInfo>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QFileDialog>
#include <QPointer>

namespace DeepLux {

LoadPointCloudPlugin::LoadPointCloudPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"filePath", ""},
        {"tiffStep", 1},
        {"scaleX", 1.0},
        {"scaleY", 1.0},
        {"scaleZ", 1.0}
    };
    m_params = m_defaultParams;
}

LoadPointCloudPlugin::~LoadPointCloudPlugin()
{
}

bool LoadPointCloudPlugin::initialize()
{
    return ModuleBase::initialize();
}

void LoadPointCloudPlugin::shutdown()
{
    ModuleBase::shutdown();
}

bool LoadPointCloudPlugin::process(const ImageData& input, ImageData& output)
{
    Q_UNUSED(input);

    QJsonObject params = currentParams();
    QString filePath = params["filePath"].toString();
    int tiffStep = params["tiffStep"].toInt(1);
    float scaleX = static_cast<float>(params["scaleX"].toDouble(1.0));
    float scaleY = static_cast<float>(params["scaleY"].toDouble(1.0));
    float scaleZ = static_cast<float>(params["scaleZ"].toDouble(1.0));

    if (filePath.isEmpty()) {
        emit errorOccurred(tr("未指定文件路径"));
        return false;
    }

    QFileInfo fileInfo(filePath);
    if (!fileInfo.exists()) {
        emit errorOccurred(tr("文件不存在: %1").arg(filePath));
        return false;
    }

    QString suffix = fileInfo.suffix().toLower();
    PointCloudData cloud;
    QString errorMsg;
    bool ok = false;

    if (suffix == "ply") {
        ok = PlyLoader::load(filePath, cloud, errorMsg);
    } else if (suffix == "tif" || suffix == "tiff") {
        TiffLoader::Config config;
        config.step = tiffStep;
        config.scaleX = scaleX;
        config.scaleY = scaleY;
        config.scaleZ = scaleZ;
        ok = TiffLoader::load(filePath, cloud, errorMsg, config);
    } else {
        emit errorOccurred(tr("不支持的文件格式: %1 (仅支持 .ply, .tif, .tiff)").arg(suffix));
        return false;
    }

    if (!ok) {
        emit errorOccurred(tr("点云加载失败: %1").arg(errorMsg));
        return false;
    }

    // Apply scale factors to point cloud (for PLY files, TiffLoader already applies them)
    bool needScale = (suffix == "ply") && (scaleX != 1.0f || scaleY != 1.0f || scaleZ != 1.0f);
    if (needScale) {
        for (auto& pt : cloud.points) {
            pt.x() *= scaleX;
            pt.y() *= scaleY;
            pt.z() *= scaleZ;
        }
    }

    // Store point cloud in output
    MeasurementData::setPointCloud(output, cloud);

    // Store metadata in output
    output.setData("point_count", static_cast<int>(cloud.points.size()));
    output.setData("point_cloud_path", filePath);

    Logger::instance().debug(
        QString("点云已加载: %1 (%2 个点)").arg(filePath).arg(cloud.points.size()),
        "LoadPointCloud");

    return true;
}

bool LoadPointCloudPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    if (params["filePath"].toString().isEmpty()) {
        error = tr("必须指定点云文件路径");
        return false;
    }
    return true;
}

QWidget* LoadPointCloudPlugin::createConfigWidget()
{
    ConfigWidgetHelper factory(true);

    QWidget* widget = new QWidget();
    factory.applyContainerStyle(widget);
    QVBoxLayout* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    // File path
    layout->addWidget(factory.createLabel(tr("点云文件:")));
    QLineEdit* filePathEdit = factory.createLineEdit(m_params["filePath"].toString());
    QPushButton* browseBtn = new QPushButton(tr("浏览..."));
    browseBtn->setStyleSheet(R"(
        QPushButton {
            background-color: #2d3748;
            color: #e8f4f8;
            border: 1px solid #4a5568;
            border-radius: 4px;
            padding: 6px 16px;
            min-height: 28px;
        }
        QPushButton:hover {
            background-color: #3d4758;
        }
    )");
    QHBoxLayout* pathLayout = new QHBoxLayout();
    pathLayout->setSpacing(8);
    pathLayout->addWidget(filePathEdit);
    pathLayout->addWidget(browseBtn);
    layout->addLayout(pathLayout);

    // TIFF step
    layout->addWidget(factory.createLabel(tr("TIFF 采样步长:")));
    QSpinBox* tiffStepSpin = new QSpinBox();
    factory.applyInputStyle(tiffStepSpin);
    tiffStepSpin->setRange(1, 100);
    tiffStepSpin->setValue(m_params["tiffStep"].toInt(1));
    layout->addWidget(tiffStepSpin);

    // Scale factors
    layout->addWidget(factory.createLabel(tr("X 比例:")));
    QDoubleSpinBox* scaleXSpin = new QDoubleSpinBox();
    factory.applyInputStyle(scaleXSpin);
    scaleXSpin->setDecimals(3);
    scaleXSpin->setRange(0.001, 1000.0);
    scaleXSpin->setValue(m_params["scaleX"].toDouble(1.0));
    layout->addWidget(scaleXSpin);

    layout->addWidget(factory.createLabel(tr("Y 比例:")));
    QDoubleSpinBox* scaleYSpin = new QDoubleSpinBox();
    factory.applyInputStyle(scaleYSpin);
    scaleYSpin->setDecimals(3);
    scaleYSpin->setRange(0.001, 1000.0);
    scaleYSpin->setValue(m_params["scaleY"].toDouble(1.0));
    layout->addWidget(scaleYSpin);

    layout->addWidget(factory.createLabel(tr("Z 比例:")));
    QDoubleSpinBox* scaleZSpin = new QDoubleSpinBox();
    factory.applyInputStyle(scaleZSpin);
    scaleZSpin->setDecimals(3);
    scaleZSpin->setRange(0.001, 1000.0);
    scaleZSpin->setValue(m_params["scaleZ"].toDouble(1.0));
    layout->addWidget(scaleZSpin);

    layout->addStretch();

    QPointer<LoadPointCloudPlugin> pluginPtr(this);
    connect(browseBtn, &QPushButton::clicked, [pluginPtr, filePathEdit]() {
        if (!pluginPtr) return;
        QString path = QFileDialog::getOpenFileName(nullptr, tr("选择点云文件"),
            pluginPtr->m_params["filePath"].toString(),
            tr("点云文件 (*.ply *.tif *.tiff);;All Files (*)"));
        if (!path.isEmpty()) {
            filePathEdit->setText(path);
            pluginPtr->setParam("filePath", path);
        }
    });

    QPointer<LoadPointCloudPlugin> pluginPtr2(this);
    connect(filePathEdit, &QLineEdit::textChanged, [pluginPtr2](const QString& text) {
        if (pluginPtr2) pluginPtr2->setParam("filePath", text);
    });

    QPointer<LoadPointCloudPlugin> pluginPtr3(this);
    connect(tiffStepSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            [pluginPtr3](int val) {
        if (pluginPtr3) pluginPtr3->setParam("tiffStep", val);
    });

    QPointer<LoadPointCloudPlugin> pluginPtr4(this);
    connect(scaleXSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            [pluginPtr4](double val) {
        if (pluginPtr4) pluginPtr4->setParam("scaleX", val);
    });

    QPointer<LoadPointCloudPlugin> pluginPtr5(this);
    connect(scaleYSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            [pluginPtr5](double val) {
        if (pluginPtr5) pluginPtr5->setParam("scaleY", val);
    });

    QPointer<LoadPointCloudPlugin> pluginPtr6(this);
    connect(scaleZSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            [pluginPtr6](double val) {
        if (pluginPtr6) pluginPtr6->setParam("scaleZ", val);
    });

    return widget;
}

IModule* LoadPointCloudPlugin::cloneImpl() const
{
    LoadPointCloudPlugin* clone = new LoadPointCloudPlugin();
    return clone;
}

} // namespace DeepLux
