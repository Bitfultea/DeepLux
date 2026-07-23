#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QScrollArea>
#include <QSpinBox>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

namespace DeepLux {

class IModule;
struct PluginInfo;

/**
 * @brief 属性面板
 *
 * 显示和编辑模块参数
 */
class PropertyPanel : public QWidget
{
    Q_OBJECT

public:
    explicit PropertyPanel(QWidget* parent = nullptr);
    ~PropertyPanel() override;

    void setModule(IModule* module, const QString& instanceId = QString());
    void setPluginInfo(const PluginInfo& info);
    void applyTheme(bool isDark);
    void clear();

    QString currentModuleId() const { return m_currentModuleId; }

signals:
    void paramsChanged(const QString& moduleId, const QString& key, const QVariant& value);

private:
    void setupUi();
    void loadParams();
    void createParamWidget(const QString& key, const QJsonObject& paramInfo);

    QWidget* createTextWidget(const QString& key, const QJsonObject& info);
    QWidget* createNumberWidget(const QString& key, const QJsonObject& info);
    QWidget* createBoolWidget(const QString& key, const QJsonObject& info);
    QWidget* createChoiceWidget(const QString& key, const QJsonObject& info);

    // 提交参数：构造候选 JSON、调用 validateParams、失败则保持旧值并标红
    bool commitParam(const QString& key, const QVariant& value);
    void markWidgetError(QWidget* widget, const QString& hint);
    void clearWidgetError(QWidget* widget);

    // 根据 ui.parameters 生成参数排序
    QStringList sortedParamKeys(const QJsonObject& params) const;

private:
    QScrollArea* m_scrollArea = nullptr;
    QWidget* m_contentWidget = nullptr;
    QVBoxLayout* m_contentLayout = nullptr;

    QLabel* m_titleLabel = nullptr;
    QLabel* m_noSelectionLabel = nullptr;

    IModule* m_currentModule = nullptr;
    QString m_currentModuleId;
    QJsonObject m_uiParameters; // ui.parameters 元数据

    QMap<QString, QWidget*> m_paramWidgets;
    QMap<QString, QLabel*> m_paramLabels; // 用于显示错误提示
};

} // namespace DeepLux
