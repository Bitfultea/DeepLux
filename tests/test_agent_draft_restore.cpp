#include "core/agent/AgentController.h"
#include "ui/views/MainWindow.h"
#include "ui/widgets/AgentChatPanel.h"

#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QSignalSpy>
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

    // 批3 复核五轮（P1-1）：面板忙碌（上一请求 Thinking 中）时再次发送——
    // onSendClicked 必须直接返回并保留输入。旧提交版无忙碌守卫：先清空输入，
    // 而控制器忙碌拒绝保留全局 Thinking 状态，MainWindow 的全局状态检查
    // 误判"本次发送成功"，第二条草稿丢失（复核负向测试已复现）
    void busyPanelKeepsDraft() {
        AgentChatPanel panel;
        panel.setThinking(true); // 模拟上一请求进行中
        panel.restoreDraft(QStringLiteral("忙碌中的草稿"), {});
        QVERIFY(QMetaObject::invokeMethod(&panel, "onSendClicked", Qt::DirectConnection));
        QCOMPARE(panel.inputDraftText(), QStringLiteral("忙碌中的草稿")); // 未被清空
    }

    // 批3 复核五轮（P2-4）：控制器附件上限显式拒绝并反馈（参数校验先于
    // 状态/客户端校验，无客户端环境亦可确定性测试）
    void controllerRejectsTooManyImages() {
        AgentController& controller = AgentController::instance();
        controller.shutdown();
        QSignalSpy spy(&controller, &AgentController::llmErrorOccurred);
        QImage img(4, 4, QImage::Format_RGB32);
        img.fill(Qt::red);
        QList<QPixmap> many;
        for (int i = 0; i < AgentController::kMaxImageAttachments + 1; ++i)
            many.append(QPixmap::fromImage(img));
        controller.sendUserMessageWithImages(QStringLiteral("x"), many);
        QCOMPARE(spy.count(), 1);
        QVERIFY(spy.first().at(0).toString().contains(QStringLiteral("Too many image attachments")));
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
    }

    // 批3 复核五轮（P2-4）：面板与控制器附件上限为同一契约
    void attachmentLimitContractAligned() {
        QCOMPARE(AgentChatPanel::kMaxImageAttachments, AgentController::kMaxImageAttachments);
    }

    // 批3 复核六轮（P2）：第 5 张附件被拒时必须给用户可见反馈——
    // 静默丢弃会让控制器侧的明确错误反馈在正常 UI 路径永远不可达
    void attachmentCapShowsUserFeedback() {
        AgentChatPanel panel;
        QImage img(8, 8, QImage::Format_RGB32);
        img.fill(Qt::red);
        for (int i = 0; i < AgentChatPanel::kMaxImageAttachments; ++i)
            panel.addImageAttachment(QPixmap::fromImage(img));
        QVERIFY(panel.attachmentNotice().isEmpty()); // 未超限无提示

        panel.addImageAttachment(QPixmap::fromImage(img)); // 第 5 张 → 拒绝
        QCOMPARE(panel.imageAttachments().size(), AgentChatPanel::kMaxImageAttachments);
        QVERIFY2(panel.attachmentNotice().contains(QStringLiteral("最多")), qPrintable(panel.attachmentNotice()));
        // UI 断言：超限提示经状态条对用户可见（文本或其提示通道）
        QLabel* status = panel.findChild<QLabel*>(QStringLiteral("AgentChatStatusLabel"));
        QVERIFY(status != nullptr);
        const bool visible =
            status->text().contains(QStringLiteral("最多")) || status->toolTip().contains(QStringLiteral("最多"));
        QVERIFY2(visible, qPrintable(QStringLiteral("text=%1 tooltip=%2").arg(status->text(), status->toolTip())));

        // 清空附件后提示复位
        panel.clearImageAttachments();
        QVERIFY(panel.attachmentNotice().isEmpty());
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
