#ifndef DEEPLUX_AGENT_CHAT_PANEL_H
#define DEEPLUX_AGENT_CHAT_PANEL_H

#include <QWidget>
#include <QList>
#include <QPixmap>

class QLineEdit;
class QPushButton;
class QScrollArea;
class QVBoxLayout;
class QLabel;

#include "AgentMessageBubble.h"
#include "AgentToolPreviewCard.h"

namespace DeepLux {

/**
 * @brief Agent 聊天主面板
 *
 * 消息气泡 + 输入框 + 操作预览卡片
 */
class AgentChatPanel : public QWidget
{
    Q_OBJECT

public:
    explicit AgentChatPanel(QWidget* parent = nullptr);
    ~AgentChatPanel() override;

    void addMessage(AgentMessageBubble::Sender sender, const QString& text);
    void addImageAttachment(const QPixmap& pixmap);
    void showToolPreview(const QList<AgentToolPreviewCard::ToolItem>& tools);
    void clearToolPreview();
    void clearImageAttachments();
    QList<QPixmap> imageAttachments() const { return m_imageAttachments; }

    void applyTheme(bool isDark);

signals:
    void userMessageSent(const QString& message);
    void userMessageWithImagesSent(const QString& message, const QList<QPixmap>& images);
    void toolPreviewConfirmed();
    void toolPreviewCancelled();

private slots:
    void onSendClicked();
    void onInputReturnPressed();

private:
    void setupUi();

    QScrollArea* m_scrollArea = nullptr;
    QWidget* m_messagesContainer = nullptr;
    QVBoxLayout* m_messagesLayout = nullptr;
    QLineEdit* m_inputEdit = nullptr;
    QPushButton* m_sendButton = nullptr;
    AgentToolPreviewCard* m_previewCard = nullptr;
    QList<QPixmap> m_imageAttachments;
    QWidget* m_attachmentBar = nullptr;

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
};

} // namespace DeepLux

#endif // DEEPLUX_AGENT_CHAT_PANEL_H
