#pragma once

#include <QWidget>
#include <QString>

class QLineEdit;
class QSpinBox;
class QDoubleSpinBox;
class QComboBox;
class QCheckBox;
class QLabel;

/**
 * @brief 配置控件工厂 - 提供统一风格的主题感知控件
 */
class ConfigWidgetFactory
{
public:
    /**
     * @brief 获取全局实例
     */
    static ConfigWidgetFactory& instance();

    /**
     * @brief 设置深色主题模式
     */
    void setDarkTheme(bool dark);

    /**
     * @brief 是否为深色主题
     */
    bool isDarkTheme() const { return m_darkTheme; }

    // ========== 控件创建方法 ==========

    /**
     * @brief 创建标签
     */
    QLabel* createLabel(const QString& text, QWidget* parent = nullptr);

    /**
     * @brief 创建单行输入框
     */
    QLineEdit* createLineEdit(const QString& text = QString(), QWidget* parent = nullptr);

    /**
     * @brief 创建整数输入框
     */
    QSpinBox* createSpinBox(int min = 0, int max = 999999, int value = 0, QWidget* parent = nullptr);

    /**
     * @brief 创建浮点数输入框
     */
    QDoubleSpinBox* createDoubleSpinBox(double min = 0.0, double max = 999999.0, double value = 0.0, int decimals = 3, QWidget* parent = nullptr);

    /**
     * @brief 创建下拉框
     */
    QComboBox* createComboBox(QWidget* parent = nullptr);

    /**
     * @brief 创建复选框
     */
    QCheckBox* createCheckBox(const QString& text = QString(), bool checked = false, QWidget* parent = nullptr);

    /**
     * @brief 应用样式到控件
     */
    void applyStyle(QWidget* widget);

    /**
     * @brief 获取标准间距
     */
    int standardSpacing() const { return 8; }

    /**
     * @brief 获取标准边距
     */
    int standardMargin() const { return 12; }

private:
    ConfigWidgetFactory();
    ~ConfigWidgetFactory() = default;
    ConfigWidgetFactory(const ConfigWidgetFactory&) = delete;
    ConfigWidgetFactory& operator=(const ConfigWidgetFactory&) = delete;

    bool m_darkTheme = true;  // 默认深色主题
};
