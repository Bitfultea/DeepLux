#include "AgentMessageBubble.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QRegularExpression>

namespace DeepLux {

// ========== ChatTheme ==========

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
        QColor("#1e1e1e"), QColor("#d4d4d4"), QColor("#1e1e1e"), QColor("#444444"),
        QColor("#569cd6"), QColor("#4ec9b0"), QColor("#888888"), QColor("#c586c0"),
        QColor("#0d0d0d"), QColor("#d4d4d4"),
        QColor("#2d2d2d"), QColor("#d4d4d4"),
        QColor("#569cd6"), QColor("#888888"), QColor("#666666"), QColor("#f44747")
    );
}

ChatTheme ChatTheme::light()
{
    return ChatTheme(
        QColor("#ffffff"), QColor("#333333"), QColor("#ffffff"), QColor("#cccccc"),
        QColor("#0078d7"), QColor("#10a37f"), QColor("#888888"), QColor("#7c3aed"),
        QColor("#f4f4f4"), QColor("#333333"),
        QColor("#f0f0f0"), QColor("#333333"),
        QColor("#0078d7"), QColor("#888888"), QColor("#aaaaaa"), QColor("#d32f2f")
    );
}

// ========== AgentMessageBubble — 终端风格：无背景盒，仅左侧色条区分角色 ==========

AgentMessageBubble::AgentMessageBubble(Sender sender, const QString& text, bool isDark, QWidget* parent)
    : QWidget(parent)
    , m_sender(sender)
    , m_timestamp(QDateTime::currentDateTime())
    , m_isDark(isDark)
{
    setupUi();
    setText(text);
}

void AgentMessageBubble::setupUi()
{
    auto* hLayout = new QHBoxLayout(this);
    hLayout->setContentsMargins(4, 1, 8, 1);
    hLayout->setSpacing(6);

    // 左侧色条：2px 宽，充满行高
    m_accentBar = new QWidget(this);
    m_accentBar->setFixedWidth(3);
    m_accentBar->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    hLayout->addWidget(m_accentBar);

    // 正文区
    auto* contentLayout = new QVBoxLayout();
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);

    // 角色名 + 时间戳：内联单行
    m_headerLabel = new QLabel(this);
    m_headerLabel->setTextFormat(Qt::RichText);
    m_headerLabel->setStyleSheet("font-size:10px;");
    contentLayout->addWidget(m_headerLabel);

    // 消息正文
    m_bodyLabel = new QLabel(this);
    m_bodyLabel->setTextFormat(Qt::RichText);
    m_bodyLabel->setWordWrap(true);
    m_bodyLabel->setOpenExternalLinks(true);
    m_bodyLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    contentLayout->addWidget(m_bodyLabel);

    hLayout->addLayout(contentLayout, 1);

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

    // 透明背景 — 融入面板底色
    setStyleSheet("background: transparent;");

    // 左侧色条
    QColor accent;
    QString roleTag;
    switch (m_sender) {
    case Sender::User:   accent = theme.userName;   roleTag = "You";   break;
    case Sender::Agent:  accent = theme.agentName;  roleTag = "Agent"; break;
    case Sender::System: accent = theme.systemName; roleTag = "Sys";   break;
    case Sender::Tool:   accent = theme.toolName;   roleTag = "Tool";  break;
    }
    m_accentBar->setStyleSheet(QString("background-color: %1; border-radius: 1px;").arg(accent.name()));

    // 角色标签
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

    // 紧凑内联 header：角色名 | 时间戳
    QString header = QString(
        "<span style=\"color:%1;font-weight:bold;\">%2</span>"
        "&nbsp;<span style=\"color:%3;\">%4</span>"
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

    // 先提取代码块，内部原封不动
    QStringList parts;
    int pos = 0;

    static QRegularExpression codeBlockRe(
        "```(\\w*)\\n?([\\s\\S]*?)\\n?```");

    QRegularExpressionMatchIterator it = codeBlockRe.globalMatch(md);
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        if (match.capturedStart() > pos) {
            parts.append(renderInlineMarkdown(md.mid(pos, match.capturedStart() - pos), theme));
        }
        QString code = escapeHtml(match.captured(2));
        parts.append(QString(
            "<pre style=\"background:%1;color:%2;padding:6px 8px;border-radius:3px;margin:2px 0;"
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

    // 透明背景，融入面板 — 无独立背景色盒
    return QString("<div style=\"font-size:13px;line-height:1.4;color:%1;\">%2</div>")
           .arg(theme.textFg.name())
           .arg(parts.join(""));
}

QString AgentMessageBubble::renderInlineMarkdown(const QString& text, const ChatTheme& theme)
{
    QString html = text;
    html.replace("&", "&amp;");
    html.replace("<", "&lt;");
    html.replace(">", "&gt;");

    html.replace(QRegularExpression("\\*\\*(.+?)\\*\\*"), "<b>\\1</b>");
    html.replace(QRegularExpression("\\b_(.+?)_\\b"), "<i>\\1</i>");

    html.replace(QRegularExpression("`([^`]+)`"),
        QString("<code style=\"background:%1;color:%2;padding:1px 3px;border-radius:2px;"
                "font-family:Consolas,Monaco,'Courier New',monospace;font-size:12px;\">\\1</code>")
        .arg(theme.inlineCodeBg.name())
        .arg(theme.inlineCodeFg.name()));

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
