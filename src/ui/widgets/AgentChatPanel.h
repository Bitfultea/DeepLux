#ifndef DEEPLUX_AGENT_CHAT_PANEL_H
#define DEEPLUX_AGENT_CHAT_PANEL_H

#include <QList>
#include <QPixmap>
#include <QPointer>
#include <QWidget>

class QPlainTextEdit;
class QScrollArea;
class QVBoxLayout;
class QLabel;
class QTimer;
class QToolButton;

#include "AgentMessageBubble.h"
#include "AgentToolPreviewCard.h"

namespace DeepLux {

/**
 * @brief Agent 对话面板
 *
 * 单列阅读布局、Markdown 渲染和带图片附件的自适应输入。
 */
class AgentChatPanel : public QWidget {
    Q_OBJECT

public:
    explicit AgentChatPanel(QWidget* parent = nullptr);
    ~AgentChatPanel() override;

    void addMessage(AgentMessageBubble::Sender sender, const QString& text, const QList<QPixmap>& images = {});
    void appendToLastMessage(const QString& text);

    // 批3 复核二轮（P2-7）/四轮（P2-5）：附件数量上限与 AgentController
    // 附件限额对齐——在源头拦截最常见的"发送后被拒、草稿已清"路径
    static constexpr int kMaxImageAttachments = 4;

    void addImageAttachment(const QPixmap& pixmap);
    // 阶段1 审核整改 R2：确认卡携带载荷上下文（工程/摘要/副作用/confirmationId）
    void showToolPreview(const QList<AgentToolPreviewCard::ToolItem>& tools,
                         const AgentToolPreviewCard::ConfirmationContext& context = {});
    void clearToolPreview();
    void clearImageAttachments();
    QList<QPixmap> imageAttachments() const {
        return m_imageAttachments;
    }
    // 控制器拒绝发送（未初始化/无客户端/模型不支持图片/附件超限等）时由
    // 调用方回填草稿，用户可原样重试——onSendClicked 为乐观 UI，已先行清空输入
    void restoreDraft(const QString& text, const QList<QPixmap>& images);
    // 当前输入框草稿文本（供测试断言回填）
    QString inputDraftText() const;
    // 批3 复核六轮（P2）：附件超限提示（达到上限被拒时置位并经状态条展示，
    // 成功添加/清空附件后复位；访问器供测试断言）
    QString attachmentNotice() const {
        return m_attachmentNotice;
    }

    void setThinking(bool thinking);
    void setError(const QString& error);

    // Shared by the latest-message button and GUI-only Agent navigation tool.
    bool canScrollToLatest() const;
    bool scrollToLatest(int& scrollValue, int& scrollMaximum);

    void applyTheme(bool isDark);

signals:
    void userMessageSent(const QString& message);
    void userMessageWithImagesSent(const QString& message, const QList<QPixmap>& images);
    void cancelRequested();
    // 阶段1 审核整改 R2：批准/拒绝携带展示时的 confirmationId——
    // 控制器校验其与当前待确认批次一致,旧批次迟到事件被拒绝（零副作用）
    void toolPreviewConfirmed(const QString& confirmationId);
    void toolPreviewCancelled(const QString& confirmationId);

private slots:
    void onSendClicked();
    void onInputChanged();
    void onThinkingTimeout();
    void scrollToBottom();

private:
    void setupUi();
    void updateInputHeight();
    void updateStatusStrip();
    void insertMessage(AgentMessageBubble* bubble);
    void rebuildAttachments();
    void updateSendButton();

    bool eventFilter(QObject* obj, QEvent* event) override;

    QWidget* m_statusStrip = nullptr;
    QPointer<QScrollArea> m_scrollArea;
    QPointer<QWidget> m_messagesContainer;
    QPointer<QVBoxLayout> m_messagesLayout;
    QPlainTextEdit* m_inputEdit = nullptr;
    QPointer<AgentToolPreviewCard> m_previewCard;
    QList<QPixmap> m_imageAttachments;
    QString m_attachmentNotice; // 批3 复核六轮（P2）：附件超限提示（空=无提示）
    QWidget* m_attachmentBar = nullptr;
    QLabel* m_statusLabel = nullptr;
    QLabel* m_messageCountLabel = nullptr;
    QLabel* m_toolCountLabel = nullptr;
    QLabel* m_attachmentCountLabel = nullptr;
    QTimer* m_thinkingTimer = nullptr;
    QToolButton* m_sendButton = nullptr;
    QToolButton* m_attachButton = nullptr;
    QPointer<QToolButton> m_latestButton;
    QPointer<QLabel> m_emptyLabel;
    bool m_followTail = true;
    bool m_composing = false;
    QString m_error;

    QPointer<AgentMessageBubble> m_lastAgentBubble;
    bool m_isDark = false;
    bool m_isThinkingTimeout = false;
    bool m_isThinking = false;
    int m_messageCount = 0;
    int m_pendingToolCount = 0;

    int m_lineHeight = 20; // 单行像素高度
    int m_inputMinLines = 1;
    int m_inputMaxLines = 6;

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
};

} // namespace DeepLux

#endif // DEEPLUX_AGENT_CHAT_PANEL_H
