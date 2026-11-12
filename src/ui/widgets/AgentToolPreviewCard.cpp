#include "AgentToolPreviewCard.h"

#include "AgentMessageBubble.h"
#include "AppIconProvider.h"
#include "core/agent/ToolSchema.h"

#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace DeepLux {

AgentToolPreviewCard::AgentToolPreviewCard(const QList<ToolItem>& tools, bool isDark, QWidget* parent,
                                           const ConfirmationContext& context)
    : QWidget(parent), m_tools(tools), m_isDark(isDark), m_context(context) {
    setupUi();
}

void AgentToolPreviewCard::setupUi() {
    auto* mainLayout = new QVBoxLayout(this);
    setObjectName("AgentToolPreview");
    setAttribute(Qt::WA_StyledBackground);
    mainLayout->setContentsMargins(12, 8, 12, 8);
    mainLayout->setSpacing(8);

    QStringList lines;
    lines.append(tr("待确认操作 · %1 项").arg(m_tools.size()));

    // 阶段1 §4（审核整改 R2）：展示工程、目标与副作用摘要（来自确认载荷 context）
    if (!m_context.isEmpty()) {
        if (!m_context.projectName.isEmpty()) {
            QString projectLine = tr("工程：%1").arg(m_context.projectName);
            if (!m_context.projectPath.isEmpty())
                projectLine += QStringLiteral("（%1）").arg(m_context.projectPath);
            lines.append(projectLine);
        }
        if (!m_context.sideEffects.isEmpty())
            lines.append(tr("副作用：%1").arg(m_context.sideEffects.join(QStringLiteral("；"))));
        if (!m_context.summary.isEmpty())
            lines.append(tr("摘要：%1").arg(m_context.summary));
        if (!m_context.reversibilityLabel.isEmpty())
            lines.append(m_context.reversibilityLabel);
    }

    for (int i = 0; i < m_tools.size(); ++i) {
        const ToolItem& t = m_tools[i];
        const bool dangerous = ToolSchema::instance().findTool(t.name).dangerous;
        const QString riskLabel = dangerous ? tr(" [高风险]") : QString();
        const bool clearsRuntimeLogs = t.name == QLatin1String("clear_runtime_logs");
        const QString toolLabel = clearsRuntimeLogs ? tr("清空运行日志内存") : t.name;
        QString line = QString("%1. %2%3").arg(i + 1).arg(toolLabel, riskLabel);

        if (clearsRuntimeLogs)
            line += tr("\n保留磁盘日志文件和 Agent 操作历史。");

        if (!t.params.isEmpty()) {
            line += "\n" + QString::fromUtf8(QJsonDocument(t.params).toJson(QJsonDocument::Indented)).trimmed();
        }
        lines.append(line);
    }

    QLabel* content = new QLabel(lines.join("\n\n"), this);
    content->setObjectName("AgentToolPreviewContent");
    content->setTextFormat(Qt::PlainText);
    content->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    content->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    content->setWordWrap(true);
    m_contentLabel = content;
    mainLayout->addWidget(content);

    // 确认/取消按钮
    auto* btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(8);
    btnLayout->addStretch();

    m_cancelBtn = new QPushButton(tr("取消"), this);
    m_cancelBtn->setObjectName("AgentCancelTools");
    m_cancelBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Cancel, 14, QColor("#DC2626")));
    m_confirmBtn = new QPushButton(tr("确认执行"), this);
    m_confirmBtn->setObjectName("AgentConfirmTools");
    m_confirmBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Confirm, 14, QColor("#16A34A")));
    const int buttonHeight = qMax(32, m_confirmBtn->fontMetrics().height() + 10);
    m_cancelBtn->setMinimumHeight(buttonHeight);
    m_confirmBtn->setMinimumHeight(buttonHeight);

    btnLayout->addWidget(m_cancelBtn);
    btnLayout->addWidget(m_confirmBtn);
    mainLayout->addLayout(btnLayout);

    connect(m_confirmBtn, &QPushButton::clicked, this, [this]() {
        m_confirmBtn->setEnabled(false);
        m_cancelBtn->setEnabled(false);
        emit confirmed();
    });
    connect(m_cancelBtn, &QPushButton::clicked, this, [this]() {
        m_confirmBtn->setEnabled(false);
        m_cancelBtn->setEnabled(false);
        emit cancelled();
    });

    applyTheme(m_isDark);
}

void AgentToolPreviewCard::applyTheme(bool isDark) {
    m_isDark = isDark;
    ChatTheme theme = isDark ? ChatTheme::dark() : ChatTheme::light();

    setStyleSheet(QString("QWidget#AgentToolPreview { border-left:2px solid %1; background:%2; }")
                      .arg(theme.toolName.name(), theme.inlineCodeBg.name()));
    m_contentLabel->setStyleSheet(
        QString("color:%1; font-size:13px; background:transparent;").arg(theme.userFg.name()));

    if (m_cancelBtn) {
        m_cancelBtn->setStyleSheet(QString("QPushButton { background: transparent; color: %1; border: 1px solid %2; "
                                           "border-radius: 4px; padding: 0 12px; font-size: 12px; }"
                                           "QPushButton:hover,QPushButton:focus { background: %3; }")
                                       .arg(theme.errorColor.name())
                                       .arg(theme.errorColor.name())
                                       .arg(theme.inputBorder.name()));
    }
    if (m_confirmBtn) {
        m_confirmBtn->setStyleSheet(QString("QPushButton { background: transparent; color: %1; border: 1px solid %1; "
                                            "border-radius: 4px; padding: 0 12px; font-size: 12px; }"
                                            "QPushButton:hover,QPushButton:focus { background: %2; }")
                                        .arg(theme.agentName.name())
                                        .arg(theme.inputBorder.name()));
    }
}

} // namespace DeepLux
