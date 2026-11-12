#ifndef DEEPLUX_AGENT_TOOL_PREVIEW_CARD_H
#define DEEPLUX_AGENT_TOOL_PREVIEW_CARD_H

#include <QJsonObject>
#include <QLabel>
#include <QList>
#include <QPushButton>
#include <QWidget>

namespace DeepLux {

/**
 * @brief Agent 计划操作预览卡片
 *
 * 显示 LLM 计划执行的工具列表，带 [确认]/[取消] 按钮。
 * 作为消息流的一部分插入，样式紧凑。
 */
class AgentToolPreviewCard : public QWidget {
    Q_OBJECT

public:
    struct ToolItem {
        QString name;
        QJsonObject params;
    };

    /**
     * @brief 阶段1 §4（审核整改 R2）：确认卡上下文——工程、目标与副作用摘要,
     * 来自 AgentController::pendingConfirmationPayload() 的 context 字段
     */
    struct ConfirmationContext {
        QString confirmationId;
        QString summary;
        QString reversibilityLabel;
        QString projectName;
        QString projectPath;
        QStringList sideEffects;
        bool isEmpty() const {
            return confirmationId.isEmpty() && summary.isEmpty() && reversibilityLabel.isEmpty() &&
                   projectName.isEmpty() && sideEffects.isEmpty();
        }
    };

    explicit AgentToolPreviewCard(const QList<ToolItem>& tools, bool isDark, QWidget* parent = nullptr,
                                  const ConfirmationContext& context = {});

    QString confirmationId() const {
        return m_context.confirmationId;
    }

    void applyTheme(bool isDark);

signals:
    void confirmed();
    void cancelled();
    void edited(int index, const QJsonObject& newParams);

private:
    void setupUi();

    QList<ToolItem> m_tools;
    bool m_isDark = false;
    ConfirmationContext m_context;
    QLabel* m_contentLabel = nullptr;
    QPushButton* m_cancelBtn = nullptr;
    QPushButton* m_confirmBtn = nullptr;
};

} // namespace DeepLux

#endif // DEEPLUX_AGENT_TOOL_PREVIEW_CARD_H
