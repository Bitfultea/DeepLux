#include "AgentActionLogWidget.h"

#include <QTableWidget>
#include <QTableWidgetItem>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QHeaderView>
#include <QDateTime>

namespace DeepLux {

AgentActionLogWidget::AgentActionLogWidget(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
}

AgentActionLogWidget::~AgentActionLogWidget() = default;

void AgentActionLogWidget::setupUi()
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    m_table = new QTableWidget(this);
    m_table->setObjectName("AgentActionLogTable");
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels(
        QStringList() << tr("时间") << tr("主体") << tr("动作") << tr("结果") << tr("参数"));
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    const int tableTextHeight = m_table->fontMetrics().height();
    m_table->horizontalHeader()->setMinimumHeight(qMax(30, tableTextHeight + 10));
    m_table->verticalHeader()->setDefaultSectionSize(qMax(26, tableTextHeight + 6));

    auto* btnLayout = new QHBoxLayout();
    m_undoButton = new QPushButton(tr("撤销最近操作"), this);
    m_clearButton = new QPushButton(tr("清空"), this);
    const int buttonHeight = qMax(28, m_undoButton->fontMetrics().height() + 8);
    m_undoButton->setMinimumHeight(buttonHeight);
    m_clearButton->setMinimumHeight(buttonHeight);
    btnLayout->addStretch();
    btnLayout->addWidget(m_undoButton);
    btnLayout->addWidget(m_clearButton);

    mainLayout->addWidget(m_table);
    mainLayout->addLayout(btnLayout);

    connect(m_undoButton, &QPushButton::clicked, this, &AgentActionLogWidget::onUndoClicked);
    connect(m_clearButton, &QPushButton::clicked, this, &AgentActionLogWidget::onClearClicked);

    applyTheme(false);
}

void AgentActionLogWidget::addEntry(const AgentActionLogEntry& entry)
{
    int row = m_table->rowCount();
    m_table->insertRow(row);
    m_entries.append(entry);

    m_table->setItem(row, 0, new QTableWidgetItem(entry.timestamp.toString("hh:mm:ss")));
    m_table->setItem(row, 1, new QTableWidgetItem(entry.actor));
    m_table->setItem(row, 2, new QTableWidgetItem(entry.action));
    m_table->setItem(row, 3, new QTableWidgetItem(entry.result));
    m_table->setItem(row, 4, new QTableWidgetItem(entry.params));

    m_table->scrollToBottom();
}

void AgentActionLogWidget::updateEntryResult(int row, const QString& result)
{
    if (row < 0 || row >= m_table->rowCount()) return;
    m_table->setItem(row, 3, new QTableWidgetItem(result));
    if (row < m_entries.size()) {
        m_entries[row].result = result;
    }
}

void AgentActionLogWidget::onUndoClicked()
{
    int row = m_table->currentRow();
    if (row >= 0) {
        emit undoRequested(row);
    }
}

void AgentActionLogWidget::onClearClicked()
{
    m_table->setRowCount(0);
    m_entries.clear();
    emit clearRequested();
}

void AgentActionLogWidget::applyTheme(bool isDark)
{
    const QString bg = isDark ? "#252525" : "#ffffff";
    const QString altBg = isDark ? "#2d2d2d" : "#f8f8f8";
    const QString headerBg = isDark ? "#333333" : "#f0f0f0";
    const QString fg = isDark ? "#f3f4f6" : "#212121";
    const QString border = isDark ? "#3a3a3a" : "#dddddd";

    setStyleSheet(QString("background-color: %1; color: %2;").arg(bg, fg));
    m_table->setStyleSheet(QString(
        "QTableWidget { background-color: %1; alternate-background-color: %2; color: %3; border: none;"
        " gridline-color: %4; }"
        "QTableWidget::item { color: %3; border-bottom: 1px solid %4; }"
        "QTableWidget::item:selected { background-color: #0078d7; color: #ffffff; }"
        "QHeaderView::section { background-color: %5; color: %3; padding: 5px; border: none; }"
        "QTableCornerButton::section { background-color: %5; border: none; }")
        .arg(bg, altBg, fg, border, headerBg));

    const QString buttonStyle =
        QString("QPushButton { background-color: #0078d7; color: white; padding: 4px 12px; border: none; }"
                "QPushButton:hover { background-color: #1e8ad6; }"
                "QPushButton:pressed { background-color: #005a9e; }");
    m_undoButton->setStyleSheet(buttonStyle);
    m_clearButton->setStyleSheet(buttonStyle);
}

} // namespace DeepLux
