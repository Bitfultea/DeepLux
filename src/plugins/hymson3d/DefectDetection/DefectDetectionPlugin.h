#pragma once

#include "core/base/ModuleBase.h"
#include "core/display/IDisplayPort.h"
#include "core/display/DisplayData.h"

#ifdef DEEPLUX_HAS_HYMSON3D
#include "plugins/hymson3d/common/PointCloudConverter.h"
#endif

namespace DeepLux {

class DefectDetectionPlugin : public ModuleBase, public IDisplayPort
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit DefectDetectionPlugin(QObject* parent = nullptr);
    ~DefectDetectionPlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.defectdetection"; }
    QString name() const override { return tr("缺陷检测"); }
    QString category() const override { return "hymson3d"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("基于HymsonVision3D的点云缺陷检测"); }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

    // IDisplayPort interface
    bool hasDisplayOutput() const override;
    DisplayData getDisplayData() const override;
    QString preferredViewport() const override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    // 缺陷检测参数
    float m_longNormalDegree = 30.0f;
    float m_longCurvatureThreshold = 0.02f;
    float m_rcornerNormalDegree = 30.0f;
    float m_rcornerCurvatureThreshold = 0.02f;
    float m_heightThreshold = 0.0f;
    float m_radius = 0.08f;
    size_t m_minPoints = 5;
    size_t m_minDefectsSize = 500;
    bool m_debugMode = false;

    // 检测结果
    PointCloudData m_defectData;
    bool m_hasDefects = false;
};

} // namespace DeepLux
