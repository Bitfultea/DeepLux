#include "AgentChatPanel.h"
#include "AgentMessageBubble.h"
#include "AgentToolPreviewCard.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QKeyEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QClipboard>
#include <QApplication>
#include <QLabel>
#include <QTimer>

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

    // 消息滚动区
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

    // 状态指示器（默认隐藏）
    m_statusLabel = new QLabel(this);
    m_statusLabel->setVisible(false);
    mainLayout->addWidget(m_statusLabel);

    // 附件栏
    m_attachmentBar = new QWidget(this);
    auto* attachLayout = new QHBoxLayout(m_attachmentBar);
    attachLayout->setContentsMargins(8, 2, 8, 0);
    attachLayout->addStretch();
    m_attachmentBar->setVisible(false);
    mainLayout->addWidget(m_attachmentBar);

    // 底部输入区
    auto* bottomLayout = new QHBoxLayout();
    bottomLayout->setContentsMargins(8, 4, 8, 4);
    bottomLayout->setSpacing(6);

    m_inputEdit = new QTextEdit(this);
    m_inputEdit->setPlaceholderText("Ask the Agent... (Shift+Enter for new line)");
    m_inputEdit->setMinimumHeight(28);
    m_inputEdit->setMaximumHeight(120);  // ~5 行，支持真正多行输入
    m_inputEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_inputEdit->installEventFilter(this);

    m_sendButton = new QPushButton("Send", this);
    m_sendButton->setFixedWidth(50);
    m_sendButton->setFixedHeight(28);

    bottomLayout->addWidget(m_inputEdit, 1);
    bottomLayout->addWidget(m_sendButton);
    mainLayout->addLayout(bottomLayout);

    connect(m_sendButton, &QPushButton::clicked, this, &AgentChatPanel::onSendClicked);

    m_thinkingTimer = new QTimer(this);
    m_thinkingTimer->setSingleShot(true);
    connect(m_thinkingTimer, &QTimer::timeout, this, &AgentChatPanel::onThinkingTimeout);

    applyTheme(false);
}

void AgentChatPanel::addMessage(AgentMessageBubble::Sender sender, const QString& text)
{
    auto* bubble = new AgentMessageBubble(sender, text, m_isDark, m_messagesContainer);

    // 用 layout item count 找到 stretch 位置，插入到 stretch 之前
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

    // 确保底部有 stretch
    if (stretchIdx < 0) {
        m_messagesLayout->addStretch();
    }

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

    QLabel* thumb = new QLabel(m_attachmentBar);
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

        if (keyEvent->matches(QKeySequence::Paste)) {
            QClipboard* clipboard = QApplication::clipboard();
            const QMimeData* mimeData = clipboard->mimeData();
            if (mimeData->hasImage()) {
                QPixmap pixmap = qvariant_cast<QPixmap>(mimeData->imageData());
                if (!pixmap.isNull()) {
                    addImageAttachment(pixmap);
                    return true;  // 图片粘贴已处理
                }
            }
            // 纯文本粘贴：放行给 QTextEdit 自行处理
        }

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

    // 整个面板统一背景
    QPalette pal = palette();
    pal.setColor(QPalette::Window, theme.windowBg);
    setPalette(pal);
    setAutoFillBackground(true);

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

    if (m_inputEdit) {
        m_inputEdit->setStyleSheet(QString(
            "QTextEdit {"
            "  background-color: %1;"
            "  color: %2;"
            "  border: 1px solid %3;"
            "  border-radius: 3px;"
            "  padding: 4px;"
            "  font-size: 13px;"
            "}"
        ).arg(theme.inputBg.name()).arg(theme.textFg.name()).arg(theme.inputBorder.name()));
    }

    if (m_sendButton) {
        m_sendButton->setStyleSheet(QString(
            "QPushButton {"
            "  background-color: #0078d7;"
            "  color: white;"
            "  border-radius: 3px;"
            "  font-size: 12px;"
            "  font-weight: bold;"
            "}"
            "QPushButton:hover { background-color: #1e8ad6; }"
            "QPushButton:pressed { background-color: #005a9e; }"
        ));
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
