#ifndef DEEPLUX_AGENT_MESSAGE_BUBBLE_H
#define DEEPLUX_AGENT_MESSAGE_BUBBLE_H

#include <QWidget>
#include <QLabel>
#include <QColor>
#include <QDateTime>

namespace DeepLux {

struct ChatTheme {
    QColor windowBg;
    QColor inputBg;
    QColor inputBorder;
    QColor userFg;          // User 消息正文颜色
    QColor agentFg;         // Agent 消息正文颜色
    QColor systemFg;        // System 消息正文颜色
    QColor toolFg;          // Tool 消息正文颜色
    QColor userName;
    QColor agentName;
    QColor systemName;
    QColor toolName;
    QColor codeBlockBg;
    QColor codeBlockFg;
    QColor inlineCodeBg;
    QColor inlineCodeFg;
    QColor linkColor;
    QColor statusColor;
    QColor timestampColor;
    QColor errorColor;

    ChatTheme(const QColor& windowBg,
              const QColor& inputBg, const QColor& inputBorder,
              const QColor& userFg, const QColor& agentFg,
              const QColor& systemFg, const QColor& toolFg,
              const QColor& userName, const QColor& agentName,
              const QColor& systemName, const QColor& toolName,
              const QColor& codeBlockBg, const QColor& codeBlockFg,
              const QColor& inlineCodeBg, const QColor& inlineCodeFg,
              const QColor& linkColor, const QColor& statusColor,
              const QColor& timestampColor, const QColor& errorColor);

    static ChatTheme dark();
    static ChatTheme light();
};

/**
 * @brief 单条聊天消息 — 终端风格
 *
 * 无独立背景盒，透明融入面板底色。
 * 仅靠左侧 3px 色条区分说话者角色。
 */
class AgentMessageBubble : public QWidget
{
    Q_OBJECT

public:
    enum class Sender { User, Agent, System, Tool };

    explicit AgentMessageBubble(Sender sender, const QString& text, bool isDark, QWidget* parent = nullptr);

    void setText(const QString& text);
    void appendText(const QString& text);
    QString text() const;
    void applyTheme(bool isDark);

    static QString markdownToHtml(const QString& md, bool isDark);
    static QString markdownToHtml(const QString& md, bool isDark, const QColor& textColor);

private:
    void setupUi();
    void renderHeader(const ChatTheme& theme);
    QColor senderFg(const ChatTheme& theme) const;
    static QString renderInlineMarkdown(const QString& text, const ChatTheme& theme);
    static QString escapeHtml(const QString& text);

    Sender m_sender;
    QWidget* m_accentBar = nullptr;     // 左侧色条
    QLabel* m_headerLabel = nullptr;    // 角色名 + 时间
    QLabel* m_bodyLabel = nullptr;      // 消息正文
    QString m_rawText;
    QDateTime m_timestamp;
    bool m_isDark = false;
};

} // namespace DeepLux

#endif // DEEPLUX_AGENT_MESSAGE_BUBBLE_H
