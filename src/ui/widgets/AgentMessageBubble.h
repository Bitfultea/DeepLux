#ifndef DEEPLUX_AGENT_MESSAGE_BUBBLE_H
#define DEEPLUX_AGENT_MESSAGE_BUBBLE_H

#include <QWidget>
#include <QLabel>
#include <QColor>
#include <QDateTime>

namespace DeepLux {

struct ChatTheme {
    QColor windowBg;
    QColor textFg;
    QColor inputBg;
    QColor inputBorder;
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

    // 命名构造函数 — 必须通过 factory 创建，杜绝位置初始化
    ChatTheme(const QColor& windowBg, const QColor& textFg,
              const QColor& inputBg, const QColor& inputBorder,
              const QColor& userName, const QColor& agentName,
              const QColor& systemName, const QColor& toolName,
              const QColor& codeBlockBg, const QColor& codeBlockFg,
              const QColor& inlineCodeBg, const QColor& inlineCodeFg,
              const QColor& linkColor, const QColor& statusColor,
              const QColor& timestampColor, const QColor& errorColor);

    static ChatTheme dark();
    static ChatTheme light();
};

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

private:
    void setupUi();
    void renderHeader(const ChatTheme& theme);
    static QString renderInlineMarkdown(const QString& text, const ChatTheme& theme);
    static QString escapeHtml(const QString& text);

    Sender m_sender;
    QLabel* m_headerLabel = nullptr;
    QLabel* m_bodyLabel = nullptr;
    QString m_rawText;
    QDateTime m_timestamp;
    bool m_isDark = false;
};

} // namespace DeepLux

#endif // DEEPLUX_AGENT_MESSAGE_BUBBLE_H
