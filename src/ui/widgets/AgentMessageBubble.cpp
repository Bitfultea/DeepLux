#include "AgentMessageBubble.h"

#include "AppIconProvider.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QResizeEvent>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextTable>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

namespace DeepLux {
namespace {
// Each message grows with its document; only the conversation owns a vertical scrollbar.
class MessageBody : public QTextBrowser {
public:
    explicit MessageBody(QWidget* parent) : QTextBrowser(parent) {
        setObjectName("AgentMessageBody");
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        setOpenLinks(false);
        setOpenExternalLinks(false);
        document()->setDocumentMargin(0);
        QTextOption option = document()->defaultTextOption();
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        document()->setDefaultTextOption(option);
        connect(
            document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this,
            [this](const QSizeF& size) { setFixedHeight(qMax(1, static_cast<int>(std::ceil(size.height())) + 2)); });
        connect(this, &QTextBrowser::anchorClicked, this, [](const QUrl& url) {
            if (url.scheme() == "https" || url.scheme() == "http" || url.scheme() == "mailto")
                QDesktopServices::openUrl(url);
        });
    }

protected:
    QVariant loadResource(int, const QUrl&) override {
        return {};
    }
    void resizeEvent(QResizeEvent* event) override {
        QTextBrowser::resizeEvent(event);
        document()->setTextWidth(viewport()->width());
    }
};
} // namespace

ChatTheme::ChatTheme(const QColor& windowBg, const QColor& inputBg, const QColor& inputBorder, const QColor& userFg,
                     const QColor& agentFg, const QColor& systemFg, const QColor& toolFg, const QColor& userName,
                     const QColor& agentName, const QColor& systemName, const QColor& toolName,
                     const QColor& codeBlockBg, const QColor& codeBlockFg, const QColor& inlineCodeBg,
                     const QColor& inlineCodeFg, const QColor& linkColor, const QColor& statusColor,
                     const QColor& timestampColor, const QColor& errorColor)
    : windowBg(windowBg), inputBg(inputBg), inputBorder(inputBorder), userFg(userFg), agentFg(agentFg),
      systemFg(systemFg), toolFg(toolFg), userName(userName), agentName(agentName), systemName(systemName),
      toolName(toolName), codeBlockBg(codeBlockBg), codeBlockFg(codeBlockFg), inlineCodeBg(inlineCodeBg),
      inlineCodeFg(inlineCodeFg), linkColor(linkColor), statusColor(statusColor), timestampColor(timestampColor),
      errorColor(errorColor) {}

ChatTheme ChatTheme::dark() {
    return ChatTheme(QColor("#202020"), QColor("#282828"), QColor("#404040"), QColor("#ededed"), QColor("#dedede"),
                     QColor("#bcbcbc"), QColor("#d4b777"), QColor("#66b4f2"), QColor("#6bcab1"), QColor("#bcbcbc"),
                     QColor("#d4b777"), QColor("#282828"), QColor("#e4e4e4"), QColor("#2b2b2b"), QColor("#e4e4e4"),
                     QColor("#66b4f2"), QColor("#bcbcbc"), QColor("#a0a0a0"), QColor("#ff8b8b"));
}

ChatTheme ChatTheme::light() {
    return ChatTheme(QColor("#ffffff"), QColor("#f6f7f8"), QColor("#dfe3e6"), QColor("#202428"), QColor("#30363b"),
                     QColor("#60676d"), QColor("#80601e"), QColor("#1268ad"), QColor("#087d66"), QColor("#60676d"),
                     QColor("#80601e"), QColor("#f5f6f7"), QColor("#30363b"), QColor("#f5f6f7"), QColor("#30363b"),
                     QColor("#1268ad"), QColor("#60676d"), QColor("#697077"), QColor("#bd3030"));
}

AgentMessageBubble::AgentMessageBubble(Sender sender, const QString& text, bool isDark, QWidget* parent)
    : QWidget(parent), m_sender(sender), m_timestamp(QDateTime::currentDateTime()), m_isDark(isDark) {
    setupUi();
    setText(text);
}

void AgentMessageBubble::setupUi() {
    setObjectName("AgentMessage");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 2, 0, 2);
    row->setSpacing(8);
    m_accentBar = new QWidget(this);
    m_accentBar->setFixedWidth(2);
    row->addWidget(m_accentBar);

    auto* column = new QVBoxLayout();
    column->setSpacing(2);
    auto* header = new QHBoxLayout();
    header->setSpacing(8);
    m_headerLabel = new QLabel(this);
    m_headerLabel->setTextFormat(Qt::RichText);
    header->addWidget(m_headerLabel);
    header->addStretch();
    m_copyButton = new QToolButton(this);
    m_copyButton->setObjectName("AgentCopyMessage");
    m_copyButton->setToolTip(tr("复制消息"));
    m_copyButton->setAccessibleName(tr("复制消息"));
    m_copyButton->setFixedSize(24, 24);
    m_copyButton->setAutoRaise(true);
    connect(m_copyButton, &QToolButton::clicked, this, [this]() { QApplication::clipboard()->setText(m_rawText); });
    header->addWidget(m_copyButton);
    column->addLayout(header);

    m_bodyLabel = new MessageBody(this);

    // reasoning 折叠块（设计 §5.3）：位于正文上方、默认折叠
    m_reasoningToggle = new QToolButton(this);
    m_reasoningToggle->setObjectName("AgentReasoningToggle");
    m_reasoningToggle->setCheckable(true);
    m_reasoningToggle->setChecked(false);
    m_reasoningToggle->setAutoRaise(true);
    m_reasoningToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_reasoningToggle->setVisible(false);
    connect(m_reasoningToggle, &QToolButton::toggled, this, [this](bool expanded) {
        if (m_reasoningLabel)
            m_reasoningLabel->setVisible(expanded && !m_reasoningText.isEmpty());
        renderReasoning();
    });
    column->addWidget(m_reasoningToggle);

    m_reasoningLabel = new QLabel(this);
    m_reasoningLabel->setObjectName("AgentReasoningBody");
    m_reasoningLabel->setWordWrap(true);
    m_reasoningLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_reasoningLabel->setVisible(false);
    {
        QFont mono = QApplication::font();
        mono.setStyleHint(QFont::Monospace);
        mono.setFamily(QStringLiteral("Consolas"));
        mono.setPixelSize(12);
        m_reasoningLabel->setFont(mono);
    }
    column->addWidget(m_reasoningLabel);

    column->addWidget(m_bodyLabel);
    m_imageGallery = new QWidget(this);
    auto* images = new QGridLayout(m_imageGallery);
    images->setContentsMargins(0, 0, 0, 0);
    images->setSpacing(6);
    images->setAlignment(Qt::AlignLeft);
    column->addWidget(m_imageGallery);
    m_imageGallery->hide();
    row->addLayout(column, 1);
    applyTheme(m_isDark);
}

void AgentMessageBubble::setText(const QString& text) {
    m_rawText = text;
    const ChatTheme theme = m_isDark ? ChatTheme::dark() : ChatTheme::light();
    m_bodyLabel->setHtml(markdownToHtml(text, m_isDark, senderFg(theme)));
    m_bodyLabel->setVisible(!text.isEmpty());
}

void AgentMessageBubble::appendText(const QString& text) {
    setText(m_rawText + text);
}

QString AgentMessageBubble::text() const {
    return m_rawText;
}

void AgentMessageBubble::appendReasoning(const QString& text) {
    if (text.isEmpty())
        return;
    m_reasoningText += text;
    renderReasoning();
}

void AgentMessageBubble::setReasoning(const QString& text) {
    m_reasoningText = text;
    renderReasoning();
}

void AgentMessageBubble::setReasoningStreaming(bool streaming) {
    m_reasoningStreaming = streaming;
    renderReasoning();
}

QString AgentMessageBubble::reasoningText() const {
    return m_reasoningText;
}

void AgentMessageBubble::renderReasoning() {
    if (!m_reasoningToggle || !m_reasoningLabel)
        return;
    const bool hasText = !m_reasoningText.isEmpty();
    m_reasoningToggle->setVisible(hasText);
    m_reasoningLabel->setVisible(hasText && m_reasoningToggle->isChecked());
    if (!hasText)
        return;
    const QString title = m_reasoningStreaming ? tr("思考中…") : tr("已思考（%1 字）").arg(m_reasoningText.size());
    m_reasoningToggle->setText(title);
    m_reasoningToggle->setToolTip(tr("展开/收起推理过程"));
    m_reasoningLabel->setText(m_reasoningText);
}

void AgentMessageBubble::setImages(const QList<QPixmap>& images) {
    auto* grid = qobject_cast<QGridLayout*>(m_imageGallery->layout());
    while (auto* item = grid->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (int i = 0; i < images.size(); ++i) {
        auto* thumbnail = new QLabel(m_imageGallery);
        thumbnail->setObjectName("AgentSentImage");
        thumbnail->setFixedSize(88, 64);
        thumbnail->setAlignment(Qt::AlignCenter);
        thumbnail->setPixmap(images[i].scaled(88, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        thumbnail->setAccessibleName(tr("已发送图片 %1").arg(i + 1));
        grid->addWidget(thumbnail, i / 3, i % 3);
    }
    m_imageGallery->setVisible(!images.isEmpty());
}

void AgentMessageBubble::applyTheme(bool isDark) {
    m_isDark = isDark;
    const ChatTheme theme = isDark ? ChatTheme::dark() : ChatTheme::light();
    setStyleSheet("QWidget#AgentMessage { background: transparent; }");
    const QColor accent = m_sender == Sender::User    ? theme.userName
                          : m_sender == Sender::Agent ? theme.agentName
                          : m_sender == Sender::Tool  ? theme.toolName
                                                      : theme.errorColor;
    m_accentBar->setStyleSheet(QString("background:%1;").arg(accent.name()));
    m_copyButton->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Copy, 16, theme.timestampColor));
    m_copyButton->setStyleSheet(QString("QToolButton { border:0; background:transparent; }"
                                        "QToolButton:hover,QToolButton:focus { background:%1; border-radius:4px; }")
                                    .arg(theme.inlineCodeBg.name()));
    m_bodyLabel->setStyleSheet(QString("QTextBrowser { border:0; background:transparent; color:%1; font-size:14px; }")
                                   .arg(senderFg(theme).name()));
    if (m_reasoningToggle) {
        m_reasoningToggle->setIcon(AppIconProvider::icon(
            m_reasoningToggle->isChecked() ? AppIconProvider::Icon::MoveUp : AppIconProvider::Icon::MoveDown, 14,
            theme.timestampColor));
        m_reasoningToggle->setStyleSheet(
            QString("QToolButton { border:0; background:transparent; color:%1; font-size:12px; font-weight:600; }"
                    "QToolButton:hover,QToolButton:focus { background:%2; border-radius:4px; }")
                .arg(theme.timestampColor.name(), theme.inlineCodeBg.name()));
    }
    if (m_reasoningLabel) {
        m_reasoningLabel->setStyleSheet(QString("QLabel { color:%1; background:%2; padding:6px; border-radius:4px; }")
                                            .arg(theme.timestampColor.name(), theme.codeBlockBg.name()));
    }
    renderHeader(theme);
    setText(m_rawText);
    renderReasoning();
}

void AgentMessageBubble::renderHeader(const ChatTheme& theme) {
    const QString name = m_sender == Sender::User    ? tr("你")
                         : m_sender == Sender::Agent ? QStringLiteral("Agent")
                         : m_sender == Sender::Tool  ? tr("工具")
                                                     : tr("系统");
    m_headerLabel->setText(
        QString("<span style='font-size:12px;font-weight:600;color:%1'>%2</span>"
                "&nbsp;&nbsp;<span style='font-size:11px;color:%3'>%4</span>")
            .arg(senderFg(theme).name(), name, theme.timestampColor.name(), m_timestamp.toString("hh:mm")));
}

QString AgentMessageBubble::markdownToHtml(const QString& md, bool isDark) {
    const ChatTheme theme = isDark ? ChatTheme::dark() : ChatTheme::light();
    return markdownToHtml(md, isDark, theme.agentFg);
}

QString AgentMessageBubble::markdownToHtml(const QString& md, bool isDark, const QColor& textColor) {
    const ChatTheme theme = isDark ? ChatTheme::dark() : ChatTheme::light();
    QTextDocument document;
    QFont font = QApplication::font();
    font.setPixelSize(14);
    document.setDefaultFont(font);
    document.setMarkdown(md, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub) |
                                 QTextDocument::MarkdownNoHTML);
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        QTextCursor cursor(block);
        auto format = block.blockFormat();
        format.setTopMargin(format.headingLevel() ? 4 : 0);
        format.setBottomMargin(2);
        format.setLineHeight(100, QTextBlockFormat::ProportionalHeight);
        if (format.hasProperty(QTextFormat::BlockCodeFence))
            format.setBackground(theme.codeBlockBg);
        cursor.setBlockFormat(format);
        cursor.select(QTextCursor::BlockUnderCursor);
        QTextCharFormat textFormat;
        textFormat.setForeground(textColor);
        if (format.headingLevel())
            textFormat.setFontPointSize(format.headingLevel() <= 2 ? 12 : 11);
        cursor.mergeCharFormat(textFormat);
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            const auto character = fragment.charFormat();
            if (!character.isAnchor() && !character.fontFixedPitch())
                continue;
            QTextCursor span(&document);
            span.setPosition(fragment.position());
            span.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
            QTextCharFormat style;
            style.setForeground(character.isAnchor() ? theme.linkColor : theme.codeBlockFg);
            if (character.fontFixedPitch())
                style.setBackground(theme.inlineCodeBg);
            span.mergeCharFormat(style);
        }
    }
    for (QTextFrame* frame : document.rootFrame()->childFrames()) {
        if (auto* table = qobject_cast<QTextTable*>(frame)) {
            auto format = table->format();
            format.setBorder(1);
            format.setBorderBrush(theme.inputBorder);
            format.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
            format.setCellPadding(5);
            format.setCellSpacing(0);
            table->setFormat(format);
        }
    }
    return document.toHtml();
}

QColor AgentMessageBubble::senderFg(const ChatTheme& theme) const {
    switch (m_sender) {
    case Sender::User:
        return theme.userFg;
    case Sender::Agent:
        return theme.agentFg;
    case Sender::System:
        return theme.errorColor;
    case Sender::Tool:
        return theme.toolFg;
    }
    return theme.userFg;
}

} // namespace DeepLux
