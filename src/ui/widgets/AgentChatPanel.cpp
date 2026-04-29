#include "AgentChatPanel.h"
#include "AgentMessageBubble.h"
#include "AgentToolPreviewCard.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QKeyEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QClipboard>
#include <QApplication>
#include <QLabel>
#include <QTimer>
#include <QTextBlock>
#include <QFontMetrics>

namespace DeepLux {

AgentChatPanel::AgentChatPanel(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
    setAcceptDrops(true);
}

AgentChatPanel::~AgentChatPanel() = default;

void AgentChatPanel::setupUi()
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // ── 消息滚动区 ──
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setFrameStyle(QFrame::NoFrame);

    m_messagesContainer = new QWidget();
    m_messagesLayout = new QVBoxLayout(m_messagesContainer);
    m_messagesLayout->setAlignment(Qt::AlignTop);
    m_messagesLayout->setSpacing(0);

    m_scrollArea->setWidget(m_messagesContainer);
    mainLayout->addWidget(m_scrollArea, 1);

    // ── 状态行 ──
    m_statusLabel = new QLabel(this);
    m_statusLabel->setVisible(false);
    mainLayout->addWidget(m_statusLabel);

    // ── 附件栏 ──
    m_attachmentBar = new QWidget(this);
    auto* attachLayout = new QHBoxLayout(m_attachmentBar);
    attachLayout->setContentsMargins(8, 2, 8, 0);
    attachLayout->addStretch();
    m_attachmentBar->setVisible(false);
    mainLayout->addWidget(m_attachmentBar);

    // ── 内联输入区（Claude Code 风格：单行自扩展，无独立 Send 按钮）──
    m_inputEdit = new QPlainTextEdit(this);
    m_inputEdit->setPlaceholderText("Ask the Agent...  (Enter to send, Shift+Enter for new line)");
    m_inputEdit->installEventFilter(this);

    // 初始单行高度
    QFontMetrics fm(m_inputEdit->font());
    m_lineHeight = fm.lineSpacing() + 4;  // +4 for padding
    m_inputEdit->setFixedHeight(m_lineHeight * m_inputMinLines + 8);

    // 禁止滚动条 — 扩展高度直到上限才让内部滚动
    m_inputEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_inputEdit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    connect(m_inputEdit, &QPlainTextEdit::textChanged,
            this, &AgentChatPanel::onInputChanged);

    mainLayout->addWidget(m_inputEdit);

    // ── Thinking 超时 ──
    m_thinkingTimer = new QTimer(this);
    m_thinkingTimer->setSingleShot(true);
    connect(m_thinkingTimer, &QTimer::timeout, this, &AgentChatPanel::onThinkingTimeout);

    applyTheme(false);
}

void AgentChatPanel::onInputChanged()
{
    updateInputHeight();
}

void AgentChatPanel::updateInputHeight()
{
    int docLines = qMax(1, m_inputEdit->document()->lineCount());
    int lines = qBound(m_inputMinLines, docLines, m_inputMaxLines);

    int newHeight = m_lineHeight * lines + 8;

    if (newHeight != m_inputEdit->height()) {
        m_inputEdit->setFixedHeight(newHeight);

        // 超出上限时启用内部滚动条
        bool needScroll = (docLines > m_inputMaxLines);
        m_inputEdit->setVerticalScrollBarPolicy(
            needScroll ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    }
}

void AgentChatPanel::insertMessage(AgentMessageBubble* bubble)
{
    int stretchIdx = -1;
    for (int i = 0; i < m_messagesLayout->count(); ++i) {
        if (m_messagesLayout->itemAt(i)->spacerItem()) {
            stretchIdx = i;
            break;
        }
    }
    if (stretchIdx >= 0) {
        m_messagesLayout->insertWidget(stretchIdx, bubble);
    } else {
        m_messagesLayout->addWidget(bubble);
    }
    if (stretchIdx < 0) {
        m_messagesLayout->addStretch();
    }
}

void AgentChatPanel::addMessage(AgentMessageBubble::Sender sender, const QString& text)
{
    auto* bubble = new AgentMessageBubble(sender, text, m_isDark, m_messagesContainer);
    insertMessage(bubble);

    if (sender == AgentMessageBubble::Sender::Agent) {
        m_lastAgentBubble = bubble;
    }
    scrollToBottom();
}

void AgentChatPanel::appendToLastMessage(const QString& text)
{
    if (m_lastAgentBubble) {
        m_lastAgentBubble->appendText(text);
        scrollToBottom();
    }
}

void AgentChatPanel::addImageAttachment(const QPixmap& pixmap)
{
    m_imageAttachments.append(pixmap);

    auto* thumb = new QLabel(m_attachmentBar);
    thumb->setPixmap(pixmap.scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    thumb->setFrameShape(QLabel::Box);
    auto* layout = qobject_cast<QHBoxLayout*>(m_attachmentBar->layout());
    if (layout) {
        layout->insertWidget(layout->count() - 1, thumb);
    }
    m_attachmentBar->setVisible(true);
}

void AgentChatPanel::clearImageAttachments()
{
    m_imageAttachments.clear();
    auto* layout = qobject_cast<QHBoxLayout*>(m_attachmentBar->layout());
    if (layout) {
        while (layout->count() > 1) {
            QLayoutItem* item = layout->takeAt(0);
            if (item && item->widget()) {
                item->widget()->deleteLater();
            }
            delete item;
        }
    }
    m_attachmentBar->setVisible(false);
}

void AgentChatPanel::showToolPreview(const QList<AgentToolPreviewCard::ToolItem>& tools)
{
    clearToolPreview();
    m_previewCard = new AgentToolPreviewCard(tools, m_isDark, m_messagesContainer);

    int stretchIdx = -1;
    for (int i = 0; i < m_messagesLayout->count(); ++i) {
        if (m_messagesLayout->itemAt(i)->spacerItem()) {
            stretchIdx = i;
            break;
        }
    }
    if (stretchIdx >= 0) {
        m_messagesLayout->insertWidget(stretchIdx, m_previewCard);
    } else {
        m_messagesLayout->addWidget(m_previewCard);
        m_messagesLayout->addStretch();
    }

    connect(m_previewCard, &AgentToolPreviewCard::confirmed,
            this, &AgentChatPanel::toolPreviewConfirmed);
    connect(m_previewCard, &AgentToolPreviewCard::cancelled,
            this, &AgentChatPanel::toolPreviewCancelled);

    scrollToBottom();
}

void AgentChatPanel::clearToolPreview()
{
    if (m_previewCard) {
        m_previewCard->deleteLater();
        m_previewCard = nullptr;
    }
}

void AgentChatPanel::setThinking(bool thinking)
{
    m_isThinkingTimeout = false;
    m_statusLabel->setVisible(thinking);
    if (thinking) {
        ChatTheme theme = m_isDark ? ChatTheme::dark() : ChatTheme::light();
        m_statusLabel->setStyleSheet(QString(
            "color: %1; font-size: 11px; padding: 2px 8px;"
        ).arg(theme.statusColor.name()));
        m_statusLabel->setText("● Agent is thinking...");
        m_thinkingTimer->start(60000);
    } else {
        m_thinkingTimer->stop();
    }
}

void AgentChatPanel::onThinkingTimeout()
{
    m_isThinkingTimeout = true;
    ChatTheme theme = m_isDark ? ChatTheme::dark() : ChatTheme::light();
    m_statusLabel->setStyleSheet(QString(
        "color: %1; font-size: 11px; padding: 2px 8px;"
    ).arg(theme.errorColor.name()));
    m_statusLabel->setText("⚠️ Request timed out");
    QTimer::singleShot(5000, this, [this]() {
        if (m_isThinkingTimeout) {
            m_statusLabel->setVisible(false);
            m_isThinkingTimeout = false;
        }
    });
}

void AgentChatPanel::onSendClicked()
{
    QString text = m_inputEdit->toPlainText().trimmed();
    if (text.isEmpty() && m_imageAttachments.isEmpty()) return;

    if (!text.isEmpty()) {
        addMessage(AgentMessageBubble::Sender::User, text);
    }
    if (!m_imageAttachments.isEmpty()) {
        addMessage(AgentMessageBubble::Sender::User,
                   QString("[Image x%1]").arg(m_imageAttachments.size()));
        emit userMessageWithImagesSent(text, m_imageAttachments);
    } else {
        emit userMessageSent(text);
    }
    m_inputEdit->clear();
    clearImageAttachments();
}

void AgentChatPanel::scrollToBottom()
{
    QScrollBar* bar = m_scrollArea->verticalScrollBar();
    bar->setValue(bar->maximum());
}

bool AgentChatPanel::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == m_inputEdit && event->type() == QEvent::KeyPress) {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);

        // 图片粘贴检测
        if (keyEvent->matches(QKeySequence::Paste)) {
            QClipboard* clipboard = QApplication::clipboard();
            const QMimeData* mimeData = clipboard->mimeData();
            if (mimeData->hasImage()) {
                QPixmap pixmap = qvariant_cast<QPixmap>(mimeData->imageData());
                if (!pixmap.isNull()) {
                    addImageAttachment(pixmap);
                    return true;
                }
            }
            // 纯文本粘贴：放行给 QPlainTextEdit
        }

        // Enter 发送，Shift+Enter 换行
        if ((keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter)
            && !(keyEvent->modifiers() & Qt::ShiftModifier)) {
            onSendClicked();
            return true;
        }
    }
    return QWidget::eventFilter(obj, event);
}

void AgentChatPanel::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasImage() || event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    }
}

void AgentChatPanel::dropEvent(QDropEvent* event)
{
    const QMimeData* mimeData = event->mimeData();
    if (mimeData->hasImage()) {
        QPixmap pixmap = qvariant_cast<QPixmap>(mimeData->imageData());
        if (!pixmap.isNull()) {
            addImageAttachment(pixmap);
        }
    } else if (mimeData->hasUrls()) {
        for (const QUrl& url : mimeData->urls()) {
            QString path = url.toLocalFile();
            if (!path.isEmpty()) {
                QPixmap pixmap(path);
                if (!pixmap.isNull()) {
                    addImageAttachment(pixmap);
                }
            }
        }
    }
}

void AgentChatPanel::applyTheme(bool isDark)
{
    m_isDark = isDark;
    ChatTheme theme = isDark ? ChatTheme::dark() : ChatTheme::light();

    // 面板整体背景
    {
        QPalette pal = palette();
        pal.setColor(QPalette::Window, theme.windowBg);
        setPalette(pal);
        setAutoFillBackground(true);
    }

    if (m_scrollArea && m_scrollArea->viewport()) {
        QPalette vpPal;
        vpPal.setColor(QPalette::Window, theme.windowBg);
        m_scrollArea->viewport()->setPalette(vpPal);
        m_scrollArea->viewport()->setAutoFillBackground(true);
    }

    if (m_messagesContainer) {
        QPalette msgPal;
        msgPal.setColor(QPalette::Window, theme.windowBg);
        m_messagesContainer->setPalette(msgPal);
        m_messagesContainer->setAutoFillBackground(true);
    }

    if (m_statusLabel) {
        m_statusLabel->setStyleSheet(QString(
            "color: %1; font-size: 11px; padding: 2px 8px;"
        ).arg(theme.statusColor.name()));
    }

    // 输入框：融入面板背景，无独立边框盒，仅顶部细线分隔
    if (m_inputEdit) {
        m_inputEdit->setStyleSheet(QString(
            "QPlainTextEdit {"
            "  background-color: %1;"
            "  color: %2;"
            "  border: none;"
            "  border-top: 1px solid %3;"
            "  padding: 4px 8px;"
            "  font-size: 13px;"
            "}"
        ).arg(theme.windowBg.name()).arg(theme.textFg.name()).arg(theme.inputBorder.name()));
    }

    if (m_messagesLayout) {
        for (int i = 0; i < m_messagesLayout->count(); ++i) {
            QLayoutItem* item = m_messagesLayout->itemAt(i);
            if (!item) continue;
            if (auto* bubble = qobject_cast<AgentMessageBubble*>(item->widget())) {
                bubble->applyTheme(isDark);
            }
            if (auto* preview = qobject_cast<AgentToolPreviewCard*>(item->widget())) {
                preview->applyTheme(isDark);
            }
        }
    }
}

} // namespace DeepLux
