#include "DataSourcePanel.h"
#include "core/model/Project.h"
#include "core/model/DataSource.h"
#include "core/common/Logger.h"

#include <QVBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QDateTime>
#include <QDesktopServices>
#include <QClipboard>
#include <QGuiApplication>
#include <QFileInfo>

namespace DeepLux {

DataSourcePanel::DataSourcePanel(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
    createActions();
}

DataSourcePanel::~DataSourcePanel() = default;

void DataSourcePanel::setupUi()
{
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    m_treeWidget = new QTreeWidget(this);
    m_treeWidget->setHeaderHidden(true);
    m_treeWidget->setDragEnabled(true);
    m_treeWidget->setSelectionMode(QAbstractItemView::SingleSelection);
    m_treeWidget->setContextMenuPolicy(Qt::CustomContextMenu);
    m_treeWidget->setIndentation(0);  // 扁平列表，无缩进

    layout->addWidget(m_treeWidget);

    connect(m_treeWidget, &QTreeWidget::itemDoubleClicked,
            this, &DataSourcePanel::onItemDoubleClicked);
    connect(m_treeWidget, &QTreeWidget::customContextMenuRequested,
            this, &DataSourcePanel::onContextMenu);
}

void DataSourcePanel::createActions()
{
    m_deleteAction = new QAction(tr("删除"), this);
    connect(m_deleteAction, &QAction::triggered, this, &DataSourcePanel::onDeleteAction);

    m_showInFolderAction = new QAction(tr("在文件夹中打开"), this);
    connect(m_showInFolderAction, &QAction::triggered, this, &DataSourcePanel::onShowInFolderAction);

    m_copyPathAction = new QAction(tr("复制路径"), this);
    connect(m_copyPathAction, &QAction::triggered, this, &DataSourcePanel::onCopyPathAction);
}

void DataSourcePanel::refreshFromProject(Project* project)
{
    m_treeWidget->clear();
    if (!project) return;
    for (const DataSource& ds : project->dataSources()) {
        addDataSource(ds);
    }
}

void DataSourcePanel::addDataSource(const DataSource& ds)
{
    QStringList columns;
    columns << ds.name;

    // 附加信息列（tooltip）
    QString tooltip = tr("路径: %1\n导入时间: %2")
                          .arg(ds.filePath)
                          .arg(QDateTime::fromMSecsSinceEpoch(ds.importTime).toString("yyyy-MM-dd hh:mm:ss"));
    if (ds.metadata.contains("pointCount")) {
        columns << QString("%1 点").arg(ds.metadata["pointCount"].toLongLong());
        tooltip += QString("\n点数: %1").arg(ds.metadata["pointCount"].toLongLong());
    }
    if (ds.metadata.contains("fileSize")) {
        qint64 size = ds.metadata["fileSize"].toLongLong();
        QString sizeStr = size > 1024 * 1024
                              ? QString("%1 MB").arg(size / (1024.0 * 1024.0), 0, 'f', 1)
                              : QString("%1 KB").arg(size / 1024.0, 0, 'f', 1);
        tooltip += QString("\n大小: %1").arg(sizeStr);
    }

    QTreeWidgetItem* item = new QTreeWidgetItem(m_treeWidget, columns);
    item->setData(0, Qt::UserRole, ds.id);
    item->setToolTip(0, tooltip);
    item->setToolTip(1, tooltip);

    // 用不同图标区分类型
    if (ds.isImage()) {
        item->setIcon(0, QIcon::fromTheme("image-x-generic"));
    } else if (ds.isPointCloud()) {
        item->setIcon(0, QIcon::fromTheme("applications-graphics"));
    }
}

void DataSourcePanel::removeDataSource(const QString& id)
{
    QTreeWidgetItem* item = findItem(id);
    if (item) {
        delete item;
    }
}

QTreeWidgetItem* DataSourcePanel::findItem(const QString& dataSourceId) const
{
    for (int i = 0; i < m_treeWidget->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_treeWidget->topLevelItem(i);
        if (item->data(0, Qt::UserRole).toString() == dataSourceId) {
            return item;
        }
    }
    return nullptr;
}

QString DataSourcePanel::selectedDataSourceId() const
{
    QTreeWidgetItem* item = m_treeWidget->currentItem();
    if (!item) {
        return QString();
    }
    return item->data(0, Qt::UserRole).toString();
}

void DataSourcePanel::onItemDoubleClicked(QTreeWidgetItem* item, int /*column*/)
{
    if (!item) return;
    QString id = item->data(0, Qt::UserRole).toString();
    if (!id.isEmpty()) {
        emit requestDisplay(id);
    }
}

void DataSourcePanel::onContextMenu(const QPoint& pos)
{
    QTreeWidgetItem* item = m_treeWidget->itemAt(pos);
    if (!item) return;

    m_treeWidget->setCurrentItem(item);

    QMenu menu(this);
    menu.addAction(m_showInFolderAction);
    menu.addAction(m_copyPathAction);
    menu.addSeparator();
    menu.addAction(m_deleteAction);
    menu.exec(m_treeWidget->mapToGlobal(pos));
}

void DataSourcePanel::onDeleteAction()
{
    QString id = selectedDataSourceId();
    if (!id.isEmpty()) {
        emit requestRemove(id);
    }
}

void DataSourcePanel::onShowInFolderAction()
{
    QString id = selectedDataSourceId();
    if (!id.isEmpty()) {
        emit requestShowInFolder(id);
    }
}

void DataSourcePanel::onCopyPathAction()
{
    QString id = selectedDataSourceId();
    if (!id.isEmpty()) {
        emit requestCopyPath(id);
    }
}

} // namespace DeepLux
