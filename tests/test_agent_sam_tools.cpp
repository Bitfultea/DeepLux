#include "core/agent/AgentActor.h"
#include "core/agent/SamPredictor.h"
#include "core/agent/ToolSchema.h"
#include "core/model/Annotation.h"
#include "helpers/SamTestServer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
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
            {"items", QJsonArray{QJsonObject{{"points", QJsonArray{QJsonValue(QJsonArray{32, 24})}},
                                             {"label", QStringLiteral("defect")}},
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

#ifndef DEEPLUX_HAS_ONNXRUNTIME
    void nativeModeUnavailableFailsClosed() {
        SamPromptItem item;
        item.positivePoints = {QPointF(32, 24)};
        const SamPredictOutcome result = runSamPredictBatch("native", m_imagePath, {item}, 5000);
        QVERIFY(!result.ok);
        QVERIFY(result.error.contains("未启用原生"));
        QVERIFY(result.regions.isEmpty());
    }
#endif

    // ---------- sam_predict ----------

    void samPredictSuccessAgainstTestServer() {
        SamTestServer server;
        QVERIFY(server.listen());
        AgentActor actor;
        const QJsonObject params{{"imagePath", m_imagePath},
                                 {"serverUrl", server.url()},
                                 {"points", QJsonArray{QJsonValue(QJsonArray{32, 24})}},
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

    // 批4（C）：多掩膜候选选择——candidateIndex=1 取次佳候选（不同区域），
    // candidateScores 回传全量 score 降序列表；越界/负值/无候选却指定均失败关闭
    void samPredictBatchCandidateSelection() {
        SamTestServer server;
        QVERIFY(server.listen());
        SamPromptItem item;
        item.positivePoints = {QPointF(15, 15)};
        item.label = QStringLiteral("cand");
        item.candidateIndex = 1;

        auto outcome = runSamPredictBatch(server.url(), m_imagePath, {item}, 5000);
        QVERIFY2(outcome.ok, qPrintable(outcome.error));
        QCOMPARE(outcome.regions.size(), 1);
        // 选中候选 1（30,30 区域），而非 best（10,10 区域）
        QCOMPARE(outcome.regions[0].bbox, QRectF(30, 30, 10, 10));
        QVERIFY(qAbs(outcome.regions[0].score - 0.9) < 1e-9);
        QCOMPARE(outcome.regions[0].polygon.size(), 4);
        QCOMPARE(outcome.regions[0].polygon.first(), QPointF(30, 30));
        QVERIFY(outcome.regions[0].maskRle.isEmpty()); // 候选 1 无 RLE：不伪造字段
        // candidateScores 回传全部候选 score（降序）
        QCOMPARE(outcome.candidateScores.size(), 1);
        QCOMPARE(outcome.candidateScores[0].size(), 3);
        QVERIFY(qAbs(outcome.candidateScores[0][0] - 0.97) < 1e-9);
        QVERIFY(qAbs(outcome.candidateScores[0][1] - 0.9) < 1e-9);
        QVERIFY(qAbs(outcome.candidateScores[0][2] - 0.8) < 1e-9);

        // 越界：本次共 3 个候选，index 5 失败关闭且指认失败项、全有或全无
        item.candidateIndex = 5;
        outcome = runSamPredictBatch(server.url(), m_imagePath, {item}, 5000);
        QVERIFY(!outcome.ok);
        QVERIFY2(outcome.error.contains(QStringLiteral("越界")), qPrintable(outcome.error));
        QCOMPARE(outcome.failedIndex, 0);
        QVERIFY(outcome.regions.isEmpty());

        // 负值：先行校验失败关闭，不发任何网络请求
        server.requestLog.clear();
        item.candidateIndex = -1;
        outcome = runSamPredictBatch(server.url(), m_imagePath, {item}, 5000);
        QVERIFY(!outcome.ok);
        QVERIFY(outcome.error.contains(QStringLiteral("candidateIndex")));
        QVERIFY2(server.requestLog.isEmpty(), "negative index must fail before any network request");

        // 后端未返回候选却指定 index>0 → 失败关闭（不静默回退 best）
        server.includePredictCandidates = false;
        item.candidateIndex = 1;
        outcome = runSamPredictBatch(server.url(), m_imagePath, {item}, 5000);
        QVERIFY(!outcome.ok);
        QVERIFY2(outcome.error.contains(QStringLiteral("未返回多掩膜候选")), qPrintable(outcome.error));
        QCOMPARE(outcome.failedIndex, 0);
        QVERIFY(outcome.regions.isEmpty());
        // 无候选 + index 0 → 仍走顶层 best 字段（向后兼容）
        item.candidateIndex = 0;
        outcome = runSamPredictBatch(server.url(), m_imagePath, {item}, 5000);
        QVERIFY2(outcome.ok, qPrintable(outcome.error));
        QVERIFY(qAbs(outcome.regions[0].score - 0.97) < 1e-9);
        QVERIFY(outcome.candidateScores[0].isEmpty()); // 无候选 → 空 score 列表
    }

    // 先行校验失败关闭：以下场景均无服务在听，若发出网络请求将超时挂起——快速返回即证明未发请求
    void samPredictValidationFailClosed() {
        AgentActor actor;
        // 缺 required imagePath（schema 校验层）
        auto r = actor.executeTool(QStringLiteral("sam_predict"),
                                   QJsonObject{{"points", QJsonArray{QJsonValue(QJsonArray{1, 2})}}});
        QVERIFY(r.value("error").toString().contains(QStringLiteral("imagePath")));
        // 图像不存在
        r = actor.executeTool(QStringLiteral("sam_predict"),
                              QJsonObject{{"imagePath", QStringLiteral("/nonexistent_sam_tool.png")},
                                          {"points", QJsonArray{QJsonValue(QJsonArray{1, 2})}}});
        QVERIFY(r.value("error").toString().contains(QStringLiteral("不存在")));
        // 无 prompt
        r = actor.executeTool(QStringLiteral("sam_predict"), QJsonObject{{"imagePath", m_imagePath}});
        QVERIFY(r.value("error").toString().contains(QStringLiteral("prompt")));
        // points 非法形态（字符串坐标）
        r = actor.executeTool(QStringLiteral("sam_predict"),
                              QJsonObject{{"imagePath", m_imagePath},
                                          {"points", QJsonArray{QJsonValue(QJsonArray{QStringLiteral("a"), 2})}}});
        QVERIFY(r.value("error").toString().contains(QStringLiteral("points")));
        // box w<=0
        r = actor.executeTool(QStringLiteral("sam_predict"),
                              QJsonObject{{"imagePath", m_imagePath}, {"box", QJsonArray{1, 2, 0, 4}}});
        QVERIFY(r.value("error").toString().contains(QStringLiteral("box")));
        // timeoutMs 越界（runSamPredictBatch 先行校验）
        r = actor.executeTool(QStringLiteral("sam_predict"),
                              QJsonObject{{"imagePath", m_imagePath},
                                          {"points", QJsonArray{QJsonValue(QJsonArray{1, 2})}},
                                          {"timeoutMs", 5}});
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
                                 {"points", QJsonArray{QJsonValue(QJsonArray{1, 2})}},
                                 {"timeoutMs", 500}};
        const QJsonObject r = actor.executeTool(QStringLiteral("sam_predict"), params);
        QVERIFY(r.contains("error"));
        QVERIFY(!r.value("error").toString().isEmpty());
    }

    // 批4（C）：sam_predict 工具 candidateIndex 参数——选中候选、候选摘要回传
    // （candidateCount/candidateScores/candidateIndex 回显），参数守卫失败关闭
    void samPredictToolCandidateIndex() {
        SamTestServer server;
        QVERIFY(server.listen());
        AgentActor actor;
        auto r = actor.executeTool(QStringLiteral("sam_predict"),
                                   QJsonObject{{"imagePath", m_imagePath},
                                               {"serverUrl", server.url()},
                                               {"points", QJsonArray{QJsonValue(QJsonArray{15, 15})}},
                                               {"candidateIndex", 1},
                                               {"timeoutMs", 5000}});
        QVERIFY2(!r.contains("error"), qPrintable(r.value("error").toString()));
        QCOMPARE(r.value("status").toString(), QStringLiteral("predicted"));
        // 结果为所选候选（30,30 区域，score 0.9），而非 best
        QVERIFY(qAbs(r.value("score").toDouble() - 0.9) < 1e-9);
        const QJsonArray bbox = r.value("bbox").toArray();
        QCOMPARE(bbox[0].toDouble(), 30.0);
        QCOMPARE(bbox[1].toDouble(), 30.0);
        QCOMPARE(r.value("polygon").toArray()[0].toArray()[0].toDouble(), 30.0);
        // 候选摘要：LLM 可据此改选
        QCOMPARE(r.value("candidateCount").toInt(), 3);
        QCOMPARE(r.value("candidateIndex").toInt(), 1);
        const QJsonArray scores = r.value("candidateScores").toArray();
        QCOMPARE(scores.size(), 3);
        QVERIFY(qAbs(scores[0].toDouble() - 0.97) < 1e-9);
        QVERIFY(qAbs(scores[2].toDouble() - 0.8) < 1e-9);
        QCOMPARE(r.value("maskRleLength").toInt(), 0); // 候选 1 无 RLE

        // 参数守卫：9 / -1 / 1.5 均拒绝且不发网络请求
        server.requestLog.clear();
        for (const QJsonValue bad : {QJsonValue(9), QJsonValue(-1), QJsonValue(1.5)}) {
            r = actor.executeTool(QStringLiteral("sam_predict"),
                                  QJsonObject{{"imagePath", m_imagePath},
                                              {"serverUrl", server.url()},
                                              {"points", QJsonArray{QJsonValue(QJsonArray{15, 15})}},
                                              {"candidateIndex", bad}});
            QVERIFY2(r.value("error").toString().contains(QStringLiteral("[0,7]")),
                     qPrintable(r.value("error").toString()));
        }
        QVERIFY2(server.requestLog.isEmpty(), "param guard must fail before any network request");

        // 5 在 [0,7] 内但超本次候选数 → 运行时越界失败关闭
        r = actor.executeTool(QStringLiteral("sam_predict"),
                              QJsonObject{{"imagePath", m_imagePath},
                                          {"serverUrl", server.url()},
                                          {"points", QJsonArray{QJsonValue(QJsonArray{15, 15})}},
                                          {"candidateIndex", 5},
                                          {"timeoutMs", 5000}});
        QVERIFY(r.value("error").toString().contains(QStringLiteral("越界")));
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
            many.append(QJsonObject{{"points", QJsonArray{QJsonValue(QJsonArray{1, 2})}}});
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
                        {"items", QJsonArray{QJsonObject{{"points", QJsonArray{QJsonValue(QJsonArray{1, 2})}}}}}});
        QVERIFY(r.value("error").toString().contains(QStringLiteral("无法读取图像")));
        QVERIFY2(!QFileInfo::exists(sessionPath), "guards must not write the session file");
    }

    // 批3 复核二轮（P1-4）：同路径 ≠ 同图——文件被替换后按尺寸+像素摘要拒绝，
    // 不把旧坐标系标注追加进新图，且原文件不被改动
    void samAnnotateRejectsReplacedImageAtSamePath() {
        SamTestServer server;
        QVERIFY(server.listen());
        AgentActor actor;
        const QString mutablePath = QDir(m_dir.path()).filePath(QStringLiteral("mutable.png"));
        const QString sessionPath = QDir(m_dir.path()).filePath(QStringLiteral("replaced.deeplux-anno.json"));

        QImage gray(64, 48, QImage::Format_RGB32);
        gray.fill(Qt::gray);
        QVERIFY(gray.save(mutablePath, "PNG"));
        auto r =
            actor.executeTool(QStringLiteral("sam_annotate"), annotateParams(server.url(), sessionPath, mutablePath));
        QVERIFY2(!r.contains("error"), qPrintable(r.value("error").toString()));
        QCOMPARE(r.value("total").toInt(), 2);
        // 会话已记录摘要
        QString loadError;
        AnnotationSession written = AnnotationSession::load(sessionPath, &loadError);
        QVERIFY(loadError.isEmpty());
        QVERIFY(!written.imageDigest.isEmpty());

        // 同路径换同尺寸异内容图 → 摘要不符拒绝
        QImage red(64, 48, QImage::Format_RGB32);
        red.fill(Qt::red);
        QVERIFY(red.save(mutablePath, "PNG")); // 覆盖同一路径
        r = actor.executeTool(QStringLiteral("sam_annotate"), annotateParams(server.url(), sessionPath, mutablePath));
        QVERIFY2(r.value("error").toString().contains(QStringLiteral("摘要")), qPrintable(r.value("error").toString()));

        // 同路径换异尺寸图 → 尺寸拒绝（尺寸校验先于摘要）
        QImage big(80, 60, QImage::Format_RGB32);
        big.fill(Qt::blue);
        QVERIFY(big.save(mutablePath, "PNG"));
        r = actor.executeTool(QStringLiteral("sam_annotate"), annotateParams(server.url(), sessionPath, mutablePath));
        QVERIFY2(r.value("error").toString().contains(QStringLiteral("尺寸")), qPrintable(r.value("error").toString()));

        // 原文件始终未被改动
        const AnnotationSession unchanged = AnnotationSession::load(sessionPath, &loadError);
        QVERIFY(loadError.isEmpty());
        QCOMPARE(unchanged.annotations.size(), 2);
        QCOMPARE(unchanged.imageWidth, 64);
        QCOMPARE(unchanged.imageDigest, written.imageDigest);
    }

    // 批3 复核三轮（P1-1）：无摘要既有会话——非空拒绝追加（无法证明图像未被
    // 替换，不静默重绑定坐标系）；空会话允许并补记摘要
    void samAnnotateDigestPolicyForLegacySessions() {
        SamTestServer server;
        QVERIFY(server.listen());
        AgentActor actor;

        auto writeSession = [&](const QString& path, const QJsonArray& annotations) {
            const QJsonObject session{{"imagePath", m_imagePath},
                                      {"imageWidth", 64},
                                      {"imageHeight", 48},
                                      {"modelName", QStringLiteral("sam")},
                                      {"annotations", annotations}};
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QJsonDocument(session).toJson());
            f.close();
        };

        // legacy 非空会话（无 imageDigest 字段）→ 拒绝追加，文件不动
        const QString legacyPath = QDir(m_dir.path()).filePath(QStringLiteral("legacy.deeplux-anno.json"));
        writeSession(legacyPath, QJsonArray{QJsonObject{{"id", QStringLiteral("legacy1")},
                                                        {"label", QStringLiteral("old")},
                                                        {"bbox", QJsonArray{1, 2, 3, 4}},
                                                        {"polygon", QJsonArray{}},
                                                        {"mask_rle", QString()},
                                                        {"score", 0.5},
                                                        {"model_name", QStringLiteral("sam")}}});
        auto r =
            actor.executeTool(QStringLiteral("sam_annotate"), annotateParams(server.url(), legacyPath, m_imagePath));
        QVERIFY2(r.value("error").toString().contains(QStringLiteral("缺少图像摘要")),
                 qPrintable(r.value("error").toString()));
        QString loadError;
        const AnnotationSession unchanged = AnnotationSession::load(legacyPath, &loadError);
        QVERIFY(loadError.isEmpty());
        QCOMPARE(unchanged.annotations.size(), 1);

        // 空会话（无摘要、无标注）→ 允许追加且补记摘要
        const QString emptyPath = QDir(m_dir.path()).filePath(QStringLiteral("empty.deeplux-anno.json"));
        writeSession(emptyPath, QJsonArray{});
        r = actor.executeTool(QStringLiteral("sam_annotate"), annotateParams(server.url(), emptyPath, m_imagePath));
        QVERIFY2(!r.contains("error"), qPrintable(r.value("error").toString()));
        QCOMPARE(r.value("total").toInt(), 2);
        const AnnotationSession filled = AnnotationSession::load(emptyPath, &loadError);
        QVERIFY(loadError.isEmpty());
        QVERIFY(!filled.imageDigest.isEmpty());
        // 同图再追加（摘要一致）→ 允许
        r = actor.executeTool(QStringLiteral("sam_annotate"), annotateParams(server.url(), emptyPath, m_imagePath));
        QVERIFY2(!r.contains("error"), qPrintable(r.value("error").toString()));
        QCOMPARE(r.value("total").toInt(), 4);
    }

    // 批4（B）：LabelMe 矩形 → box prompt 批量分割写入会话；XOR/无矩形/超限/
    // 损坏文件失败关闭；跳过形状显式报告（不静默）
    void samAnnotateFromLabelMe() {
        SamTestServer server;
        QVERIFY(server.listen());
        AgentActor actor;

        const QString lmPath = QDir(m_dir.path()).filePath(QStringLiteral("boxes.labelme.json"));
        {
            QFile f(lmPath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(R"({"shapes":[
                {"label":"defect","shape_type":"rectangle","points":[[10,10],[30,30]]},
                {"label":"poly","shape_type":"polygon","points":[[1,2],[3,4],[5,6]]},
                {"label":"dent","shape_type":"rectangle","points":[[40,20],[50,35]]}]})");
            f.close();
        }
        const QString sessionPath = QDir(m_dir.path()).filePath(QStringLiteral("lm.deeplux-anno.json"));
        auto r = actor.executeTool(QStringLiteral("sam_annotate"), QJsonObject{{"imagePath", m_imagePath},
                                                                               {"sessionPath", sessionPath},
                                                                               {"labelMePath", lmPath},
                                                                               {"serverUrl", server.url()},
                                                                               {"timeoutMs", 5000}});
        QVERIFY2(!r.contains("error"), qPrintable(r.value("error").toString()));
        QCOMPARE(r.value("added").toInt(), 2);
        QCOMPARE(r.value("importedRectangles").toInt(), 2);
        QCOMPARE(r.value("skippedShapes").toInt(), 1); // polygon 显式报告跳过
        QString loadError;
        const AnnotationSession session = AnnotationSession::load(sessionPath, &loadError);
        QVERIFY(loadError.isEmpty());
        QCOMPARE(session.annotations.size(), 2);
        QCOMPARE(session.annotations[0].label, QStringLiteral("defect"));
        QVERIFY(session.annotations[0].prompts.box.has_value());
        QCOMPARE(session.annotations[0].prompts.box.value(), QRectF(10, 10, 20, 20)); // 归一化
        QCOMPARE(session.annotations[1].label, QStringLiteral("dent"));
        QCOMPARE(server.lastPredictBody.value("box").toArray().size(), 4); // box prompt 透传

        // XOR：both / neither → 拒绝
        r = actor.executeTool(QStringLiteral("sam_annotate"), QJsonObject{{"imagePath", m_imagePath},
                                                                          {"sessionPath", sessionPath},
                                                                          {"labelMePath", lmPath},
                                                                          {"items", QJsonArray{QJsonObject{}}},
                                                                          {"serverUrl", server.url()}});
        QVERIFY(r.value("error").toString().contains(QStringLiteral("恰好提供一个")));
        r = actor.executeTool(
            QStringLiteral("sam_annotate"),
            QJsonObject{{"imagePath", m_imagePath}, {"sessionPath", sessionPath}, {"serverUrl", server.url()}});
        QVERIFY(r.value("error").toString().contains(QStringLiteral("恰好提供一个")));

        // 无可用矩形 → 拒绝且不写文件
        const QString polyOnly = QDir(m_dir.path()).filePath(QStringLiteral("poly.labelme.json"));
        {
            QFile f(polyOnly);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(R"({"shapes":[{"label":"p","shape_type":"polygon","points":[[1,2],[3,4],[5,6]]}]})");
            f.close();
        }
        const QString session2 = QDir(m_dir.path()).filePath(QStringLiteral("lm2.deeplux-anno.json"));
        r = actor.executeTool(QStringLiteral("sam_annotate"), QJsonObject{{"imagePath", m_imagePath},
                                                                          {"sessionPath", session2},
                                                                          {"labelMePath", polyOnly},
                                                                          {"serverUrl", server.url()}});
        QVERIFY(r.value("error").toString().contains(QStringLiteral("无可用矩形框")));
        QVERIFY(!QFileInfo::exists(session2));

        // 矩形数超 [1,16] → 拒绝
        {
            QJsonArray shapes;
            for (int i = 0; i < 17; ++i)
                shapes.append(QJsonObject{{"label", QStringLiteral("r%1").arg(i)},
                                          {"shape_type", QStringLiteral("rectangle")},
                                          {"points", QJsonArray{QJsonArray{i, 0}, QJsonArray{i + 1, 2}}}});
            const QString manyPath = QDir(m_dir.path()).filePath(QStringLiteral("many.labelme.json"));
            QFile f(manyPath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QJsonDocument(QJsonObject{{"shapes", shapes}}).toJson());
            f.close();
            r = actor.executeTool(QStringLiteral("sam_annotate"), QJsonObject{{"imagePath", m_imagePath},
                                                                              {"sessionPath", session2},
                                                                              {"labelMePath", manyPath},
                                                                              {"serverUrl", server.url()}});
            QVERIFY(r.value("error").toString().contains(QStringLiteral("[1,16]")));
        }

        // 损坏 LabelMe → 失败关闭
        const QString badLm = QDir(m_dir.path()).filePath(QStringLiteral("bad.labelme.json"));
        {
            QFile f(badLm);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("not json");
            f.close();
        }
        r = actor.executeTool(QStringLiteral("sam_annotate"), QJsonObject{{"imagePath", m_imagePath},
                                                                          {"sessionPath", session2},
                                                                          {"labelMePath", badLm},
                                                                          {"serverUrl", server.url()}});
        QVERIFY(r.value("error").toString().contains(QStringLiteral("LabelMe 导入失败")));
        QVERIFY(!QFileInfo::exists(session2));
    }

    // 批4（C）：sam_annotate items[].candidateIndex——写入会话的对象为所选
    // 候选；参数守卫与运行时越界失败关闭、不落盘
    void samAnnotateItemCandidateIndex() {
        SamTestServer server;
        QVERIFY(server.listen());
        AgentActor actor;
        const QString sessionPath = QDir(m_dir.path()).filePath(QStringLiteral("cand.deeplux-anno.json"));
        auto r = actor.executeTool(
            QStringLiteral("sam_annotate"),
            QJsonObject{{"imagePath", m_imagePath},
                        {"sessionPath", sessionPath},
                        {"serverUrl", server.url()},
                        {"timeoutMs", 5000},
                        {"items", QJsonArray{QJsonObject{{"points", QJsonArray{QJsonValue(QJsonArray{15, 15})}},
                                                         {"label", QStringLiteral("cand")},
                                                         {"candidateIndex", 1}}}}});
        QVERIFY2(!r.contains("error"), qPrintable(r.value("error").toString()));
        QCOMPARE(r.value("added").toInt(), 1);
        QString loadError;
        const AnnotationSession session = AnnotationSession::load(sessionPath, &loadError);
        QVERIFY(loadError.isEmpty());
        QCOMPARE(session.annotations.size(), 1);
        const AnnotationObject& obj = session.annotations[0];
        QCOMPARE(obj.label, QStringLiteral("cand"));
        QVERIFY(qAbs(obj.score - 0.9) < 1e-9); // 所选候选而非 best（0.97）
        QCOMPARE(obj.bbox, QRectF(30, 30, 10, 10));
        QCOMPARE(obj.polygon.size(), 4);
        QCOMPARE(obj.polygon.first(), QPointF(30, 30));

        // 参数守卫：items[0].candidateIndex=9 → 拒绝且不发网络请求
        server.requestLog.clear();
        const QString session2 = QDir(m_dir.path()).filePath(QStringLiteral("cand2.deeplux-anno.json"));
        r = actor.executeTool(
            QStringLiteral("sam_annotate"),
            QJsonObject{{"imagePath", m_imagePath},
                        {"sessionPath", session2},
                        {"serverUrl", server.url()},
                        {"items", QJsonArray{QJsonObject{{"points", QJsonArray{QJsonValue(QJsonArray{15, 15})}},
                                                         {"candidateIndex", 9}}}}});
        QVERIFY(r.value("error").toString().contains(QStringLiteral("items[0].candidateIndex")));
        QVERIFY2(server.requestLog.isEmpty(), "param guard must fail before any network request");
        QVERIFY(!QFileInfo::exists(session2));

        // 运行时越界：candidateIndex=5（合法参数、超本次候选数）→ 全有或全无
        const QString session3 = QDir(m_dir.path()).filePath(QStringLiteral("cand3.deeplux-anno.json"));
        r = actor.executeTool(
            QStringLiteral("sam_annotate"),
            QJsonObject{{"imagePath", m_imagePath},
                        {"sessionPath", session3},
                        {"serverUrl", server.url()},
                        {"timeoutMs", 5000},
                        {"items", QJsonArray{QJsonObject{{"points", QJsonArray{QJsonValue(QJsonArray{15, 15})}},
                                                         {"candidateIndex", 5}}}}});
        QVERIFY(r.value("error").toString().contains(QStringLiteral("越界")));
        QCOMPARE(r.value("failedItem").toInt(), 0);
        QVERIFY(!QFileInfo::exists(session3));
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
