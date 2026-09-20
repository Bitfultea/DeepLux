#include "core/agent/AgentController.h"
#include "ui/views/MainWindow.h"
#include "ui/widgets/AgentChatPanel.h"

#include <QImage>
#include <QPixmap>
#include <QTemporaryDir>
#include <QtTest>

using namespace DeepLux;

/**
 * 批3 复核四轮（P2-5）：发送拒绝的草稿恢复回归。
 *
 * onSendClicked 为乐观 UI（先上屏并清空输入）；当 AgentController 同步校验
 * 拒绝（未初始化/未配置 LLM 客户端——状态停留在 Idle 而非 Thinking）时，
 * MainWindow 接线必须调用 restoreDraft 回填草稿，用户可原样重试。
 * 另覆盖附件源头限额（与控制器附件上限对齐）。
 */
class TestAgentDraftRestore : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_appDir;

private slots:
    void initTestCase() {
        QVERIFY(m_appDir.isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", m_appDir.path().toLocal8Bit());
    }
    void cleanupTestCase() {
        qunsetenv("DEEPLUX_APP_DATA_DIR");
    }

    // 控制器同步拒绝（无 LLM 客户端）→ 接线回填草稿
    void rejectedSendRestoresDraft() {
        MainWindow window;
        AgentChatPanel* panel = window.agentChatPanel();
        QVERIFY(panel != nullptr);

        // 确定性前置：shutdown 保证控制器无客户端/未初始化——sendUserMessage
        // 在状态迁移之前同步拒绝（state 保持 Idle）
        AgentController::instance().shutdown();
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);

        panel->restoreDraft(QStringLiteral("待重试草稿"), {}); // 公共入口注入草稿
        QCOMPARE(panel->inputDraftText(), QStringLiteral("待重试草稿"));

        QVERIFY(QMetaObject::invokeMethod(panel, "onSendClicked", Qt::DirectConnection));
        // 乐观 UI 已清空输入 → MainWindow 状态检查发现拒绝 → 回填
        QCOMPARE(panel->inputDraftText(), QStringLiteral("待重试草稿"));
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
    }

    // 附件源头限额：超出 kMaxImageAttachments 的附加被拒
    void attachmentCapEnforced() {
        AgentChatPanel panel;
        QImage img(8, 8, QImage::Format_RGB32);
        img.fill(Qt::red);
        for (int i = 0; i < 6; ++i)
            panel.addImageAttachment(QPixmap::fromImage(img));
        QCOMPARE(panel.imageAttachments().size(), AgentChatPanel::kMaxImageAttachments);
        // 空 pixmap 不计入
        panel.addImageAttachment(QPixmap());
        QCOMPARE(panel.imageAttachments().size(), AgentChatPanel::kMaxImageAttachments);
    }

    // restoreDraft 回填文本与限额内附件
    void restoreDraftRefillsTextAndAttachments() {
        AgentChatPanel panel;
        QImage img(8, 8, QImage::Format_RGB32);
        img.fill(Qt::blue);
        const QList<QPixmap> images{QPixmap::fromImage(img), QPixmap::fromImage(img)};
        panel.restoreDraft(QStringLiteral("重试"), images);
        QCOMPARE(panel.inputDraftText(), QStringLiteral("重试"));
        QCOMPARE(panel.imageAttachments().size(), 2);

        // 超限附件按限额截断（经 addImageAttachment 公共入口）
        QList<QPixmap> many;
        for (int i = 0; i < 6; ++i)
            many.append(QPixmap::fromImage(img));
        panel.clearImageAttachments();
        panel.restoreDraft(QString(), many);
        QCOMPARE(panel.imageAttachments().size(), AgentChatPanel::kMaxImageAttachments);
    }
};

QTEST_MAIN(TestAgentDraftRestore)
#include "test_agent_draft_restore.moc"
