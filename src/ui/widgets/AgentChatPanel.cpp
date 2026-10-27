#include "AgentChatPanel.h"

#include "AppIconProvider.h"

#include <QApplication>
#include <QClipboard>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QTextBlock>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace DeepLux {

AgentChatPanel::AgentChatPanel(QWidget* parent) : QWidget(parent) {
    setupUi();
    setAcceptDrops(true);
}
AgentChatPanel::~AgentChatPanel() = default;

void AgentChatPanel::setupUi() {
    setObjectName("AgentChatPanel");
    auto* main = new QVBoxLayout(this);
    main->setContentsMargins(0, 0, 0, 0);
    main->setSpacing(0);

    m_statusStrip = new QWidget(this);
    m_statusStrip->setObjectName("AgentChatStatusStrip");
    m_statusStrip->setFixedHeight(24);
    auto* status = new QHBoxLayout(m_statusStrip);
    status->setContentsMargins(12, 0, 10, 0);
    status->setSpacing(12);
    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName("AgentChatStatusLabel");
    status->addWidget(m_statusLabel);
    status->addStretch();
    m_messageCountLabel = new QLabel(this);
    m_messageCountLabel->setObjectName("AgentChatMessageCountLabel");
    m_toolCountLabel = new QLabel(this);
    m_toolCountLabel->setObjectName("AgentChatToolCountLabel");
    m_attachmentCountLabel = new QLabel(this);
    m_attachmentCountLabel->setObjectName("AgentChatAttachmentCountLabel");
    status->addWidget(m_messageCountLabel);
    status->addWidget(m_toolCountLabel);
    status->addWidget(m_attachmentCountLabel);
    m_latestButton = new QToolButton(this);
    m_latestButton->setObjectName("AgentLatestMessage");
    m_latestButton->setFixedSize(24, 24);
    m_latestButton->setToolTip(tr("回到最新消息"));
    m_latestButton->setAccessibleName(tr("回到最新消息"));
    connect(m_latestButton, &QToolButton::clicked, this, &AgentChatPanel::scrollToBottom);
    status->addWidget(m_latestButton);
    m_latestButton->hide();
    main->addWidget(m_statusStrip);

    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setObjectName("AgentMessageScroll");
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setFrameStyle(QFrame::NoFrame);
    m_messagesContainer = new QWidget();
    m_messagesContainer->setObjectName("AgentMessages");
    m_messagesLayout = new QVBoxLayout(m_messagesContainer);
    m_messagesLayout->setAlignment(Qt::AlignTop);
    m_messagesLayout->setContentsMargins(8, 2, 8, 4);
    m_messagesLayout->setSpacing(2);
    m_emptyLabel = new QLabel(tr("开始新的对话"), m_messagesContainer);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setMinimumHeight(48);
    m_messagesLayout->addWidget(m_emptyLabel);
    m_messagesLayout->addStretch();
    m_scrollArea->setWidget(m_messagesContainer);
    main->addWidget(m_scrollArea, 1);
    auto* scrollbar = m_scrollArea->verticalScrollBar();
    connect(scrollbar, &QScrollBar::valueChanged, this, [this, scrollbar](int value) {
        m_followTail = value >= scrollbar->maximum() - 8;
        m_latestButton->setVisible(!m_followTail);
    });
    connect(scrollbar, &QScrollBar::rangeChanged, this, [this, scrollbar](int, int maximum) {
        if (m_followTail)
            scrollbar->setValue(maximum);
        m_latestButton->setVisible(!m_followTail);
    });

    // The attachment strip scrolls horizontally without increasing the composer height.
    auto* attachments = new QScrollArea(this);
    attachments->setObjectName("AgentAttachmentScroll");
    attachments->setFrameShape(QFrame::NoFrame);
    attachments->setWidgetResizable(true);
    attachments->setFixedHeight(70);
    attachments->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_attachmentBar = new QWidget();
    auto* attachmentLayout = new QHBoxLayout(m_attachmentBar);
    attachmentLayout->setContentsMargins(12, 4, 12, 4);
    attachmentLayout->setSpacing(6);
    attachmentLayout->addStretch();
    attachments->setWidget(m_attachmentBar);
    main->addWidget(attachments);
    attachments->hide();

    auto* composer = new QWidget(this);
    composer->setObjectName("AgentComposer");
    auto* inputLayout = new QHBoxLayout(composer);
    inputLayout->setContentsMargins(8, 5, 8, 5);
    inputLayout->setSpacing(6);
    m_attachButton = new QToolButton(composer);
    m_attachButton->setObjectName("AgentAttachImage");
    m_attachButton->setToolTip(tr("添加图片"));
    m_attachButton->setAccessibleName(tr("添加图片"));
    m_attachButton->setFixedSize(30, 30);
    connect(m_attachButton, &QToolButton::clicked, this, [this]() {
        const QStringList paths = QFileDialog::getOpenFileNames(
            this, tr("添加图片"), QString(), tr("图片 (*.png *.jpg *.jpeg *.bmp *.tif *.tiff *.webp)"));
        for (const QString& path : paths)
            addImageAttachment(QPixmap(path));
    });
    inputLayout->addWidget(m_attachButton, 0, Qt::AlignBottom);
    m_inputEdit = new QPlainTextEdit(composer);
    m_inputEdit->setObjectName("AgentMessageInput");
    m_inputEdit->setPlaceholderText(tr("输入 Agent 指令…"));
    m_inputEdit->setAccessibleName(tr("Agent 消息"));
    m_inputEdit->setToolTip(tr("Enter 发送，Shift+Enter 换行"));
    m_inputEdit->installEventFilter(this);
    m_inputEdit->setFrameShape(QFrame::NoFrame);
    m_inputEdit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_inputEdit->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_inputEdit->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    connect(m_inputEdit, &QPlainTextEdit::textChanged, this, &AgentChatPanel::onInputChanged);
    inputLayout->addWidget(m_inputEdit, 1);
    m_sendButton = new QToolButton(composer);
    m_sendButton->setObjectName("AgentSendMessage");
    m_sendButton->setFixedSize(30, 30);
    m_sendButton->setAccessibleName(tr("发送消息"));
    connect(m_sendButton, &QToolButton::clicked, this, &AgentChatPanel::onSendClicked);
    inputLayout->addWidget(m_sendButton, 0, Qt::AlignBottom);
    // 停止按钮（设计 §5.6）：仅在思考/执行中可见，点击请求控制器中止当前轮
    m_stopButton = new QToolButton(composer);
    m_stopButton->setObjectName("AgentStopMessage");
    m_stopButton->setFixedSize(30, 30);
    m_stopButton->setToolTip(tr("停止当前轮"));
    m_stopButton->setAccessibleName(tr("停止当前轮"));
    connect(m_stopButton, &QToolButton::clicked, this, [this]() {
        m_stopButton->setEnabled(false); // 连点保护：等待控制器回 agentTurnAborted
        emit stopRequested();
    });
    m_stopButton->hide();
    inputLayout->addWidget(m_stopButton, 0, Qt::AlignBottom);
    main->addWidget(composer);

    m_thinkingTimer = new QTimer(this);
    m_thinkingTimer->setSingleShot(true);
    connect(m_thinkingTimer, &QTimer::timeout, this, &AgentChatPanel::onThinkingTimeout);
    applyTheme(false);
    updateInputHeight();
    updateStatusStrip();
}

void AgentChatPanel::onInputChanged() {
    updateInputHeight();
    updateSendButton();
}

void AgentChatPanel::updateInputHeight() {
    int lines = 0;
    for (QTextBlock block = m_inputEdit->document()->begin(); block.isValid(); block = block.next())
        lines += qMax(1, block.layout()->lineCount());
    m_lineHeight = m_inputEdit->fontMetrics().lineSpacing();
    const int bounded = qBound(m_inputMinLines, lines, m_inputMaxLines);
    m_inputEdit->setFixedHeight(qMax(30, bounded * m_lineHeight + 8));
    m_inputEdit->setVerticalScrollBarPolicy(lines > m_inputMaxLines ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
}

void AgentChatPanel::insertMessage(AgentMessageBubble* bubble) {
    m_emptyLabel->hide();
    m_messagesLayout->insertWidget(m_messagesLayout->count() - 1, bubble);
}

void AgentChatPanel::addMessage(AgentMessageBubble::Sender sender, const QString& text, const QList<QPixmap>& images) {
    if (text.isEmpty() && images.isEmpty())
        return;
    auto* bubble = new AgentMessageBubble(sender, text, m_isDark, m_messagesContainer);
    bubble->setImages(images);
    insertMessage(bubble);
    ++m_messageCount;
    updateStatusStrip();
    if (sender == AgentMessageBubble::Sender::Agent)
        m_lastAgentBubble = bubble;
    QTimer::singleShot(0, this, [this]() {
        if (m_followTail)
            scrollToBottom();
    });
}

void AgentChatPanel::appendToLastMessage(const QString& text) {
    if (m_lastAgentBubble)
        m_lastAgentBubble->appendText(text);
}

void AgentChatPanel::addImageAttachment(const QPixmap& pixmap) {
    if (pixmap.isNull())
        return;
    // 批3 复核四轮（P2-5）：源头限额（与 AgentController 附件上限对齐），避免
    // "附加成功→发送被控制器拒绝→草稿已清空无法重试"
    // 批3 复核六轮（P2）：超限给出用户可见反馈（状态条提示）——静默丢弃会让
    // 控制器侧的明确错误反馈在正常 UI 路径永远不可达；提示随成功添加/清空复位
    if (m_imageAttachments.size() >= kMaxImageAttachments) {
        m_attachmentNotice = tr("最多 %1 张附件").arg(kMaxImageAttachments);
        updateStatusStrip();
        return;
    }
    m_attachmentNotice.clear();
    m_imageAttachments.append(pixmap);
    rebuildAttachments();
}

void AgentChatPanel::rebuildAttachments() {
    auto* layout = qobject_cast<QHBoxLayout*>(m_attachmentBar->layout());
    while (layout->count() > 1) {
        auto* item = layout->takeAt(0);
        item->widget()->hide();
        item->widget()->deleteLater();
        delete item;
    }
    const ChatTheme theme = m_isDark ? ChatTheme::dark() : ChatTheme::light();
    for (int i = 0; i < m_imageAttachments.size(); ++i) {
        auto* item = new QWidget(m_attachmentBar);
        auto* row = new QHBoxLayout(item);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(2);
        auto* thumbnail = new QLabel(item);
        thumbnail->setFixedSize(48, 40);
        thumbnail->setAlignment(Qt::AlignCenter);
        thumbnail->setPixmap(m_imageAttachments[i].scaled(48, 40, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        row->addWidget(thumbnail);
        auto* remove = new QToolButton(item);
        remove->setObjectName("AgentRemoveAttachment");
        remove->setFixedSize(24, 24);
        remove->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Close, 14, theme.timestampColor));
        remove->setToolTip(tr("移除图片 %1").arg(i + 1));
        remove->setAccessibleName(remove->toolTip());
        connect(remove, &QToolButton::clicked, this, [this, i]() {
            m_imageAttachments.removeAt(i);
            rebuildAttachments();
        });
        row->addWidget(remove);
        layout->insertWidget(layout->count() - 1, item);
    }
    findChild<QScrollArea*>("AgentAttachmentScroll")->setVisible(!m_imageAttachments.isEmpty());
    updateStatusStrip();
}

void AgentChatPanel::clearImageAttachments() {
    m_imageAttachments.clear();
    m_attachmentNotice.clear(); // 批3 复核六轮（P2）：附件清空即复位超限提示
    // 布局清理与附件条可见性统一走 rebuildAttachments()（内部含 updateStatusStrip）
    rebuildAttachments();
}

void AgentChatPanel::showToolPreview(const QList<AgentToolPreviewCard::ToolItem>& tools) {
    clearToolPreview();
    m_emptyLabel->hide();
    m_stopped = false;
    m_pendingToolCount = tools.size();
    m_previewCard = new AgentToolPreviewCard(tools, m_isDark, m_messagesContainer);
    m_messagesLayout->insertWidget(m_messagesLayout->count() - 1, m_previewCard);
    connect(m_previewCard, &AgentToolPreviewCard::confirmed, this, [this]() {
        clearToolPreview();
        emit toolPreviewConfirmed();
    });
    connect(m_previewCard, &AgentToolPreviewCard::cancelled, this, [this]() {
        clearToolPreview();
        emit toolPreviewCancelled();
        m_inputEdit->setFocus();
    });
    updateStatusStrip();
}

void AgentChatPanel::clearToolPreview() {
    if (m_previewCard) {
        m_messagesLayout->removeWidget(m_previewCard);
        m_previewCard->hide();
        m_previewCard->deleteLater();
        m_previewCard = nullptr;
    }
    m_pendingToolCount = 0;
    updateStatusStrip();
}

void AgentChatPanel::setThinking(bool thinking) {
    m_isThinking = thinking;
    m_isThinkingTimeout = false;
    m_error.clear();
    if (thinking) {
        m_stopped = false;
        m_isExecuting = false;
        m_thinkingTimer->start(60000);
    } else {
        m_thinkingTimer->stop();
    }
    updateStatusStrip();
}

void AgentChatPanel::setExecuting(bool executing) {
    m_isExecuting = executing;
    if (executing) {
        m_stopped = false;
        m_isThinking = false;
        m_error.clear();
        m_thinkingTimer->stop();
    }
    updateStatusStrip();
}

void AgentChatPanel::setStopped() {
    m_stopped = true;
    m_isThinking = false;
    m_isExecuting = false;
    m_isThinkingTimeout = false;
    m_error.clear();
    m_thinkingTimer->stop();
    updateStatusStrip();
}

void AgentChatPanel::updateStopButton() {
    if (!m_stopButton)
        return;
    const bool stoppable = m_isThinking || m_isExecuting || m_pendingToolCount > 0;
    m_stopButton->setVisible(stoppable);
    m_stopButton->setEnabled(stoppable);
    m_stopButton->setToolTip(tr("停止当前轮"));
}

void AgentChatPanel::setError(const QString& error) {
    setThinking(false);
    m_error = error;
    updateStatusStrip();
}

void AgentChatPanel::onThinkingTimeout() {
    m_isThinkingTimeout = true;
    updateStatusStrip();
}

void AgentChatPanel::updateSendButton() {
    const bool busy = m_isThinking || m_isExecuting || m_pendingToolCount > 0;
    m_sendButton->setEnabled(!busy &&
                             (!m_inputEdit->toPlainText().trimmed().isEmpty() || !m_imageAttachments.isEmpty()));
    m_sendButton->setToolTip(busy ? tr("等待当前操作完成") : tr("发送消息"));
}

void AgentChatPanel::onSendClicked() {
    // 批3 复核五轮（P1-1）：忙碌守卫——上一请求仍在进行（Thinking）时直接
    // 返回、输入原样保留；不得先清空再依赖控制器全局状态反推"本次发送是否
    // 成功"——忙碌拒绝会保留上一请求的 Thinking 状态，反推将误判为成功
    if (m_isThinking || m_isExecuting || m_pendingToolCount > 0 || m_composing)
        return;
    const QString text = m_inputEdit->toPlainText().trimmed();
    const QList<QPixmap> images = m_imageAttachments; // 清空前快照
    if (text.isEmpty() && images.isEmpty())
        return;
    addMessage(AgentMessageBubble::Sender::User, text, images);
    // 批3 复核四轮（P2-5，隔离树实测）：先清空再 emit——控制器同步拒绝时
    // MainWindow 接线在 emit 返回前调用 restoreDraft 回填；若清空发生在
    // emit 之后，回填的草稿会被随即清掉（旧顺序在同步拒绝路径必然丢草稿）
    m_inputEdit->clear();
    clearImageAttachments();
    scrollToBottom();
    if (images.isEmpty())
        emit userMessageSent(text);
    else
        emit userMessageWithImagesSent(text, images);
    m_inputEdit->setFocus();
}

void AgentChatPanel::scrollToBottom() {
    m_followTail = true;
    m_scrollArea->verticalScrollBar()->setValue(m_scrollArea->verticalScrollBar()->maximum());
    m_latestButton->hide();
}

bool AgentChatPanel::eventFilter(QObject* obj, QEvent* event) {
    if (obj == m_inputEdit) {
        if (event->type() == QEvent::Resize)
            QTimer::singleShot(0, this, &AgentChatPanel::updateInputHeight);
        if (event->type() == QEvent::InputMethod)
            m_composing = !static_cast<QInputMethodEvent*>(event)->preeditString().isEmpty();
        if (event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->matches(QKeySequence::Paste)) {
                const auto* mime = QApplication::clipboard()->mimeData();
                if (mime->hasImage()) {
                    addImageAttachment(QPixmap::fromImage(qvariant_cast<QImage>(mime->imageData())));
                    return true;
                }
            }
            if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
                !(key->modifiers() & Qt::ShiftModifier) && !m_composing) {
                onSendClicked();
                return true;
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

void AgentChatPanel::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasImage() || event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void AgentChatPanel::dropEvent(QDropEvent* event) {
    const auto* mime = event->mimeData();
    if (mime->hasImage())
        addImageAttachment(QPixmap::fromImage(qvariant_cast<QImage>(mime->imageData())));
    else if (mime->hasUrls()) {
        for (const QUrl& url : mime->urls())
            if (url.isLocalFile())
                addImageAttachment(QPixmap(url.toLocalFile()));
    }
    event->acceptProposedAction();
}

void AgentChatPanel::applyTheme(bool isDark) {
    m_isDark = isDark;
    const ChatTheme theme = isDark ? ChatTheme::dark() : ChatTheme::light();
    setStyleSheet(QString("QWidget#AgentChatPanel,QWidget#AgentMessages { background:%1; }"
                          "QScrollArea { border:0; background:%1; }"
                          "QWidget#AgentChatStatusStrip { background:%1; border-bottom:1px solid %2; }"
                          "QWidget#AgentComposer { background:%3; border-top:1px solid %2; }"
                          "QLabel { color:%4; background:transparent; font-size:11px; }"
                          "QPlainTextEdit { background:%3; color:%5; border:0; padding:3px 4px; font-size:14px; }"
                          "QPlainTextEdit:focus { background:%1; }"
                          "QToolButton { border:0; background:transparent; border-radius:4px; }"
                          "QToolButton:hover,QToolButton:focus { background:%2; }"
                          "QToolButton#AgentSendMessage { background:%6; }"
                          "QToolButton#AgentSendMessage:disabled { background:%2; }")
                      .arg(theme.windowBg.name(), theme.inputBorder.name(), theme.inlineCodeBg.name(),
                           theme.timestampColor.name(), theme.userFg.name(), theme.userName.name()));
    m_sendButton->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Execute, 18, Qt::white));
    if (m_stopButton)
        m_stopButton->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Stop, 16, theme.errorColor));
    m_attachButton->setIcon(AppIconProvider::icon(AppIconProvider::Icon::LoadImage, 18, theme.timestampColor));
    m_latestButton->setIcon(AppIconProvider::icon(AppIconProvider::Icon::MoveDown, 16, theme.userName));
    for (auto* message : m_messagesContainer->findChildren<AgentMessageBubble*>())
        message->applyTheme(isDark);
    if (m_previewCard)
        m_previewCard->applyTheme(isDark);
    rebuildAttachments();
    updateInputHeight();
}

void AgentChatPanel::updateStatusStrip() {
    const ChatTheme theme = m_isDark ? ChatTheme::dark() : ChatTheme::light();
    const bool error = !m_error.isEmpty();
    if (m_statusLabel) {
        QColor statusColor = error ? theme.errorColor : theme.statusColor;
        if (error) {
            m_statusLabel->setText(tr("请求失败"));
        } else if (m_stopped) {
            m_statusLabel->setText(tr("已停止"));
        } else if (m_pendingToolCount > 0) {
            m_statusLabel->setText(tr("等待确认"));
        } else if (m_isThinkingTimeout) {
            m_statusLabel->setText(tr("请求超时"));
            statusColor = theme.errorColor;
        } else if (m_isExecuting) {
            m_statusLabel->setText(tr("执行中（可停止）"));
        } else if (m_isThinking) {
            m_statusLabel->setText(tr("● Agent 正在思考"));
        } else if (!m_attachmentNotice.isEmpty()) {
            // 批3 复核六轮（P2）：附件超限提示经状态条对用户可见
            m_statusLabel->setText(m_attachmentNotice);
            statusColor = theme.errorColor;
        } else {
            m_statusLabel->setText(tr("Agent 就绪"));
        }
        m_statusLabel->setToolTip(m_error);
        m_statusLabel->setStyleSheet(QString("color: %1; font-size: 12px; font-weight: 600;").arg(statusColor.name()));
    }
    if (m_messageCountLabel) {
        m_messageCountLabel->setText(tr("消息 %1").arg(m_messageCount));
        m_messageCountLabel->setVisible(m_messageCount > 0);
    }
    if (m_toolCountLabel) {
        m_toolCountLabel->setText(tr("待确认 %1").arg(m_pendingToolCount));
        m_toolCountLabel->setVisible(m_pendingToolCount > 0);
    }
    if (m_attachmentCountLabel) {
        // 批3 复核七轮（P2）：超限提示经附件计数器常显——状态条主文本在
        // 思考/超时态让位于运行状态，而用户忙碌期仍可添加附件，提示不得
        // 因状态优先级竞争而不可见（空闲态另经主文本错误色展示）
        if (!m_attachmentNotice.isEmpty())
            m_attachmentCountLabel->setText(m_attachmentNotice);
        else
            m_attachmentCountLabel->setText(tr("附件 %1").arg(m_imageAttachments.size()));
        m_attachmentCountLabel->setVisible(!m_imageAttachments.isEmpty() || !m_attachmentNotice.isEmpty());
    }
    updateStopButton();
    updateSendButton();
}

void AgentChatPanel::restoreDraft(const QString& text, const QList<QPixmap>& images) {
    // 批3 复核二轮（P2-7）/四轮（P2-5）：控制器拒绝后回填草稿（文本 + 限额内
    // 附件）。附件经公共入口 addImageAttachment 逐个回填：限额与附件条刷新
    // 复用同一实现，不依赖内部布局细节
    if (!text.isEmpty())
        m_inputEdit->setPlainText(text);
    for (const QPixmap& pixmap : images)
        addImageAttachment(pixmap);
    m_inputEdit->setFocus();
}

QString AgentChatPanel::inputDraftText() const {
    return m_inputEdit->toPlainText();
}

} // namespace DeepLux
