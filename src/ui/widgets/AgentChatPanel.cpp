#include "AgentChatPanel.h"
#include "AgentMessageBubble.h"
#include "AgentToolPreviewCard.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QKeyEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QClipboard>
#include <QApplication>
#include <QLabel>
#include <QDebug>

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
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // Scroll area for messages
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_messagesContainer = new QWidget();
    m_messagesLayout = new QVBoxLayout(m_messagesContainer);
    m_messagesLayout->setAlignment(Qt::AlignTop);
    m_messagesLayout->addStretch();

    m_scrollArea->setWidget(m_messagesContainer);
    mainLayout->addWidget(m_scrollArea, 1);

    // Image attachment bar
    m_attachmentBar = new QWidget(this);
    auto* attachLayout = new QHBoxLayout(m_attachmentBar);
    attachLayout->setContentsMargins(4, 2, 4, 2);
    attachLayout->addStretch();
    m_attachmentBar->setVisible(false);
    mainLayout->addWidget(m_attachmentBar);

    // Input area
    auto* inputLayout = new QHBoxLayout();
    m_inputEdit = new QLineEdit(this);
    m_inputEdit->setPlaceholderText("Ask Agent to help you... (Ctrl+V to paste image)");
    m_sendButton = new QPushButton("Send", this);
    inputLayout->addWidget(m_inputEdit, 1);
    inputLayout->addWidget(m_sendButton);
    mainLayout->addLayout(inputLayout);

    connect(m_sendButton, &QPushButton::clicked, this, &AgentChatPanel::onSendClicked);
    connect(m_inputEdit, &QLineEdit::returnPressed, this, &AgentChatPanel::onInputReturnPressed);

    // 默认应用浅色主题
    applyTheme(false);
}

void AgentChatPanel::addMessage(AgentMessageBubble::Sender sender, const QString& text)
{
    auto* bubble = new AgentMessageBubble(sender, text, m_messagesContainer);
    int count = m_messagesLayout->count();
    m_messagesLayout->insertWidget(count - 1, bubble);

    QScrollBar* bar = m_scrollArea->verticalScrollBar();
    bar->setValue(bar->maximum());
}

void AgentChatPanel::addImageAttachment(const QPixmap& pixmap)
{
    m_imageAttachments.append(pixmap);

    QLabel* thumb = new QLabel(m_attachmentBar);
    thumb->setPixmap(pixmap.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation));
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
    m_previewCard = new AgentToolPreviewCard(tools, m_messagesContainer);
    int count = m_messagesLayout->count();
    m_messagesLayout->insertWidget(count - 1, m_previewCard);

    connect(m_previewCard, &AgentToolPreviewCard::confirmed,
            this, &AgentChatPanel::toolPreviewConfirmed);
    connect(m_previewCard, &AgentToolPreviewCard::cancelled,
            this, &AgentChatPanel::toolPreviewCancelled);
}

void AgentChatPanel::clearToolPreview()
{
    if (m_previewCard) {
        m_previewCard->deleteLater();
        m_previewCard = nullptr;
    }
}

void AgentChatPanel::onSendClicked()
{
    QString text = m_inputEdit->text().trimmed();
    if (text.isEmpty() && m_imageAttachments.isEmpty()) return;

    if (!text.isEmpty()) {
        addMessage(AgentMessageBubble::Sender::User, text);
    }
    if (!m_imageAttachments.isEmpty()) {
        addMessage(AgentMessageBubble::Sender::User,
                   QString("[Image attachment x%1]").arg(m_imageAttachments.size()));
        emit userMessageWithImagesSent(text, m_imageAttachments);
    } else {
        emit userMessageSent(text);
    }
    m_inputEdit->clear();
    clearImageAttachments();
}

void AgentChatPanel::onInputReturnPressed()
{
    onSendClicked();
}

void AgentChatPanel::keyPressEvent(QKeyEvent* event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        if (event->key() == Qt::Key_V) {
            QClipboard* clipboard = QApplication::clipboard();
            const QMimeData* mimeData = clipboard->mimeData();
            if (mimeData->hasImage()) {
                QPixmap pixmap = qvariant_cast<QPixmap>(mimeData->imageData());
                if (!pixmap.isNull()) {
                    addImageAttachment(pixmap);
                    return;
                }
            }
        }
    }
    QWidget::keyPressEvent(event);
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
    QColor bg = isDark ? QColor("#252525") : QColor("#ffffff");
    QColor text = isDark ? QColor("#ffffff") : QColor("#212121");
    QColor inputBg = isDark ? QColor("#333333") : QColor("#ffffff");
    QColor inputBorder = isDark ? QColor("#555555") : QColor("#cccccc");

    // 主面板背景
    QPalette mainPal = palette();
    mainPal.setColor(QPalette::Window, bg);
    setPalette(mainPal);
    setAutoFillBackground(true);

    // ScrollArea viewport 背景
    if (m_scrollArea) {
        m_scrollArea->setAutoFillBackground(true);
        QPalette saPal = m_scrollArea->palette();
        saPal.setColor(QPalette::Window, bg);
        m_scrollArea->setPalette(saPal);

        if (m_scrollArea->viewport()) {
            m_scrollArea->viewport()->setAutoFillBackground(true);
            m_scrollArea->viewport()->setPalette(saPal);
        }
    }

    // 消息容器背景
    if (m_messagesContainer) {
        m_messagesContainer->setAutoFillBackground(true);
        QPalette msgPal = m_messagesContainer->palette();
        msgPal.setColor(QPalette::Window, bg);
        m_messagesContainer->setPalette(msgPal);
    }

    // 附件栏背景
    if (m_attachmentBar) {
        m_attachmentBar->setAutoFillBackground(true);
        QPalette attachPal = m_attachmentBar->palette();
        attachPal.setColor(QPalette::Window, bg);
        m_attachmentBar->setPalette(attachPal);
    }

    // 输入框
    if (m_inputEdit) {
        m_inputEdit->setStyleSheet(QString("QLineEdit { background-color: %1; color: %2; border: 1px solid %3; padding: 5px; }")
                                   .arg(inputBg.name(), text.name(), inputBorder.name()));
    }

    // 发送按钮
    if (m_sendButton) {
        m_sendButton->setStyleSheet(QString("QPushButton { background-color: #0078d7; color: white; padding: 5px 12px; border: none; }"
                                            "QPushButton:hover { background-color: #1e8ad6; }"));
    }
}

} // namespace DeepLux
