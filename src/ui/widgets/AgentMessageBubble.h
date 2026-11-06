#ifndef DEEPLUX_AGENT_MESSAGE_BUBBLE_H
#define DEEPLUX_AGENT_MESSAGE_BUBBLE_H

#include <QColor>
#include <QDateTime>
#include <QLabel>
#include <QPixmap>
#include <QWidget>

class QTextBrowser;
class QToolButton;

namespace DeepLux {

struct ChatTheme {
    QColor windowBg;
    QColor inputBg;
    QColor inputBorder;
    QColor userFg;   // User 消息正文颜色
    QColor agentFg;  // Agent 消息正文颜色
    QColor systemFg; // System 消息正文颜色
    QColor toolFg;   // Tool 消息正文颜色
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

    ChatTheme(const QColor& windowBg, const QColor& inputBg, const QColor& inputBorder, const QColor& userFg,
              const QColor& agentFg, const QColor& systemFg, const QColor& toolFg, const QColor& userName,
              const QColor& agentName, const QColor& systemName, const QColor& toolName, const QColor& codeBlockBg,
              const QColor& codeBlockFg, const QColor& inlineCodeBg, const QColor& inlineCodeFg,
              const QColor& linkColor, const QColor& statusColor, const QColor& timestampColor,
              const QColor& errorColor);

    static ChatTheme dark();
    static ChatTheme light();
};

/**
 * @brief 单条聊天消息，包含角色、Markdown 正文和图片附件。
 *
 * 无独立背景盒，透明融入面板底色。
 * 左侧色条和标题区分说话者角色。
 */
class AgentMessageBubble : public QWidget {
    Q_OBJECT

public:
    enum class Sender { User, Agent, System, Tool };

    explicit AgentMessageBubble(Sender sender, const QString& text, bool isDark, QWidget* parent = nullptr);

    void setText(const QString& text);
    void appendText(const QString& text);
    QString text() const;
    void applyTheme(bool isDark);
    void setImages(const QList<QPixmap>& images);

    // ---- reasoning 折叠块（设计 §5.3；不得改既有方法签名）----
    void appendReasoning(const QString& text);  // 流式追加
    void setReasoning(const QString& text);     // 一次性设置（非流式/历史回放）
    void setReasoningStreaming(bool streaming); // 标题在「思考中…」与「已思考（N 字）」间切换
    QString reasoningText() const;              // 测试断言

    static QString markdownToHtml(const QString& md, bool isDark);
    static QString markdownToHtml(const QString& md, bool isDark, const QColor& textColor);

private:
    void setupUi();
    void renderHeader(const ChatTheme& theme);
    void renderReasoning();
    QColor senderFg(const ChatTheme& theme) const;

    Sender m_sender;
    QWidget* m_accentBar = nullptr;           // 左侧色条
    QLabel* m_headerLabel = nullptr;          // 角色名 + 时间
    QToolButton* m_reasoningToggle = nullptr; // reasoning 折叠标题
    QLabel* m_reasoningLabel = nullptr;       // reasoning 正文（只读、等宽、默认折叠）
    QTextBrowser* m_bodyLabel = nullptr;
    QToolButton* m_copyButton = nullptr;
    QWidget* m_imageGallery = nullptr;
    QString m_rawText;
    QString m_reasoningText;
    bool m_reasoningStreaming = false;
    QDateTime m_timestamp;
    bool m_isDark = false;
};

} // namespace DeepLux

#endif // DEEPLUX_AGENT_MESSAGE_BUBBLE_H
