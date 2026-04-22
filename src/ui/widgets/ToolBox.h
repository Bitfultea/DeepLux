#pragma once

#include <QWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QLineEdit>
#include <QMap>
#include <QPoint>

namespace DeepLux {

/**
 * @brief 工具箱
 * 
 * 显示可用模块列表，支持拖拽
 */
class ToolBox : public QWidget
{
    Q_OBJECT

public:
    explicit ToolBox(QWidget* parent = nullptr);
    ~ToolBox() override;

    // 添加模块分类
    void addCategory(const QString& name);
    
    // 添加模块到分类
    void addModule(const QString& category, const QString& moduleId, 
                   const QString& name, const QString& iconPath = QString());
    
    // 清空
    void clear();
    
    // 搜索过滤
    void setSearchFilter(const QString& filter);

signals:
    void moduleDragStarted(const QString& moduleId, const QString& name);
    void moduleDoubleClicked(const QString& moduleId);

private:
    void setupUi();
    void updateFilter();
    void startDrag(QTreeWidgetItem* item);
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    QVBoxLayout* m_layout = nullptr;
    QLineEdit* m_searchEdit = nullptr;
    QTreeWidget* m_tree = nullptr;

    QMap<QString, QTreeWidgetItem*> m_categories;
    QString m_filter;
    QPoint m_dragStartPos;
};

} // namespace DeepLux
