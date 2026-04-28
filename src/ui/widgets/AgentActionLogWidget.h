#ifndef DEEPLUX_AGENT_ACTION_LOG_WIDGET_H
#define DEEPLUX_AGENT_ACTION_LOG_WIDGET_H

#include <QWidget>
#include <QList>

#include "core/agent/GuiEvent.h"

class QTableWidget;
class QPushButton;

namespace DeepLux {

/**
 * @brief Agent Action Log Widget
 *
 * 显示 Agent 所有操作的时间线，支持撤销。
 * 放置在 MainWindow 的 dock 区域或独立面板。
 */
class AgentActionLogWidget : public QWidget
{
    Q_OBJECT

public:
    explicit AgentActionLogWidget(QWidget* parent = nullptr);
    ~AgentActionLogWidget() override;

    void addEntry(const AgentActionLogEntry& entry);
    void updateEntryResult(int row, const QString& result);

    QList<AgentActionLogEntry> entries() const { return m_entries; }

signals:
    void undoRequested(int row);
    void clearRequested();

private slots:
    void onUndoClicked();
    void onClearClicked();

private:
    void setupUi();

    QTableWidget* m_table = nullptr;
    QPushButton* m_undoButton = nullptr;
    QPushButton* m_clearButton = nullptr;

    QList<AgentActionLogEntry> m_entries;
};

} // namespace DeepLux

#endif // DEEPLUX_AGENT_ACTION_LOG_WIDGET_H
