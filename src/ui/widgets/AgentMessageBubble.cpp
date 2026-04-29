#include "AgentMessageBubble.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QRegularExpression>

namespace DeepLux {

// ========== ChatTheme 命名工厂 — 每个字段显式命名，消除位置初始化误读 ==========

ChatTheme::ChatTheme(const QColor& windowBg, const QColor& textFg,
                     const QColor& inputBg, const QColor& inputBorder,
                     const QColor& userName, const QColor& agentName,
                     const QColor& systemName, const QColor& toolName,
                     const QColor& codeBlockBg, const QColor& codeBlockFg,
                     const QColor& inlineCodeBg, const QColor& inlineCodeFg,
                     const QColor& linkColor, const QColor& statusColor,
                     const QColor& timestampColor, const QColor& errorColor)
    : windowBg(windowBg), textFg(textFg)
    , inputBg(inputBg), inputBorder(inputBorder)
    , userName(userName), agentName(agentName)
    , systemName(systemName), toolName(toolName)
    , codeBlockBg(codeBlockBg), codeBlockFg(codeBlockFg)
    , inlineCodeBg(inlineCodeBg), inlineCodeFg(inlineCodeFg)
    , linkColor(linkColor), statusColor(statusColor)
    , timestampColor(timestampColor), errorColor(errorColor)
{}

ChatTheme ChatTheme::dark()
{
    return ChatTheme(
        QColor("#1e1e1e"), QColor("#ffffff"), QColor("#1e1e1e"), QColor("#555555"),
        QColor("#2b8dda"), QColor("#19c59f"), QColor("#aaaaaa"), QColor("#8b5cf6"),
        QColor("#0d0d0d"), QColor("#d4d4d4"),
        QColor("#2d2d2d"), QColor("#ffffff"),
        QColor("#58a6ff"), QColor("#888888"), QColor("#888888"), QColor("#e74c3c")
    );
}

ChatTheme ChatTheme::light()
{
    return ChatTheme(
        QColor("#ffffff"), QColor("#1a1a1a"), QColor("#ffffff"), QColor("#dddddd"),
        QColor("#0078d7"), QColor("#10a37f"), QColor("#888888"), QColor("#7c3aed"),
        QColor("#f4f4f4"), QColor("#333333"),
        QColor("#f0f0f0"), QColor("#1a1a1a"),
        QColor("#0078d7"), QColor("#666666"), QColor("#888888"), QColor("#e74c3c")
    );
}

// ========== AgentMessageBubble ==========

AgentMessageBubble::AgentMessageBubble(Sender sender, const QString& text, bool isDark, QWidget* parent)
    : QWidget(parent)
    , m_sender(sender)
    , m_timestamp(QDateTime::currentDateTime())
    , m_isDark(isDark)
{
    setupUi();
    setText(text);         // 通过 m_rawText → markdownToHtml 渲染
}

void AgentMessageBubble::setupUi()
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 0, 8, 0);
    layout->setSpacing(0);

    // Header: 独立 widget，不使用不可靠的 float:right
    m_headerLabel = new QLabel(this);
    m_headerLabel->setTextFormat(Qt::RichText);
    m_headerLabel->setStyleSheet("font-size:11px;");
    layout->addWidget(m_headerLabel);

    // Body
    m_bodyLabel = new QLabel(this);
    m_bodyLabel->setTextFormat(Qt::RichText);
    m_bodyLabel->setWordWrap(true);
    m_bodyLabel->setOpenExternalLinks(true);
    m_bodyLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    layout->addWidget(m_bodyLabel);

    applyTheme(m_isDark);
}

void AgentMessageBubble::setText(const QString& text)
{
    m_rawText = text;
    ChatTheme theme = m_isDark ? ChatTheme::dark() : ChatTheme::light();
    renderHeader(theme);
    if (m_bodyLabel) {
        m_bodyLabel->setText(markdownToHtml(text, m_isDark));
    }
}

void AgentMessageBubble::appendText(const QString& text)
{
    // 始终从 m_rawText 全量重新渲染，不依赖 HTML 字符串拼接
    m_rawText += text;
    if (m_bodyLabel) {
        m_bodyLabel->setText(markdownToHtml(m_rawText, m_isDark));
    }
}

QString AgentMessageBubble::text() const
{
    return m_rawText;
}

void AgentMessageBubble::applyTheme(bool isDark)
{
    m_isDark = isDark;
    ChatTheme theme = isDark ? ChatTheme::dark() : ChatTheme::light();
    renderHeader(theme);
    if (!m_rawText.isEmpty() && m_bodyLabel) {
        m_bodyLabel->setText(markdownToHtml(m_rawText, isDark));
    }
}

void AgentMessageBubble::renderHeader(const ChatTheme& theme)
{
    QColor nameColor;
    QString name;
    switch (m_sender) {
    case Sender::User:   nameColor = theme.userName;  name = "You"; break;
    case Sender::Agent:  nameColor = theme.agentName; name = "Assistant"; break;
    case Sender::System: nameColor = theme.systemName; name = "System"; break;
    case Sender::Tool:   nameColor = theme.toolName;  name = "Tool"; break;
    }

    QString time = m_timestamp.toString("hh:mm:ss");

    // 用 <table> 实现左右对齐，Qt RichText 稳定支持
    QString header = QString(
        "<table width=\"100%%\"><tr>"
        "<td align=\"left\"><span style=\"color:%1;font-weight:bold;\">%2</span></td>"
        "<td align=\"right\"><span style=\"color:%3;\">%4</span></td>"
        "</tr></table>"
    ).arg(nameColor.name()).arg(name)
     .arg(theme.timestampColor.name()).arg(time);

    if (m_headerLabel) {
        m_headerLabel->setText(header);
    }
}

// ========== Markdown → HTML ==========

QString AgentMessageBubble::markdownToHtml(const QString& md, bool isDark)
{
    ChatTheme theme = isDark ? ChatTheme::dark() : ChatTheme::light();

    // 1. 先提取代码块边界，代码块内部原封不动
    QStringList parts;
    int pos = 0;

    static QRegularExpression codeBlockRe(
        "```(\\w*)\\n?([\\s\\S]*?)\\n?```");  // [\s\S] 跨行匹配

    QRegularExpressionMatchIterator it = codeBlockRe.globalMatch(md);
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        // 代码块前的普通文本
        if (match.capturedStart() > pos) {
            parts.append(renderInlineMarkdown(md.mid(pos, match.capturedStart() - pos), theme));
        }
        // 代码块：内容 HTML 转义但不做 markdown 处理
        QString code = escapeHtml(match.captured(2));
        parts.append(QString(
            "<pre style=\"background:%1;color:%2;padding:4px 6px;border-radius:3px;"
            "font-family:Consolas,Monaco,'Courier New',monospace;font-size:12px;\">"
            "<code>%3</code></pre>"
        ).arg(theme.codeBlockBg.name())
         .arg(theme.codeBlockFg.name())
         .arg(code));
        pos = match.capturedEnd();
    }
    if (pos < md.length()) {
        parts.append(renderInlineMarkdown(md.mid(pos), theme));
    }

    return QString("<div style=\"font-size:13px;line-height:1.3;color:%1;\">%2</div>")
           .arg(theme.textFg.name())
           .arg(parts.join(""));
}

QString AgentMessageBubble::renderInlineMarkdown(const QString& text, const ChatTheme& theme)
{
    QString html = text;
    html.replace("&", "&amp;");
    html.replace("<", "&lt;");
    html.replace(">", "&gt;");

    // Bold / italic
    html.replace(QRegularExpression("\\*\\*(.+?)\\*\\*"), "<b>\\1</b>");
    html.replace(QRegularExpression("\\b_(.+?)_\\b"), "<i>\\1</i>");

    // Inline code
    html.replace(QRegularExpression("`([^`]+)`"),
        QString("<code style=\"background:%1;color:%2;padding:1px 3px;border-radius:2px;"
                "font-family:Consolas,Monaco,'Courier New',monospace;font-size:12px;\">\\1</code>")
        .arg(theme.inlineCodeBg.name())
        .arg(theme.inlineCodeFg.name()));

    // Headings
    html.replace(QRegularExpression("^## (.+)$", QRegularExpression::MultilineOption),
                 "<b style=\"font-size:14px;\">\\1</b>");
    html.replace(QRegularExpression("^### (.+)$", QRegularExpression::MultilineOption),
                 "<b style=\"font-size:13px;\">\\1</b>");

    // Lists
    QStringList lines = html.split('\n');
    QStringList resultLines;
    bool inList = false;
    static QRegularExpression listRe("^\\s*[-\\*]\\s+(.+)$");
    for (const QString& line : lines) {
        QRegularExpressionMatch m = listRe.match(line);
        if (m.hasMatch()) {
            if (!inList) { resultLines.append("<ul style=\"margin:2px 0;padding-left:16px;\">"); inList = true; }
            resultLines.append(QString("<li>%1</li>").arg(m.captured(1)));
        } else {
            if (inList) { resultLines.append("</ul>"); inList = false; }
            resultLines.append(line);
        }
    }
    if (inList) resultLines.append("</ul>");
    html = resultLines.join("\n");

    // Links
    html.replace(QRegularExpression("\\[(.+?)\\]\\((.+?)\\)"),
        QString("<a href=\"\\2\" style=\"color:%1;\">\\1</a>").arg(theme.linkColor.name()));
    html.replace(QRegularExpression("(https?://[^\\s<>]+)"),
        QString("<a href=\"\\1\" style=\"color:%1;\">\\1</a>").arg(theme.linkColor.name()));

    html.replace("\n", "<br>");
    return html;
}

QString AgentMessageBubble::escapeHtml(const QString& text)
{
    QString result = text;
    result.replace("&", "&amp;");
    result.replace("<", "&lt;");
    result.replace(">", "&gt;");
    return result;
}

} // namespace DeepLux
