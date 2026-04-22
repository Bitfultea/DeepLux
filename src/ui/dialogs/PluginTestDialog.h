#pragma once

#include <QDialog>
#include <QString>
#include <QJsonObject>
#include <QMap>

class QComboBox;
class QPushButton;
class QTextEdit;
class QFormLayout;
class QGroupBox;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QLineEdit;
class QLabel;
class QVBoxLayout;
class QHBoxLayout;
class QScrollArea;
class QWidget;

namespace DeepLux {

class IModule;
class ImageData;

/**
 * @brief 插件测试对话框
 *
 * 用于在UI中手动测试插件功能
 * - 选择插件
 * - 设置输入参数
 * - 执行插件
 * - 查看结果
 * - 对比预期值
 */
class PluginTestDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PluginTestDialog(QWidget* parent = nullptr);
    ~PluginTestDialog();

private slots:
    void onPluginSelected(int index);
    void onExecuteClicked();
    void onLoadImageClicked();
    void onClearClicked();
    void onAddExpectedClicked();
    void onRemoveExpectedClicked();

private:
    void setupUi();
    void populatePluginList();
    void loadPluginParams();
    void updateResultDisplay(const ImageData& output);

    // 创建参数输入控件
    QWidget* createParamWidget(const QString& key, const QJsonValue& value);
    QWidget* createIntWidget(const QString& key, int value, int min, int max);
    QWidget* createDoubleWidget(const QString& key, double value, double min, double max);
    QWidget* createBoolWidget(const QString& key, bool value);
    QWidget* createStringWidget(const QString& key, const QString& value);

    // 执行测试
    bool executeTest();
    void showTestResult(bool success, const QString& message);

    // 获取输入数据
    ImageData createInputData();

private:
    // UI 控件
    QComboBox* m_pluginCombo;
    QPushButton* m_refreshBtn;
    QGroupBox* m_paramsGroup;
    QFormLayout* m_paramsLayout;
    QWidget* m_paramsWidget;
    QScrollArea* m_paramsScroll;

    QPushButton* m_loadImageBtn;
    QLabel* m_imagePreview;
    QLineEdit* m_imagePathEdit;

    QPushButton* m_executeBtn;
    QPushButton* m_clearBtn;

    QGroupBox* m_expectedGroup;
    QVBoxLayout* m_expectedLayout;
    QMap<QWidget*, QPair<QString, QString>> m_expectedWidgets; // widget -> (key, value)

    QTextEdit* m_resultEdit;

    // 当前插件
    IModule* m_currentModule;
    QMap<QString, QWidget*> m_paramWidgets; // param name -> widget
    QString m_currentPluginName;
};

} // namespace DeepLux