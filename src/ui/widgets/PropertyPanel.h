#pragma once

#include <QWidget>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QMap>

namespace DeepLux {

class IModule;

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

    void setModule(IModule* module);
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

private:
    QScrollArea* m_scrollArea = nullptr;
    QWidget* m_contentWidget = nullptr;
    QVBoxLayout* m_contentLayout = nullptr;
    
    QLabel* m_titleLabel = nullptr;
    QLabel* m_noSelectionLabel = nullptr;
    
    IModule* m_currentModule = nullptr;
    QString m_currentModuleId;
    
    QMap<QString, QWidget*> m_paramWidgets;
};

} // namespace DeepLux
