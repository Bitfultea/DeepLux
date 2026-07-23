#pragma once

#include <QWidget>
#include <QTreeWidget>
#include <QLabel>
#include <QMap>

namespace DeepLux {

class Project;
struct DataSource;

/**
 * @brief 数据源管理面板
 *
 * 显示当前项目中已导入的所有数据源（图像 / 3D 点云）。
 * 支持双击显示、拖放、右键操作。
 */
class DataSourcePanel : public QWidget
{
    Q_OBJECT

public:
    explicit DataSourcePanel(QWidget* parent = nullptr);
    ~DataSourcePanel() override;

    // 从 Project 刷新整个列表
    void refreshFromProject(Project* project);

    // 添加/移除单个条目（用于增量更新）
    void addDataSource(const DataSource& ds);
    void removeDataSource(const QString& id);

signals:
    // 请求显示某个数据源到视口
    void requestDisplay(const QString& dataSourceId);
    // 请求从项目中删除某个数据源
    void requestRemove(const QString& dataSourceId);
    // 请求在文件夹中打开数据源文件
    void requestShowInFolder(const QString& dataSourceId);
    // 请求复制数据源文件路径
    void requestCopyPath(const QString& dataSourceId);

protected:
    void setupUi();
    void createActions();

    QTreeWidgetItem* findItem(const QString& dataSourceId) const;
    QString selectedDataSourceId() const;

private slots:
    void onItemDoubleClicked(QTreeWidgetItem* item, int column);
    void onContextMenu(const QPoint& pos);
    void onDeleteAction();
    void onShowInFolderAction();
    void onCopyPathAction();

private:
    QTreeWidget* m_treeWidget = nullptr;
    QLabel* m_emptyLabel = nullptr;
    QAction* m_deleteAction = nullptr;
    QAction* m_showInFolderAction = nullptr;
    QAction* m_copyPathAction = nullptr;
};

} // namespace DeepLux
