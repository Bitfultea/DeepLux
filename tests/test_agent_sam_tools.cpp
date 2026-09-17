#include "core/agent/AgentActor.h"
#include "core/agent/SamPredictor.h"
#include "core/agent/ToolSchema.h"
#include "core/model/Annotation.h"
#include "helpers/SamTestServer.h"

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

using namespace DeepLux;
using DeepLuxTest::SamTestServer;

/**
 * SAM 批3：Agent 工具化 SAM——sam_predict / sam_annotate 白名单工具。
 *
 * 覆盖：Actor×SamTestServer 成功路径（结果 JSON/请求记录/prompt 透传）、
 * runSamPredictBatch 批量 embedding 复用、参数与图像先行校验失败关闭
 * （不发网络请求）、服务不可达失败关闭、会话文件写入+追加+字段完整性、
 * 全有或全无（任一失败不落盘）、混图拒绝与 items 守卫。
 * 权限门禁（Observer/Advisor/Autopilot 口径）在 test_agentpermissions 覆盖。
 */
class TestAgentSamTools : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString m_imagePath;
    QString m_otherImagePath; // 同尺寸异内容（混图场景）

    static QJsonObject annotateParams(const QString& serverUrl, const QString& sessionPath, const QString& imagePath,
                                      int timeoutMs = 5000) {
        return QJsonObject{
            {"imagePath", imagePath},
            {"sessionPath", sessionPath},
            {"serverUrl", serverUrl},
            {"timeoutMs", timeoutMs},
            {"items",
             QJsonArray{QJsonObject{{"points", QJsonArray{QJsonArray{32, 24}}}, {"label", QStringLiteral("defect")}},
                        QJsonObject{{"box", QJsonArray{5, 5, 20, 20}}, {"label", QStringLiteral("region")}}}}};
    }

private slots:
    void initTestCase() {
        QVERIFY(m_dir.isValid());
        QImage img(64, 48, QImage::Format_RGB32);
        img.fill(Qt::gray);
        m_imagePath = QDir(m_dir.path()).filePath(QStringLiteral("test_image.png"));
        QVERIFY(img.save(m_imagePath, "PNG"));
        QImage other(64, 48, QImage::Format_RGB32); // 同尺寸异内容
        other.fill(Qt::red);
        m_otherImagePath = QDir(m_dir.path()).filePath(QStringLiteral("other_image.png"));
        QVERIFY(other.save(m_otherImagePath, "PNG"));
        ToolSchema::instance().registerDefaultTools();
    }

    // ---------- sam_predict ----------

    void samPredictSuccessAgainstTestServer() {
        SamTestServer server;
        QVERIFY(server.listen());
        AgentActor actor;
        const QJsonObject params{{"imagePath", m_imagePath},
                                 {"serverUrl", server.url()},
                                 {"points", QJsonArray{QJsonArray{32, 24}}},
                                 {"label", QStringLiteral("widget")},
                                 {"timeoutMs", 5000}};
        const QJsonObject result = actor.executeTool(QStringLiteral("sam_predict"), params);
        QVERIFY2(!result.contains("error"), qPrintable(result.value("error").toString()));
        QCOMPARE(result.value("status").toString(), QStringLiteral("predicted"));
        QCOMPARE(result.value("label").toString(), QStringLiteral("widget"));
        QVERIFY(qAbs(result.value("score").toDouble() - 0.97) < 1e-9);
        const QJsonArray bbox = result.value("bbox").toArray();
        QCOMPARE(bbox.size(), 4);
        QCOMPARE(bbox[0].toDouble(), 10.0);
        QCOMPARE(bbox[2].toDouble(), 10.0);
        QCOMPARE(result.value("polygonPoints").toInt(), 4);
        const QJsonArray polygon = result.value("polygon").toArray();
        QCOMPARE(polygon.size(), 4);
        QCOMPARE(polygon[0].toArray()[0].toDouble(), 10.0);
        QCOMPARE(result.value("modelName").toString(), QStringLiteral("sam_test_vit_b"));
        // 不回传 RLE 本体（防上下文爆炸），仅长度
        QVERIFY(!result.contains("maskRle"));
        QVERIFY(result.contains("maskRleLength"));
        // 真实调用 set_image + predict，prompt 透传后端
        QVERIFY(server.requestLog.contains(QStringLiteral("POST /set_image")));
        QVERIFY(server.requestLog.contains(QStringLiteral("POST /predict")));
        QCOMPARE(server.lastPredictBody.value("points_pos").toArray()[0].toArray()[0].toDouble(), 32.0);
        QVERIFY(!server.lastPredictBody.contains("box")); // 未提供 box 不发送
    }

    // 批量预测：set_image 一次（embedding 复用），逐项 predict，标签一一对应
    void samPredictBatchReusesEmbedding() {
        SamTestServer server;
        QVERIFY(server.listen());
        SamPromptItem a;
        a.positivePoints = {QPointF(1, 2)};
        a.label = QStringLiteral("a");
        SamPromptItem b;
        b.box = QRectF(0, 0, 5, 5);
        b.label = QStringLiteral("b");

        const SamPredictOutcome outcome = runSamPredictBatch(server.url(), m_imagePath, {a, b}, 5000);
        QVERIFY2(outcome.ok, qPrintable(outcome.error));
        QCOMPARE(outcome.regions.size(), 2);
        QCOMPARE(outcome.regions[0].label, QStringLiteral("a"));
        QCOMPARE(outcome.regions[1].label, QStringLiteral("b"));
        QCOMPARE(outcome.failedIndex, -1);
        QCOMPARE(outcome.modelName, QStringLiteral("sam_test_vit_b"));
        QCOMPARE(server.setImageCount, 1); // embedding 复用：仅一次 set_image
        int predictCount = 0;
        for (const QString& entry : server.requestLog) {
            if (entry == QLatin1String("POST /predict"))
                ++predictCount;
        }
        QCOMPARE(predictCount, 2);
        // 第二项 box prompt 透传
        QCOMPARE(server.lastPredictBody.value("box").toArray().size(), 4);
    }

    // 先行校验失败关闭：以下场景均无服务在听，若发出网络请求将超时挂起——快速返回即证明未发请求
    void samPredictValidationFailClosed() {
        AgentActor actor;
        // 缺 required imagePath（schema 校验层）
        auto r =
            actor.executeTool(QStringLiteral("sam_predict"), QJsonObject{{"points", QJsonArray{QJsonArray{1, 2}}}});
        QVERIFY(r.value("error").toString().contains(QStringLiteral("imagePath")));
        // 图像不存在
        r = actor.executeTool(QStringLiteral("sam_predict"),
                              QJsonObject{{"imagePath", QStringLiteral("/nonexistent_sam_tool.png")},
                                          {"points", QJsonArray{QJsonArray{1, 2}}}});
        QVERIFY(r.value("error").toString().contains(QStringLiteral("不存在")));
        // 无 prompt
        r = actor.executeTool(QStringLiteral("sam_predict"), QJsonObject{{"imagePath", m_imagePath}});
        QVERIFY(r.value("error").toString().contains(QStringLiteral("prompt")));
        // points 非法形态（字符串坐标）
        r = actor.executeTool(
            QStringLiteral("sam_predict"),
            QJsonObject{{"imagePath", m_imagePath}, {"points", QJsonArray{QJsonArray{QStringLiteral("a"), 2}}}});
        QVERIFY(r.value("error").toString().contains(QStringLiteral("points")));
        // box w<=0
        r = actor.executeTool(QStringLiteral("sam_predict"),
                              QJsonObject{{"imagePath", m_imagePath}, {"box", QJsonArray{1, 2, 0, 4}}});
        QVERIFY(r.value("error").toString().contains(QStringLiteral("box")));
        // timeoutMs 越界（runSamPredictBatch 先行校验）
        r = actor.executeTool(
            QStringLiteral("sam_predict"),
            QJsonObject{{"imagePath", m_imagePath}, {"points", QJsonArray{QJsonArray{1, 2}}}, {"timeoutMs", 5}});
        QVERIFY(r.value("error").toString().contains(QStringLiteral("timeoutMs")));
    }

    void samPredictServerDownFailsClosed() {
        SamTestServer server;
        QVERIFY(server.listen());
        const QString deadUrl = server.url();
        server.crash(); // 端口释放：连接拒绝
        AgentActor actor;
        const QJsonObject params{{"imagePath", m_imagePath},
                                 {"serverUrl", deadUrl},
                                 {"points", QJsonArray{QJsonArray{1, 2}}},
                                 {"timeoutMs", 500}};
        const QJsonObject r = actor.executeTool(QStringLiteral("sam_predict"), params);
        QVERIFY(r.contains("error"));
        QVERIFY(!r.value("error").toString().isEmpty());
    }

    // ---------- sam_annotate ----------

    void samAnnotateWritesSessionAndAppends() {
        SamTestServer server;
        QVERIFY(server.listen());
        AgentActor actor;
        const QString sessionPath = QDir(m_dir.path()).filePath(QStringLiteral("session.deeplux-anno.json"));
        const QJsonObject params = annotateParams(server.url(), sessionPath, m_imagePath);

        auto r = actor.executeTool(QStringLiteral("sam_annotate"), params);
        QVERIFY2(!r.contains("error"), qPrintable(r.value("error").toString()));
        QCOMPARE(r.value("status").toString(), QStringLiteral("annotated"));
        QCOMPARE(r.value("added").toInt(), 2);
        QCOMPARE(r.value("total").toInt(), 2);
        QCOMPARE(r.value("objectIds").toArray().size(), 2);

        // 回读会话：字段完整（标签/置信度/bbox/polygon/prompts/尺寸/模型名）
        QString loadError;
        const AnnotationSession session = AnnotationSession::load(sessionPath, &loadError);
        QVERIFY2(loadError.isEmpty(), qPrintable(loadError));
        QCOMPARE(session.annotations.size(), 2);
        QCOMPARE(session.imagePath, m_imagePath);
        QCOMPARE(session.imageWidth, 64);
        QCOMPARE(session.imageHeight, 48);
        const AnnotationObject& first = session.annotations[0];
        QCOMPARE(first.label, QStringLiteral("defect"));
        QVERIFY(qAbs(first.score - 0.97) < 1e-9);
        QCOMPARE(first.bbox, QRectF(10, 10, 10, 10));
        QCOMPARE(first.polygon.size(), 4);
        QCOMPARE(first.prompts.pointsPos.size(), 1);
        QCOMPARE(first.prompts.pointsPos.first(), QPointF(32, 24));
        QVERIFY(!first.prompts.box.has_value());
        QVERIFY(!first.id.isEmpty());
        const AnnotationObject& second = session.annotations[1];
        QCOMPARE(second.label, QStringLiteral("region"));
        QVERIFY(second.prompts.box.has_value());
        QCOMPARE(second.prompts.box.value(), QRectF(5, 5, 20, 20));
        QVERIFY(second.prompts.pointsPos.isEmpty());
        QCOMPARE(second.modelName, QStringLiteral("sam_test_vit_b"));
        QVERIFY(first.id != second.id);

        // 追加语义：再次调用 → total 4，既有对象保留
        r = actor.executeTool(QStringLiteral("sam_annotate"), params);
        QVERIFY2(!r.contains("error"), qPrintable(r.value("error").toString()));
        QCOMPARE(r.value("added").toInt(), 2);
        QCOMPARE(r.value("total").toInt(), 4);
        const AnnotationSession appended = AnnotationSession::load(sessionPath, &loadError);
        QVERIFY(loadError.isEmpty());
        QCOMPARE(appended.annotations.size(), 4);
        QCOMPARE(appended.annotations[0].id, first.id);
    }

    // 全有或全无：任一预测失败不落盘，failedItem 指认失败项
    void samAnnotateAllOrNothing() {
        SamTestServer server;
        QVERIFY(server.listen());
        server.predictStatus = QStringLiteral("error");
        AgentActor actor;
        const QString sessionPath = QDir(m_dir.path()).filePath(QStringLiteral("aon.deeplux-anno.json"));
        const QJsonObject params = annotateParams(server.url(), sessionPath, m_imagePath);

        const auto r = actor.executeTool(QStringLiteral("sam_annotate"), params);
        QVERIFY(r.contains("error"));
        QCOMPARE(r.value("failedItem").toInt(), 0); // 第 0 项预测即失败
        QVERIFY2(!QFileInfo::exists(sessionPath), "failed batch must not write the session file");
    }

    void samAnnotateGuards() {
        SamTestServer server;
        QVERIFY(server.listen());
        AgentActor actor;
        const QString sessionPath = QDir(m_dir.path()).filePath(QStringLiteral("guards.deeplux-anno.json"));

        // items 缺失/非数组/空/超限/元素非对象/元素无 prompt
        auto r = actor.executeTool(QStringLiteral("sam_annotate"),
                                   QJsonObject{{"imagePath", m_imagePath}, {"sessionPath", sessionPath}});
        QVERIFY(r.value("error").toString().contains(QStringLiteral("items")));
        r = actor.executeTool(
            QStringLiteral("sam_annotate"),
            QJsonObject{{"imagePath", m_imagePath}, {"sessionPath", sessionPath}, {"items", QJsonArray{}}});
        QVERIFY(r.value("error").toString().contains(QStringLiteral("[1,16]")));
        QJsonArray many;
        for (int i = 0; i < 17; ++i)
            many.append(QJsonObject{{"points", QJsonArray{QJsonArray{1, 2}}}});
        r = actor.executeTool(QStringLiteral("sam_annotate"),
                              QJsonObject{{"imagePath", m_imagePath}, {"sessionPath", sessionPath}, {"items", many}});
        QVERIFY(r.value("error").toString().contains(QStringLiteral("[1,16]")));
        r = actor.executeTool(QStringLiteral("sam_annotate"), QJsonObject{{"imagePath", m_imagePath},
                                                                          {"sessionPath", sessionPath},
                                                                          {"items", QJsonArray{QJsonValue(1)}}});
        QVERIFY(r.value("error").toString().contains(QStringLiteral("items[0]")));
        r = actor.executeTool(QStringLiteral("sam_annotate"),
                              QJsonObject{{"imagePath", m_imagePath},
                                          {"sessionPath", sessionPath},
                                          {"serverUrl", server.url()},
                                          {"items", QJsonArray{QJsonObject{{"label", "x"}}}}});
        QVERIFY(r.value("error").toString().contains(QStringLiteral("prompt")));
        // 图像不可读（失败关闭在预测之前，不发网络请求）
        r = actor.executeTool(
            QStringLiteral("sam_annotate"),
            QJsonObject{{"imagePath", QStringLiteral("/nonexistent_sam_tool.png")},
                        {"sessionPath", sessionPath},
                        {"serverUrl", server.url()},
                        {"items", QJsonArray{QJsonObject{{"points", QJsonArray{QJsonArray{1, 2}}}}}}});
        QVERIFY(r.value("error").toString().contains(QStringLiteral("无法读取图像")));
        QVERIFY2(!QFileInfo::exists(sessionPath), "guards must not write the session file");
    }

    // 混图拒绝：既有会话绑定 imagePath，异图追加失败关闭且不改动文件
    void samAnnotateRejectsMixedImage() {
        SamTestServer server;
        QVERIFY(server.listen());
        AgentActor actor;
        const QString sessionPath = QDir(m_dir.path()).filePath(QStringLiteral("mixed.deeplux-anno.json"));
        // 首次成功写入（绑定 m_imagePath）
        auto r =
            actor.executeTool(QStringLiteral("sam_annotate"), annotateParams(server.url(), sessionPath, m_imagePath));
        QVERIFY2(!r.contains("error"), qPrintable(r.value("error").toString()));
        QCOMPARE(r.value("total").toInt(), 2);

        // 同尺寸异图追加 → 拒绝
        r = actor.executeTool(QStringLiteral("sam_annotate"),
                              annotateParams(server.url(), sessionPath, m_otherImagePath));
        QVERIFY(r.value("error").toString().contains(QStringLiteral("混图")));
        // 文件未被改动
        QString loadError;
        const AnnotationSession session = AnnotationSession::load(sessionPath, &loadError);
        QVERIFY(loadError.isEmpty());
        QCOMPARE(session.annotations.size(), 2);
        QCOMPARE(session.imagePath, m_imagePath);
    }
};

QTEST_MAIN(TestAgentSamTools)
#include "test_agent_sam_tools.moc"
