#include "ToolBox.h"
#include <QDrag>
#include <QMimeData>
#include <QHeaderView>
#include <QMouseEvent>
#include <QApplication>

namespace DeepLux {

ToolBox::ToolBox(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
}

ToolBox::~ToolBox()
{
}

void ToolBox::setupUi()
{
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(4, 4, 4, 4);
    m_layout->setSpacing(4);
    
    // 搜索框
    m_searchEdit = new QLineEdit();
    m_searchEdit->setPlaceholderText(tr("搜索模块..."));
    m_searchEdit->setClearButtonEnabled(true);
    m_layout->addWidget(m_searchEdit);
    
    connect(m_searchEdit, &QLineEdit::textChanged, this, &ToolBox::setSearchFilter);
    
    // 树形列表
    m_tree = new QTreeWidget();
    m_tree->setHeaderHidden(true);
    m_tree->setDragEnabled(true);
    m_tree->setAnimated(true);
    m_tree->setFrameShape(QFrame::NoFrame);
    m_layout->addWidget(m_tree);
    
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        QString moduleId = item->data(0, Qt::UserRole).toString();
        if (!moduleId.isEmpty()) {
            emit moduleDoubleClicked(moduleId);
        }
    });

    m_tree->setMouseTracking(true);
    m_tree->viewport()->installEventFilter(this);
    
    // 初始化默认分类
    addCategory(tr("图像处理"));
    addCategory(tr("检测识别"));
    addCategory(tr("坐标标定"));
    addCategory(tr("逻辑控制"));
    addCategory(tr("通讯"));
    addCategory(tr("深度学习"));
    
    // 添加示例模块
    addModule(tr("图像处理"), "grabimage", tr("图像采集"));
    addModule(tr("图像处理"), "saveimage", tr("保存图像"));
    addModule(tr("检测识别"), "qrcode", tr("二维码检测"));
    addModule(tr("检测识别"), "measureline", tr("测量线"));
    addModule(tr("检测识别"), "measurerect", tr("测量矩形"));
    addModule(tr("坐标标定"), "npointcal", tr("N点标定"));
    addModule(tr("坐标标定"), "coordinate", tr("坐标转换"));
    addModule(tr("逻辑控制"), "condition", tr("条件判断"));
    addModule(tr("逻辑控制"), "loop", tr("循环"));
    addModule(tr("逻辑控制"), "delay", tr("延时"));
    addModule(tr("通讯"), "tcpserver", tr("TCP服务端"));
    addModule(tr("通讯"), "tcpclient", tr("TCP客户端"));
    addModule(tr("深度学习"), "yolo", tr("YOLO检测"));
}

bool ToolBox::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == m_tree->viewport() && event->type() == QEvent::MouseButtonPress) {
        QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
        QTreeWidgetItem* item = m_tree->itemAt(mouseEvent->pos());
        if (item && item->parent()) {  // Only for module items, not categories
            m_dragStartPos = mouseEvent->pos();
        }
    } else if (obj == m_tree->viewport() && event->type() == QEvent::MouseMove) {
        QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);
        if (!m_dragStartPos.isNull() &&
            (mouseEvent->pos() - m_dragStartPos).manhattanLength() > QApplication::startDragDistance()) {
            QTreeWidgetItem* item = m_tree->itemAt(m_dragStartPos);
            if (item && item->parent()) {
                startDrag(item);
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

void ToolBox::startDrag(QTreeWidgetItem* item)
{
    QString moduleId = item->data(0, Qt::UserRole).toString();
    QString name = item->text(0);

    QDrag* drag = new QDrag(this);
    QMimeData* mimeData = new QMimeData();

    // Format: moduleId|name
    mimeData->setText(QString("%1|%2").arg(moduleId).arg(name));
    drag->setMimeData(mimeData);

    m_dragStartPos = QPoint();  // Reset
    drag->exec(Qt::CopyAction);
}

void ToolBox::addCategory(const QString& name)
{
    if (!m_categories.contains(name)) {
        QTreeWidgetItem* item = new QTreeWidgetItem(m_tree, QStringList(name));
        item->setExpanded(true);
        QFont font = item->font(0);
        font.setBold(true);
        item->setFont(0, font);
        m_categories[name] = item;
    }
}

void ToolBox::addModule(const QString& category, const QString& moduleId,
                        const QString& name, const QString& iconPath)
{
    Q_UNUSED(iconPath)
    
    if (!m_categories.contains(category)) {
        addCategory(category);
    }
    
    QTreeWidgetItem* parentItem = m_categories[category];
    QTreeWidgetItem* item = new QTreeWidgetItem(parentItem, QStringList(name));
    item->setData(0, Qt::UserRole, moduleId);
    item->setToolTip(0, tr("拖拽到画布添加模块"));
}

void ToolBox::clear()
{
    m_tree->clear();
    m_categories.clear();
}

void ToolBox::setSearchFilter(const QString& filter)
{
    m_filter = filter.toLower();
    updateFilter();
}

void ToolBox::updateFilter()
{
    // 遍历所有项目
    for (int i = 0; i < m_tree->topLevelItemCount(); i++) {
        QTreeWidgetItem* category = m_tree->topLevelItem(i);
        bool hasVisibleChild = false;
        
        for (int j = 0; j < category->childCount(); j++) {
            QTreeWidgetItem* module = category->child(j);
            QString name = module->text(0).toLower();
            QString id = module->data(0, Qt::UserRole).toString().toLower();
            
            bool visible = m_filter.isEmpty() || 
                          name.contains(m_filter) || 
                          id.contains(m_filter);
            
            module->setHidden(!visible);
            if (visible) hasVisibleChild = true;
        }
        
        category->setHidden(!hasVisibleChild && !m_filter.isEmpty());
        if (hasVisibleChild) {
            category->setExpanded(true);
        }
    }
}

} // namespace DeepLux
