#ifndef DEEPLUX_AGENT_TOOL_PREVIEW_CARD_H
#define DEEPLUX_AGENT_TOOL_PREVIEW_CARD_H

#include <QWidget>
#include <QJsonObject>
#include <QList>

namespace DeepLux {

/**
 * @brief Agent 计划操作预览卡片
 *
 * 显示 LLM 计划执行的工具列表，带 [确认]/[取消]/[编辑] 按钮。
 */
class AgentToolPreviewCard : public QWidget
{
    Q_OBJECT

public:
    struct ToolItem {
        QString name;
        QJsonObject params;
    };

    explicit AgentToolPreviewCard(const QList<ToolItem>& tools, QWidget* parent = nullptr);

signals:
    void confirmed();
    void cancelled();
    void edited(int index, const QJsonObject& newParams);

private:
    void setupUi();

    QList<ToolItem> m_tools;
};

} // namespace DeepLux

#endif // DEEPLUX_AGENT_TOOL_PREVIEW_CARD_H
