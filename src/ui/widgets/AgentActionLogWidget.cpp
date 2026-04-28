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
    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels(
        QStringList() << "Time" << "Actor" << "Action" << "Result" << "Params");
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);

    auto* btnLayout = new QHBoxLayout();
    m_undoButton = new QPushButton("Undo", this);
    m_clearButton = new QPushButton("Clear", this);
    btnLayout->addStretch();
    btnLayout->addWidget(m_undoButton);
    btnLayout->addWidget(m_clearButton);

    mainLayout->addWidget(m_table);
    mainLayout->addLayout(btnLayout);

    connect(m_undoButton, &QPushButton::clicked, this, &AgentActionLogWidget::onUndoClicked);
    connect(m_clearButton, &QPushButton::clicked, this, &AgentActionLogWidget::onClearClicked);
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

} // namespace DeepLux
