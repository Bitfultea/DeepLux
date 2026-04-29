#ifndef DEEPLUX_AGENT_CHAT_PANEL_H
#define DEEPLUX_AGENT_CHAT_PANEL_H

#include <QWidget>
#include <QList>
#include <QPixmap>

class QTextEdit;
class QPushButton;
class QScrollArea;
class QVBoxLayout;
class QLabel;
class QTimer;

#include "AgentMessageBubble.h"
#include "AgentToolPreviewCard.h"

namespace DeepLux {

/**
 * @brief Agent 聊天主面板 —— Claude Code 风格
 *
 * 全宽流式单列布局 + Markdown 渲染 + 多行输入 + 状态指示
 */
class AgentChatPanel : public QWidget
{
    Q_OBJECT

public:
    explicit AgentChatPanel(QWidget* parent = nullptr);
    ~AgentChatPanel() override;

    void addMessage(AgentMessageBubble::Sender sender, const QString& text);
    void appendToLastMessage(const QString& text);
    void addImageAttachment(const QPixmap& pixmap);
    void showToolPreview(const QList<AgentToolPreviewCard::ToolItem>& tools);
    void clearToolPreview();
    void clearImageAttachments();
    QList<QPixmap> imageAttachments() const { return m_imageAttachments; }

    void setThinking(bool thinking);

    void applyTheme(bool isDark);

signals:
    void userMessageSent(const QString& message);
    void userMessageWithImagesSent(const QString& message, const QList<QPixmap>& images);
    void toolPreviewConfirmed();
    void toolPreviewCancelled();

private slots:
    void onSendClicked();
    void onThinkingTimeout();

private:
    void setupUi();
    void scrollToBottom();

    bool eventFilter(QObject* obj, QEvent* event) override;

    QScrollArea* m_scrollArea = nullptr;
    QWidget* m_messagesContainer = nullptr;
    QVBoxLayout* m_messagesLayout = nullptr;
    QTextEdit* m_inputEdit = nullptr;
    QPushButton* m_sendButton = nullptr;
    AgentToolPreviewCard* m_previewCard = nullptr;
    QList<QPixmap> m_imageAttachments;
    QWidget* m_attachmentBar = nullptr;
    QLabel* m_statusLabel = nullptr;
    QTimer* m_thinkingTimer = nullptr;

    AgentMessageBubble* m_lastAgentBubble = nullptr;
    bool m_isDark = false;
    bool m_isThinkingTimeout = false;  // 超时标志，避免字符串匹配

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
};

} // namespace DeepLux

#endif // DEEPLUX_AGENT_CHAT_PANEL_H
