#include "core/agent/AgentActor.h"
#include "core/agent/AgentController.h"
#include "core/agent/AnnotationSessionExport.h"
#include "core/agent/AnnotationSessionTransform.h"
#include "core/agent/GuiEvent.h"
#include "core/agent/SamPredictor.h"
#include "core/agent/ToolSchema.h"
#include "core/common/UserManager.h"
#include "core/engine/RunEngine.h"
#include "core/io/CocoExporter.h"
#include "core/io/LabelMeExporter.h"
#include "core/io/YoloSegExporter.h"
#include "core/manager/PluginManager.h"
#include "core/manager/ProjectManager.h"
#include "core/model/Annotation.h"
#include "core/model/Project.h"
#include "helpers/SamTestServer.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#ifdef Q_OS_UNIX
#include <signal.h>
#include <sys/resource.h>
#endif
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QtTest>
#include <functional>

using namespace DeepLux;
using DeepLuxTest::SamTestServer;

namespace {

struct ScopeExit {
    std::function<void()> callback;
    ~ScopeExit() {
        if (callback)
            callback();
    }
};

} // namespace

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

    static int moduleSignalCount(const QSignalSpy& spy, const QString& instanceId) {
        int count = 0;
        for (const QList<QVariant>& args : spy)
            if (!args.isEmpty() && args.first().toString() == instanceId)
                ++count;
        return count;
    }

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

    static QString expectedImageEffect(const QString& imagePath) {
        QFile file(imagePath);
        if (!file.open(QIODevice::ReadOnly))
            return QStringLiteral("sam_input_image:%1 image_snapshot=unavailable").arg(imagePath);
        const QByteArray bytes = file.readAll();
        return QStringLiteral("sam_input_image:%1 bytes=%2 image_sha256=%3")
            .arg(imagePath)
            .arg(bytes.size())
            .arg(QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().left(12)));
    }

    static QString expectedCachedImageEffect(const QString& imagePath, const QString& targetModuleId, int width,
                                             int height, const QColor& color) {
        QImage pixels(width, height, QImage::Format_RGB32);
        pixels.fill(color);
        pixels = pixels.convertToFormat(QImage::Format_RGBA8888);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(QStringLiteral("%1x%2").arg(width).arg(height).toLatin1());
        for (int row = 0; row < pixels.height(); ++row)
            hash.addData(reinterpret_cast<const char*>(pixels.constScanLine(row)), pixels.bytesPerLine());
        return QStringLiteral("sam_input_image:%1 source=cached_input_for:%2 dimensions=%3x%4 pixel_bytes=%5 "
                              "image_sha256=%6")
            .arg(imagePath, targetModuleId)
            .arg(width)
            .arg(height)
            .arg(pixels.sizeInBytes())
            .arg(QString::fromLatin1(hash.result().toHex().left(12)));
    }

private slots:
    void initTestCase() {
        UserManager::instance().login(QStringLiteral("developer"), QString());
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

    void getAnnotationSessionReturnsBoundedPagedOriginalImageData() {
        const QString sessionPath = QDir(m_dir.path()).filePath(QStringLiteral("query.anno.json"));
        AnnotationSession session;
        session.imagePath = m_imagePath;
        session.imageWidth = 64;
        session.imageHeight = 48;
        session.modelName = QStringLiteral("fixture-model");
        AnnotationObject first;
        first.id = QStringLiteral("object-a");
        first.label = QStringLiteral("crack");
        first.bbox = QRectF(5.5, 6.5, 7.0, 8.0);
        first.polygon = {QPointF(12.25, 18.5), QPointF(22.75, 28.125)};
        first.maskRle = QStringLiteral("must-not-leak-mask");
        first.score = 0.875;
        first.modelName = QStringLiteral("object-model");
        first.prompts.pointsPos = {QPointF(10.0, 11.0), QPointF(12.0, 13.0)};
        first.prompts.pointsNeg = {QPointF(14.0, 15.0)};
        first.prompts.box = QRectF(3.0, 4.0, 20.0, 15.0);
        AnnotationObject second;
        second.id = QStringLiteral("object-b");
        second.label = QStringLiteral("dent");
        second.bbox = QRectF(30, 20, 9, 6);
        second.score = 0.5;
        session.annotations = {first, second};
        QString saveError;
        QVERIFY(session.save(sessionPath, &saveError));

        QFile sessionFile(sessionPath);
        QVERIFY(sessionFile.open(QIODevice::ReadOnly));
        const QByteArray originalBytes = sessionFile.readAll();
        sessionFile.close();
        const QByteArray originalDigest = QCryptographicHash::hash(originalBytes, QCryptographicHash::Sha256);

        AgentActor actor;
        QJsonObject page = actor.executeTool(QStringLiteral("get_annotation_session"),
                                             QJsonObject{{QStringLiteral("sessionPath"), sessionPath},
                                                         {QStringLiteral("offset"), 0},
                                                         {QStringLiteral("limit"), 1}});
        QCOMPARE(page.value(QStringLiteral("status")).toString(), QStringLiteral("session_loaded"));
        QCOMPARE(page.value(QStringLiteral("sessionPath")).toString(), sessionPath);
        QCOMPARE(page.value(QStringLiteral("imagePath")).toString(), m_imagePath);
        QCOMPARE(page.value(QStringLiteral("imageWidth")).toInt(), 64);
        QCOMPARE(page.value(QStringLiteral("imageHeight")).toInt(), 48);
        QCOMPARE(page.value(QStringLiteral("modelName")).toString(), QStringLiteral("fixture-model"));
        QVERIFY(!page.value(QStringLiteral("hasImageDigest")).toBool());
        QCOMPARE(page.value(QStringLiteral("total")).toInt(), 2);
        QCOMPARE(page.value(QStringLiteral("hasMore")).toBool(), true);
        const QJsonArray firstPage = page.value(QStringLiteral("annotations")).toArray();
        QCOMPARE(firstPage.size(), 1);
        const QJsonObject firstDto = firstPage.first().toObject();
        QCOMPARE(firstDto.value(QStringLiteral("id")).toString(), QStringLiteral("object-a"));
        QCOMPARE(firstDto.value(QStringLiteral("label")).toString(), QStringLiteral("crack"));
        const QJsonArray expectedBbox{5.5, 6.5, 7.0, 8.0};
        QCOMPARE(firstDto.value(QStringLiteral("bbox")).toArray(), expectedBbox);
        QCOMPARE(firstDto.value(QStringLiteral("score")).toDouble(), 0.875);
        QVERIFY(!firstDto.contains(QStringLiteral("polygon")));
        QVERIFY(!firstDto.contains(QStringLiteral("maskRle")));
        const QJsonObject promptDto = firstDto.value(QStringLiteral("prompt")).toObject();
        QCOMPARE(promptDto.value(QStringLiteral("positivePointCount")).toInt(), 2);
        QCOMPARE(promptDto.value(QStringLiteral("negativePointCount")).toInt(), 1);
        QVERIFY(promptDto.value(QStringLiteral("hasBox")).toBool());
        QVERIFY(QJsonDocument(page).toJson(QJsonDocument::Compact).size() <= 65536);

        page = actor.executeTool(QStringLiteral("get_annotation_session"),
                                 QJsonObject{{QStringLiteral("sessionPath"), sessionPath},
                                             {QStringLiteral("offset"), 1},
                                             {QStringLiteral("limit"), 1},
                                             {QStringLiteral("includePolygon"), true}});
        QCOMPARE(page.value(QStringLiteral("status")).toString(), QStringLiteral("session_loaded"));
        QCOMPARE(page.value(QStringLiteral("hasMore")).toBool(), false);
        QCOMPARE(page.value(QStringLiteral("annotations")).toArray().size(), 1);
        QCOMPARE(page.value(QStringLiteral("annotations"))
                     .toArray()
                     .first()
                     .toObject()
                     .value(QStringLiteral("id"))
                     .toString(),
                 QStringLiteral("object-b"));

        page = actor.executeTool(QStringLiteral("get_annotation_session"),
                                 QJsonObject{{QStringLiteral("sessionPath"), sessionPath},
                                             {QStringLiteral("offset"), 0},
                                             {QStringLiteral("limit"), 1},
                                             {QStringLiteral("includePolygon"), true}});
        const QJsonObject polygonDto = page.value(QStringLiteral("annotations")).toArray().first().toObject();
        const QJsonArray expectedPolygon{QJsonArray{12.25, 18.5}, QJsonArray{22.75, 28.125}};
        QCOMPARE(polygonDto.value(QStringLiteral("polygon")).toArray(), expectedPolygon);

        page =
            actor.executeTool(QStringLiteral("get_annotation_session"),
                              QJsonObject{{QStringLiteral("sessionPath"), sessionPath}, {QStringLiteral("offset"), 2}});
        QCOMPARE(page.value(QStringLiteral("status")).toString(), QStringLiteral("session_loaded"));
        QVERIFY(page.value(QStringLiteral("annotations")).toArray().isEmpty());
        QVERIFY(!page.value(QStringLiteral("hasMore")).toBool());
        page =
            actor.executeTool(QStringLiteral("get_annotation_session"),
                              QJsonObject{{QStringLiteral("sessionPath"), sessionPath}, {QStringLiteral("limit"), 50}});
        QCOMPARE(page.value(QStringLiteral("status")).toString(), QStringLiteral("session_loaded"));
        QCOMPARE(page.value(QStringLiteral("annotations")).toArray().size(), 2);

        QJsonObject invalid = actor.executeTool(QStringLiteral("get_annotation_session"),
                                                QJsonObject{{QStringLiteral("sessionPath"), sessionPath},
                                                            {QStringLiteral("includePolygon"), QStringLiteral("yes")}});
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        invalid = actor.executeTool(
            QStringLiteral("get_annotation_session"),
            QJsonObject{{QStringLiteral("sessionPath"), sessionPath}, {QStringLiteral("offset"), -1}});
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        invalid =
            actor.executeTool(QStringLiteral("get_annotation_session"),
                              QJsonObject{{QStringLiteral("sessionPath"), sessionPath}, {QStringLiteral("limit"), 51}});
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));

        AnnotationSession largePolygonSession;
        AnnotationObject largeObject;
        largeObject.id = QStringLiteral("large-polygon");
        largeObject.label = QStringLiteral("large");
        for (int i = 0; i < 6000; ++i)
            largeObject.polygon.append(QPointF(i + 0.25, i + 0.5));
        largePolygonSession.annotations.append(largeObject);
        const QString largePolygonPath = QDir(m_dir.path()).filePath(QStringLiteral("large-polygon.anno.json"));
        QVERIFY(largePolygonSession.save(largePolygonPath, &saveError));
        invalid = actor.executeTool(
            QStringLiteral("get_annotation_session"),
            QJsonObject{{QStringLiteral("sessionPath"), largePolygonPath}, {QStringLiteral("includePolygon"), true}});
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("OUTPUT_LIMIT"));
        QVERIFY(!invalid.contains(QStringLiteral("annotations")));
        page = actor.executeTool(QStringLiteral("get_annotation_session"),
                                 QJsonObject{{QStringLiteral("sessionPath"), largePolygonPath}});
        QCOMPARE(page.value(QStringLiteral("status")).toString(), QStringLiteral("session_loaded"));
        QVERIFY(QJsonDocument(page).toJson(QJsonDocument::Compact).size() <= 65536);

        const QString oversizedPath = QDir(m_dir.path()).filePath(QStringLiteral("oversized.anno.json"));
        QFile oversized(oversizedPath);
        QVERIFY(oversized.open(QIODevice::WriteOnly));
        QVERIFY(oversized.resize(64LL * 1024 * 1024 + 1));
        oversized.close();
        invalid = actor.executeTool(QStringLiteral("get_annotation_session"),
                                    QJsonObject{{QStringLiteral("sessionPath"), oversizedPath}});
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INPUT_LIMIT_EXCEEDED"));

        const QString malformedPath = QDir(m_dir.path()).filePath(QStringLiteral("malformed.anno.json"));
        QFile malformed(malformedPath);
        QVERIFY(malformed.open(QIODevice::WriteOnly));
        QCOMPARE(malformed.write(R"({"annotations":[{"id":"duplicate"},{"id":"duplicate"}]})"),
                 qint64(QByteArray(R"({"annotations":[{"id":"duplicate"},{"id":"duplicate"}]})").size()));
        malformed.close();
        invalid = actor.executeTool(QStringLiteral("get_annotation_session"),
                                    QJsonObject{{QStringLiteral("sessionPath"), malformedPath}});
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_SESSION"));
        QVERIFY(!invalid.value(QStringLiteral("error")).toString().contains(malformedPath));
        QVERIFY(QJsonDocument(invalid).toJson(QJsonDocument::Compact).size() <= 65536);
        const QString longDuplicatePath = QDir(m_dir.path()).filePath(QStringLiteral("long-duplicate.anno.json"));
        const QByteArray longId(70 * 1024, 'd');
        const QByteArray longDuplicateBytes = QByteArray(R"({"annotations":[{"id":")") + longId +
                                              QByteArray(R"("},{"id":")") + longId + QByteArray(R"("}]})");
        QFile longDuplicate(longDuplicatePath);
        QVERIFY(longDuplicate.open(QIODevice::WriteOnly));
        QCOMPARE(longDuplicate.write(longDuplicateBytes), qint64(longDuplicateBytes.size()));
        longDuplicate.close();
        invalid = actor.executeTool(QStringLiteral("get_annotation_session"),
                                    QJsonObject{{QStringLiteral("sessionPath"), longDuplicatePath}});
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_SESSION"));
        QVERIFY(QJsonDocument(invalid).toJson(QJsonDocument::Compact).size() <= 65536);
        QVERIFY(!invalid.value(QStringLiteral("error")).toString().contains(QString::fromLatin1(longId)));
        const QString invalidJsonPath = QDir(m_dir.path()).filePath(QStringLiteral("invalid-json.anno.json"));
        QFile invalidJson(invalidJsonPath);
        QVERIFY(invalidJson.open(QIODevice::WriteOnly));
        QCOMPARE(invalidJson.write("{"), qint64(1));
        invalidJson.close();
        invalid = actor.executeTool(QStringLiteral("get_annotation_session"),
                                    QJsonObject{{QStringLiteral("sessionPath"), invalidJsonPath}});
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_SESSION"));
        const QString missingFieldPath = QDir(m_dir.path()).filePath(QStringLiteral("missing-field.anno.json"));
        QFile missingField(missingFieldPath);
        QVERIFY(missingField.open(QIODevice::WriteOnly));
        const QByteArray missingFieldBytes = R"({"imagePath":"img.png"})";
        QCOMPARE(missingField.write(missingFieldBytes), qint64(missingFieldBytes.size()));
        missingField.close();
        invalid = actor.executeTool(QStringLiteral("get_annotation_session"),
                                    QJsonObject{{QStringLiteral("sessionPath"), missingFieldPath}});
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_SESSION"));
        invalid = actor.executeTool(QStringLiteral("get_annotation_session"),
                                    QJsonObject{{QStringLiteral("sessionPath"),
                                                 QDir(m_dir.path()).filePath(QStringLiteral("missing.anno.json"))}});
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("FILE_NOT_FOUND"));
        invalid = actor.executeTool(QStringLiteral("get_annotation_session"),
                                    QJsonObject{{QStringLiteral("sessionPath"), m_dir.path()}});
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PRECONDITION_FAILED"));

        const QString unreadablePath = QDir(m_dir.path()).filePath(QStringLiteral("unreadable.anno.json"));
        QFile unreadable(unreadablePath);
        QVERIFY(unreadable.open(QIODevice::WriteOnly));
        QCOMPARE(unreadable.write(R"({"annotations":[]})"), qint64(18));
        unreadable.close();
        QVERIFY(QFile::setPermissions(unreadablePath, QFileDevice::WriteOwner));
        QFile unreadableProbe(unreadablePath);
        if (unreadableProbe.open(QIODevice::ReadOnly)) {
            unreadableProbe.close();
            QVERIFY(QFile::setPermissions(unreadablePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
            QWARN("Current test user can read owner-restricted files; unreadable-file open failure is not observable");
        } else {
            invalid = actor.executeTool(QStringLiteral("get_annotation_session"),
                                        QJsonObject{{QStringLiteral("sessionPath"), unreadablePath}});
            QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PRECONDITION_FAILED"));
        }

        const QString emptySessionPath = QDir(m_dir.path()).filePath(QStringLiteral("empty.anno.json"));
        AnnotationSession emptySession;
        QVERIFY(emptySession.save(emptySessionPath, &saveError));
        page = actor.executeTool(QStringLiteral("get_annotation_session"),
                                 QJsonObject{{QStringLiteral("sessionPath"), emptySessionPath}});
        QCOMPARE(page.value(QStringLiteral("status")).toString(), QStringLiteral("session_loaded"));
        QCOMPARE(page.value(QStringLiteral("total")).toInt(), 0);
        QVERIFY(page.value(QStringLiteral("annotations")).toArray().isEmpty());
        QVERIFY(!page.value(QStringLiteral("hasMore")).toBool());

        const QString omittedDigestPath = QDir(m_dir.path()).filePath(QStringLiteral("omitted-digest.anno.json"));
        QFile omittedDigest(omittedDigestPath);
        QVERIFY(omittedDigest.open(QIODevice::WriteOnly));
        const QByteArray omittedDigestBytes = R"({"annotations":[]})";
        QCOMPARE(omittedDigest.write(omittedDigestBytes), qint64(omittedDigestBytes.size()));
        omittedDigest.close();
        page = actor.executeTool(QStringLiteral("get_annotation_session"),
                                 QJsonObject{{QStringLiteral("sessionPath"), omittedDigestPath}});
        QCOMPARE(page.value(QStringLiteral("status")).toString(), QStringLiteral("session_loaded"));
        QVERIFY(!page.value(QStringLiteral("hasImageDigest")).toBool());

        AnnotationSession withDigest;
        withDigest.imageDigest = QString(64, QLatin1Char('a'));
        const QString withDigestPath = QDir(m_dir.path()).filePath(QStringLiteral("with-digest.anno.json"));
        QVERIFY(withDigest.save(withDigestPath, &saveError));
        page = actor.executeTool(QStringLiteral("get_annotation_session"),
                                 QJsonObject{{QStringLiteral("sessionPath"), withDigestPath}});
        QVERIFY(page.value(QStringLiteral("hasImageDigest")).toBool());

        QFile check(sessionPath);
        QVERIFY(check.open(QIODevice::ReadOnly));
        const QByteArray finalBytes = check.readAll();
        QCOMPARE(QCryptographicHash::hash(finalBytes, QCryptographicHash::Sha256), originalDigest);
    }

    void exportAnnotationSessionMatchesAllExistingFormatsAndRejectsStaleTargets() {
        const QString sessionPath = QDir(m_dir.path()).filePath(QStringLiteral("export-source.anno.json"));
        AnnotationSession session;
        session.imagePath = m_imagePath;
        session.imageWidth = 100;
        session.imageHeight = 50;
        AnnotationObject first;
        first.id = QStringLiteral("id-crack");
        first.label = QStringLiteral("crack");
        first.bbox = QRectF(10, 5, 40, 20);
        first.polygon = {QPointF(10, 5), QPointF(50, 5), QPointF(50, 25), QPointF(10, 25)};
        first.maskRle = QStringLiteral("must-not-be-exported-as-mask-data");
        AnnotationObject second;
        second.id = QStringLiteral("id-dent");
        second.label = QStringLiteral("dent");
        second.bbox = QRectF(60, 20, 15, 10);
        second.polygon = {QPointF(60, 20), QPointF(75, 20), QPointF(68, 30)};
        session.annotations = {first, second};
        QString saveError;
        QVERIFY(session.save(sessionPath, &saveError));
        const QByteArray originalSession = [&]() {
            QFile input(sessionPath);
            if (!input.open(QIODevice::ReadOnly))
                return QByteArray();
            return input.readAll();
        }();
        QVERIFY(!originalSession.isEmpty());

        const auto runExport = [&](const QString& format, const QString& fileName, const QStringList& categories) {
            const QString outputPath = QDir(m_dir.path()).filePath(fileName);
            QJsonArray categoryValues;
            for (const QString& category : categories)
                categoryValues.append(category);
            const QJsonObject params{{QStringLiteral("sessionPath"), sessionPath},
                                     {QStringLiteral("format"), format},
                                     {QStringLiteral("outputPath"), outputPath},
                                     {QStringLiteral("categories"), categoryValues}};
            const QJsonObject approved = AnnotationSessionExport::confirmationSnapshot(params);
            const QJsonObject result = AnnotationSessionExport::execute(params, approved);
            return qMakePair(outputPath, result);
        };
        const auto jsonPointsMatch = [](const QJsonArray& actual, const QList<QPointF>& expected) {
            if (actual.size() != expected.size())
                return false;
            for (int i = 0; i < actual.size(); ++i) {
                const QJsonArray point = actual.at(i).toArray();
                if (point.size() != 2 || qAbs(point.at(0).toDouble() - expected.at(i).x()) > 1e-9 ||
                    qAbs(point.at(1).toDouble() - expected.at(i).y()) > 1e-9)
                    return false;
            }
            return true;
        };
        const auto flatPointsMatch = [](const QJsonArray& actual, const QList<double>& expected) {
            if (actual.size() != expected.size())
                return false;
            for (int i = 0; i < actual.size(); ++i) {
                if (qAbs(actual.at(i).toDouble() - expected.at(i)) > 1e-9)
                    return false;
            }
            return true;
        };

        const auto labelme = runExport(QStringLiteral("labelme"), QStringLiteral("out-labelme.json"), {});
        QCOMPARE(labelme.second.value(QStringLiteral("status")).toString(), QStringLiteral("exported"));
        QFile labelmeOutput(labelme.first);
        QVERIFY(labelmeOutput.open(QIODevice::ReadOnly));
        const QByteArray labelmeBytes = labelmeOutput.readAll();
        QCOMPARE(labelmeBytes, LabelMeExporter::toJsonString(session).toUtf8());
        QVERIFY(!labelmeBytes.contains("must-not-be-exported-as-mask-data"));
        QCOMPARE(labelme.second.value(QStringLiteral("objectCount")).toInt(), 2);
        const QJsonObject labelmeJson = QJsonDocument::fromJson(labelmeBytes).object();
        QCOMPARE(labelmeJson.value(QStringLiteral("imageWidth")).toInt(), 100);
        QCOMPARE(labelmeJson.value(QStringLiteral("imageHeight")).toInt(), 50);
        const QJsonArray labelmeShapes = labelmeJson.value(QStringLiteral("shapes")).toArray();
        QCOMPARE(labelmeShapes.size(), 2);
        const QJsonObject labelmeCrack = labelmeShapes.at(0).toObject();
        QCOMPARE(labelmeCrack.value(QStringLiteral("label")).toString(), QStringLiteral("crack"));
        QVERIFY(jsonPointsMatch(labelmeCrack.value(QStringLiteral("points")).toArray(),
                                {QPointF(10, 5), QPointF(50, 5), QPointF(50, 25), QPointF(10, 25)}));
        const QJsonObject labelmeDent = labelmeShapes.at(1).toObject();
        QCOMPARE(labelmeDent.value(QStringLiteral("label")).toString(), QStringLiteral("dent"));
        QVERIFY(jsonPointsMatch(labelmeDent.value(QStringLiteral("points")).toArray(),
                                {QPointF(60, 20), QPointF(75, 20), QPointF(68, 30)}));

        const QStringList yoloCategories{QStringLiteral("dent"), QStringLiteral("crack")};
        const auto yolo = runExport(QStringLiteral("yolo_seg"), QStringLiteral("out-yolo.txt"), yoloCategories);
        QCOMPARE(yolo.second.value(QStringLiteral("status")).toString(), QStringLiteral("exported"));
        QFile yoloOutput(yolo.first);
        QVERIFY(yoloOutput.open(QIODevice::ReadOnly));
        const QByteArray yoloBytes = yoloOutput.readAll();
        QCOMPARE(yoloBytes, YoloSegExporter::toJsonString(session, yoloCategories).toUtf8());
        QVERIFY(yoloBytes.startsWith("1 "));
        const QStringList yoloLines = QString::fromUtf8(yoloBytes).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QCOMPARE(yoloLines.size(), 2);
        const QStringList yoloCrack = yoloLines.at(0).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        QCOMPARE(yoloCrack.size(), 9);
        QCOMPARE(yoloCrack.at(0).toInt(), 1);
        const QList<double> expectedYoloCrack{0.1, 0.1, 0.5, 0.1, 0.5, 0.5, 0.1, 0.5};
        for (int i = 0; i < expectedYoloCrack.size(); ++i)
            QVERIFY(qAbs(yoloCrack.at(i + 1).toDouble() - expectedYoloCrack.at(i)) < 1e-6);
        const QStringList yoloDent = yoloLines.at(1).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        QCOMPARE(yoloDent.size(), 7);
        QCOMPARE(yoloDent.at(0).toInt(), 0);
        const QList<double> expectedYoloDent{0.6, 0.4, 0.75, 0.4, 0.68, 0.6};
        for (int i = 0; i < expectedYoloDent.size(); ++i)
            QVERIFY(qAbs(yoloDent.at(i + 1).toDouble() - expectedYoloDent.at(i)) < 1e-6);

        const QString unknownLabelPath = QDir(m_dir.path()).filePath(QStringLiteral("unknown-yolo.txt"));
        const QJsonObject unknownLabelParams{{QStringLiteral("sessionPath"), sessionPath},
                                             {QStringLiteral("format"), QStringLiteral("yolo_seg")},
                                             {QStringLiteral("outputPath"), unknownLabelPath},
                                             {QStringLiteral("categories"), QJsonArray{QStringLiteral("crack")}}};
        const QJsonObject unknownLabelResult = AnnotationSessionExport::execute(
            unknownLabelParams, AnnotationSessionExport::confirmationSnapshot(unknownLabelParams));
        QCOMPARE(unknownLabelResult.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        QVERIFY(!QFileInfo::exists(unknownLabelPath));

        const QJsonObject overlongPath = AnnotationSessionExport::preflight(
            QJsonObject{{QStringLiteral("sessionPath"), sessionPath},
                        {QStringLiteral("format"), QStringLiteral("labelme")},
                        {QStringLiteral("outputPath"), QString(4097, QLatin1Char('x')) + QStringLiteral(".json")}});
        QCOMPARE(overlongPath.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));

        const QStringList cocoCategories{QStringLiteral("dent")};
        const auto coco = runExport(QStringLiteral("coco"), QStringLiteral("out-coco.json"), cocoCategories);
        QCOMPARE(coco.second.value(QStringLiteral("status")).toString(), QStringLiteral("exported"));
        QFile cocoOutput(coco.first);
        QVERIFY(cocoOutput.open(QIODevice::ReadOnly));
        QString cocoError;
        QCOMPARE(
            cocoOutput.readAll(),
            QJsonDocument(CocoExporter::toJson(session, cocoCategories, &cocoError)).toJson(QJsonDocument::Indented));
        QVERIFY(cocoError.isEmpty());
        QFile cocoReadback(coco.first);
        QVERIFY(cocoReadback.open(QIODevice::ReadOnly));
        const QJsonObject cocoJson = QJsonDocument::fromJson(cocoReadback.readAll()).object();
        const QJsonArray cocoImages = cocoJson.value(QStringLiteral("images")).toArray();
        QCOMPARE(cocoImages.size(), 1);
        QCOMPARE(cocoImages.at(0).toObject().value(QStringLiteral("width")).toInt(), 100);
        QCOMPARE(cocoImages.at(0).toObject().value(QStringLiteral("height")).toInt(), 50);
        const QJsonArray cocoCategoriesJson = cocoJson.value(QStringLiteral("categories")).toArray();
        QCOMPARE(cocoCategoriesJson.size(), 2);
        QCOMPARE(cocoCategoriesJson.at(0).toObject().value(QStringLiteral("id")).toInt(), 1);
        QCOMPARE(cocoCategoriesJson.at(0).toObject().value(QStringLiteral("name")).toString(), QStringLiteral("dent"));
        QCOMPARE(cocoCategoriesJson.at(1).toObject().value(QStringLiteral("id")).toInt(), 2);
        QCOMPARE(cocoCategoriesJson.at(1).toObject().value(QStringLiteral("name")).toString(), QStringLiteral("crack"));
        const QJsonArray cocoAnnotations = cocoJson.value(QStringLiteral("annotations")).toArray();
        QCOMPARE(cocoAnnotations.size(), 2);
        const QJsonObject cocoCrack = cocoAnnotations.at(0).toObject();
        QCOMPARE(cocoCrack.value(QStringLiteral("category_id")).toInt(), 2);
        QVERIFY(flatPointsMatch(cocoCrack.value(QStringLiteral("bbox")).toArray(), {10, 5, 40, 20}));
        QVERIFY(flatPointsMatch(cocoCrack.value(QStringLiteral("segmentation")).toArray().at(0).toArray(),
                                {10, 5, 50, 5, 50, 25, 10, 25}));
        QVERIFY(qAbs(cocoCrack.value(QStringLiteral("area")).toDouble() - 800.0) < 1e-9);
        const QJsonObject cocoDent = cocoAnnotations.at(1).toObject();
        QCOMPARE(cocoDent.value(QStringLiteral("category_id")).toInt(), 1);
        QVERIFY(flatPointsMatch(cocoDent.value(QStringLiteral("bbox")).toArray(), {60, 20, 15, 10}));
        QVERIFY(flatPointsMatch(cocoDent.value(QStringLiteral("segmentation")).toArray().at(0).toArray(),
                                {60, 20, 75, 20, 68, 30}));
        QVERIFY(qAbs(cocoDent.value(QStringLiteral("area")).toDouble() - 75.0) < 1e-9);

        QFile checkSession(sessionPath);
        QVERIFY(checkSession.open(QIODevice::ReadOnly));
        QCOMPARE(checkSession.readAll(), originalSession);

        const QString occupiedPath = QDir(m_dir.path()).filePath(QStringLiteral("already-exists.json"));
        QFile occupied(occupiedPath);
        QVERIFY(occupied.open(QIODevice::WriteOnly));
        const QByteArray sentinel("preserve-existing-target");
        QCOMPARE(occupied.write(sentinel), qint64(sentinel.size()));
        occupied.close();
        const QJsonObject occupiedParams{{QStringLiteral("sessionPath"), sessionPath},
                                         {QStringLiteral("format"), QStringLiteral("labelme")},
                                         {QStringLiteral("outputPath"), occupiedPath}};
        const QJsonObject occupiedResult = AnnotationSessionExport::execute(
            occupiedParams, AnnotationSessionExport::confirmationSnapshot(occupiedParams));
        QCOMPARE(occupiedResult.value(QStringLiteral("errorCode")).toString(), QStringLiteral("OUTPUT_EXISTS"));
        QFile occupiedCheck(occupiedPath);
        QVERIFY(occupiedCheck.open(QIODevice::ReadOnly));
        QCOMPARE(occupiedCheck.readAll(), sentinel);

        const QString preparedPath = QDir(m_dir.path()).filePath(QStringLiteral("prepared-export.tmp"));
        QFile prepared(preparedPath);
        QVERIFY(prepared.open(QIODevice::WriteOnly));
        QCOMPARE(prepared.write("prepared-complete-output"), qint64(24));
        prepared.close();
        QString publishError;
        const QString occupiedParentIdentity = AnnotationSessionExport::confirmationSnapshot(occupiedParams)
                                                   .value(QStringLiteral("outputParentIdentity"))
                                                   .toString();
        QVERIFY(!occupiedParentIdentity.isEmpty());
        QVERIFY(!AnnotationSessionExport::publishPreparedFileNoReplace(preparedPath, occupiedPath,
                                                                       occupiedParentIdentity, &publishError));
        QCOMPARE(publishError, QStringLiteral("OUTPUT_EXISTS"));
        QFile stillOccupied(occupiedPath);
        QVERIFY(stillOccupied.open(QIODevice::ReadOnly));
        QCOMPARE(stillOccupied.readAll(), sentinel);
        QFile::remove(preparedPath);

        const QString symlinkPreparedPath = QDir(m_dir.path()).filePath(QStringLiteral("prepared-export-link.tmp"));
        if (QFile::link(sessionPath, symlinkPreparedPath)) {
            const QString symlinkTargetPath = QDir(m_dir.path()).filePath(QStringLiteral("symlink-export.json"));
            publishError.clear();
            QVERIFY(!AnnotationSessionExport::publishPreparedFileNoReplace(symlinkPreparedPath, symlinkTargetPath,
                                                                           occupiedParentIdentity, &publishError));
            QCOMPARE(publishError, QStringLiteral("FILE_WRITE_FAILED"));
            QVERIFY(!QFileInfo::exists(symlinkTargetPath));
            QFile::remove(symlinkPreparedPath);
        } else {
            QWARN("Symlink creation is unavailable; publisher symlink rejection is not observable");
        }

        const QString symlinkSourcePath = QDir(m_dir.path()).filePath(QStringLiteral("source-link.anno.json"));
        if (QFile::link(sessionPath, symlinkSourcePath)) {
            const QString symlinkSourceOutput = QDir(m_dir.path()).filePath(QStringLiteral("source-link-output.json"));
            const QJsonObject symlinkSourceParams{{QStringLiteral("sessionPath"), symlinkSourcePath},
                                                  {QStringLiteral("format"), QStringLiteral("labelme")},
                                                  {QStringLiteral("outputPath"), symlinkSourceOutput}};
            const QJsonObject symlinkSourceResult = AnnotationSessionExport::execute(
                symlinkSourceParams, AnnotationSessionExport::confirmationSnapshot(symlinkSourceParams));
            QCOMPARE(symlinkSourceResult.value(QStringLiteral("errorCode")).toString(),
                     QStringLiteral("PRECONDITION_FAILED"));
            QVERIFY(!QFileInfo::exists(symlinkSourceOutput));
            QFile::remove(symlinkSourcePath);
        } else {
            QWARN("Symlink creation is unavailable; source symlink rejection is not observable");
        }

        const QString parentPath = QDir(m_dir.path()).filePath(QStringLiteral("export-parent"));
        QVERIFY(QDir().mkpath(parentPath));
        const QString parentOutput = QDir(parentPath).filePath(QStringLiteral("parent-drift.json"));
        const QJsonObject parentParams{{QStringLiteral("sessionPath"), sessionPath},
                                       {QStringLiteral("format"), QStringLiteral("labelme")},
                                       {QStringLiteral("outputPath"), parentOutput}};
        const QJsonObject parentSnapshot = AnnotationSessionExport::confirmationSnapshot(parentParams);
        QVERIFY(!parentSnapshot.value(QStringLiteral("outputParentIdentity")).toString().isEmpty());
        const QString movedParentPath = parentPath + QStringLiteral("-moved");
        QVERIFY(QDir().rename(parentPath, movedParentPath));
        QVERIFY(QDir().mkpath(parentPath));
        const QJsonObject parentDriftResult = AnnotationSessionExport::execute(parentParams, parentSnapshot);
        QCOMPARE(parentDriftResult.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("CONFIRMATION_EXPIRED"));
        QVERIFY(!QFileInfo::exists(parentOutput));

#ifdef Q_OS_UNIX
        const QString writeFailureOutput = QDir(m_dir.path()).filePath(QStringLiteral("write-failure.json"));
        const QJsonObject writeFailureParams{{QStringLiteral("sessionPath"), sessionPath},
                                             {QStringLiteral("format"), QStringLiteral("labelme")},
                                             { QStringLiteral("outputPath"),
                                               writeFailureOutput }};
        const QJsonObject writeFailureSnapshot = AnnotationSessionExport::confirmationSnapshot(writeFailureParams);
        struct rlimit oldFileLimit {};
        if (::getrlimit(RLIMIT_FSIZE, &oldFileLimit) == 0) {
            struct rlimit limitedFileSize = oldFileLimit;
            limitedFileSize.rlim_cur = 1;
            const auto oldSignalHandler = ::signal(SIGXFSZ, SIG_IGN);
            if (::setrlimit(RLIMIT_FSIZE, &limitedFileSize) == 0) {
                const QJsonObject writeFailureResult =
                    AnnotationSessionExport::execute(writeFailureParams, writeFailureSnapshot);
                ::setrlimit(RLIMIT_FSIZE, &oldFileLimit);
                ::signal(SIGXFSZ, oldSignalHandler);
                QCOMPARE(writeFailureResult.value(QStringLiteral("errorCode")).toString(),
                         QStringLiteral("FILE_WRITE_FAILED"));
                QVERIFY(!QFileInfo::exists(writeFailureOutput));
                QVERIFY(QDir(m_dir.path()).entryList({QStringLiteral(".deeplux-export-*")}, QDir::Dirs).isEmpty());
            } else {
                ::signal(SIGXFSZ, oldSignalHandler);
                QWARN("RLIMIT_FSIZE could not be lowered; deterministic write-failure injection is unavailable");
            }
        } else {
            QWARN("RLIMIT_FSIZE is unavailable; deterministic write-failure injection is unavailable");
        }
#endif

        const QString staleOutput = QDir(m_dir.path()).filePath(QStringLiteral("stale-output.json"));
        const QJsonObject staleParams{{QStringLiteral("sessionPath"), sessionPath},
                                      {QStringLiteral("format"), QStringLiteral("labelme")},
                                      {QStringLiteral("outputPath"), staleOutput}};
        const QJsonObject staleSnapshot = AnnotationSessionExport::confirmationSnapshot(staleParams);
        AnnotationSession changed = session;
        changed.annotations[0].label = QStringLiteral("changed-after-approval");
        QVERIFY(changed.save(sessionPath, &saveError));
        const QJsonObject staleResult = AnnotationSessionExport::execute(staleParams, staleSnapshot);
        QCOMPARE(staleResult.value(QStringLiteral("errorCode")).toString(), QStringLiteral("CONFIRMATION_EXPIRED"));
        QVERIFY(!QFileInfo::exists(staleOutput));
    }

    void transformAnnotationSessionPreservesExtensionsAndSourceBytes() {
        QTemporaryDir transformDir;
        QVERIFY(transformDir.isValid());
        AnnotationSession session;
        session.imagePath = m_imagePath;
        session.imageWidth = 100;
        session.imageHeight = 50;
        AnnotationObject first;
        first.id = QStringLiteral("object-a");
        first.label = QStringLiteral("old");
        first.bbox = QRectF(1, 2, 10, 12);
        first.polygon = {QPointF(1, 2), QPointF(11, 2), QPointF(11, 14)};
        first.maskRle = QStringLiteral("private-mask-bytes");
        AnnotationObject second = first;
        second.id = QStringLiteral("object-b");
        second.label = QStringLiteral("keep");
        session.annotations = {first, second};

        const QString sourcePath =
            QDir(transformDir.path()).filePath(QStringLiteral("transform-source.deeplux-anno.json"));
        QString saveError;
        QVERIFY(session.save(sourcePath, &saveError));
        QFile sourceFile(sourcePath);
        QVERIFY(sourceFile.open(QIODevice::ReadOnly));
        QJsonObject sourceRoot = QJsonDocument::fromJson(sourceFile.readAll()).object();
        sourceFile.close();
        sourceRoot.insert(QStringLiteral("futureRootField"), QJsonObject{{QStringLiteral("keep"), 17}});
        QJsonArray sourceAnnotations = sourceRoot.value(QStringLiteral("annotations")).toArray();
        QJsonObject firstJson = sourceAnnotations.at(0).toObject();
        firstJson.insert(QStringLiteral("futureObjectField"), QStringLiteral("preserve-object-extension"));
        QJsonObject prompts = firstJson.value(QStringLiteral("prompts")).toObject();
        prompts.insert(QStringLiteral("futurePromptField"), QJsonArray{1, 2, 3});
        firstJson.insert(QStringLiteral("prompts"), prompts);
        sourceAnnotations.replace(0, firstJson);
        sourceRoot.insert(QStringLiteral("annotations"), sourceAnnotations);
        const QByteArray originalBytes = QJsonDocument(sourceRoot).toJson(QJsonDocument::Indented);
        QFile sourceRewrite(sourcePath);
        QVERIFY(sourceRewrite.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(sourceRewrite.write(originalBytes), qint64(originalBytes.size()));
        sourceRewrite.close();

        const QString labelOutput = QDir(transformDir.path()).filePath(QStringLiteral("relabeled.deeplux-anno.json"));
        const QJsonObject labelParams{{QStringLiteral("sessionPath"), sourcePath},
                                      {QStringLiteral("outputPath"), labelOutput},
                                      {QStringLiteral("operation"), QStringLiteral("set_label")},
                                      {QStringLiteral("objectIds"), QJsonArray{QStringLiteral("object-a")}},
                                      {QStringLiteral("label"), QStringLiteral("  repaired  ")}};
        QVERIFY(AnnotationSessionTransform::preflight(labelParams).isEmpty());
        const QJsonObject labelResult = AnnotationSessionTransform::execute(
            labelParams, AnnotationSessionTransform::confirmationSnapshot(labelParams));
        QCOMPARE(labelResult.value(QStringLiteral("status")).toString(), QStringLiteral("transformed"));
        QCOMPARE(labelResult.value(QStringLiteral("changedCount")).toInt(), 1);
        QCOMPARE(labelResult.value(QStringLiteral("objectCount")).toInt(), 2);
        QFile labelFile(labelOutput);
        QVERIFY(labelFile.open(QIODevice::ReadOnly));
        const QByteArray transformedLabelBytes = labelFile.readAll();
        const QJsonObject labelRoot = QJsonDocument::fromJson(transformedLabelBytes).object();
        QCOMPARE(labelRoot.value(QStringLiteral("futureRootField")).toObject().value(QStringLiteral("keep")).toInt(),
                 17);
        const QJsonArray labelObjects = labelRoot.value(QStringLiteral("annotations")).toArray();
        const QJsonObject relabeledObject = labelObjects.at(0).toObject();
        QCOMPARE(relabeledObject.value(QStringLiteral("label")).toString(), QStringLiteral("repaired"));
        QCOMPARE(relabeledObject.value(QStringLiteral("futureObjectField")).toString(),
                 QStringLiteral("preserve-object-extension"));
        QCOMPARE(relabeledObject.value(QStringLiteral("prompts"))
                     .toObject()
                     .value(QStringLiteral("futurePromptField"))
                     .toArray()
                     .size(),
                 3);
        QCOMPARE(relabeledObject.value(QStringLiteral("mask_rle")).toString(),
                 firstJson.value(QStringLiteral("mask_rle")).toString());
        QVERIFY(!QJsonDocument(labelResult).toJson(QJsonDocument::Compact).contains("private-mask-bytes"));

        const QString deleteOutput = QDir(transformDir.path()).filePath(QStringLiteral("deleted.deeplux-anno.json"));
        const QJsonObject deleteParams{{QStringLiteral("sessionPath"), sourcePath},
                                       {QStringLiteral("outputPath"), deleteOutput},
                                       {QStringLiteral("operation"), QStringLiteral("delete_objects")},
                                       {QStringLiteral("objectIds"), QJsonArray{QStringLiteral("object-b")}}};
        const QJsonObject deleteResult = AnnotationSessionTransform::execute(
            deleteParams, AnnotationSessionTransform::confirmationSnapshot(deleteParams));
        QCOMPARE(deleteResult.value(QStringLiteral("status")).toString(), QStringLiteral("transformed"));
        QCOMPARE(deleteResult.value(QStringLiteral("changedCount")).toInt(), 1);
        QCOMPARE(deleteResult.value(QStringLiteral("objectCount")).toInt(), 1);
        QFile deleteFile(deleteOutput);
        QVERIFY(deleteFile.open(QIODevice::ReadOnly));
        const QJsonObject deleteRoot = QJsonDocument::fromJson(deleteFile.readAll()).object();
        QCOMPARE(deleteRoot.value(QStringLiteral("futureRootField")).toObject().value(QStringLiteral("keep")).toInt(),
                 17);
        QCOMPARE(deleteRoot.value(QStringLiteral("annotations")).toArray().size(), 1);
        QFile unchangedSource(sourcePath);
        QVERIFY(unchangedSource.open(QIODevice::ReadOnly));
        QCOMPARE(unchangedSource.readAll(), originalBytes);

        QJsonObject duplicateIds = labelParams;
        duplicateIds.insert(QStringLiteral("outputPath"),
                            QDir(transformDir.path()).filePath(QStringLiteral("duplicate.deeplux-anno.json")));
        duplicateIds.insert(QStringLiteral("objectIds"),
                            QJsonArray{QStringLiteral("object-a"), QStringLiteral("object-a")});
        QCOMPARE(AnnotationSessionTransform::preflight(duplicateIds).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("INVALID_PARAM"));
        QVERIFY(!QFileInfo::exists(duplicateIds.value(QStringLiteral("outputPath")).toString()));

        QJsonObject unknownId = labelParams;
        const QString unknownOutput =
            QDir(transformDir.path()).filePath(QStringLiteral("unknown-id.deeplux-anno.json"));
        unknownId.insert(QStringLiteral("outputPath"), unknownOutput);
        unknownId.insert(QStringLiteral("objectIds"), QJsonArray{QStringLiteral("not-in-session")});
        QCOMPARE(AnnotationSessionTransform::preflight(unknownId).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("ANNOTATION_ID_NOT_FOUND"));
        QVERIFY(!QFileInfo::exists(unknownOutput));

        const QString staleOutput =
            QDir(transformDir.path()).filePath(QStringLiteral("stale-transform.deeplux-anno.json"));
        QJsonObject staleParams = labelParams;
        staleParams.insert(QStringLiteral("outputPath"), staleOutput);
        const QJsonObject staleSnapshot = AnnotationSessionTransform::confirmationSnapshot(staleParams);
        const QByteArray changedBytes = originalBytes + QByteArrayLiteral("\n");
        QFile sourceChange(sourcePath);
        QVERIFY(sourceChange.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(sourceChange.write(changedBytes), qint64(changedBytes.size()));
        sourceChange.close();
        QCOMPARE(AnnotationSessionTransform::execute(staleParams, staleSnapshot)
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("CONFIRMATION_EXPIRED"));
        QVERIFY(!QFileInfo::exists(staleOutput));
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

    void samPredictUploadedRequestTimeoutIsUncertain() {
        AgentController& controller = AgentController::instance();
        QVERIFY(controller.initialize());
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        qRegisterMetaType<AgentActionLogEntry>("AgentActionLogEntry");
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy actionCompleted(&controller, &AgentController::agentActionCompleted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);

        const QJsonObject params{
            {"imagePath", m_imagePath}, {"points", QJsonArray{QJsonArray{32, 24}}}, {"timeoutMs", 200}};

        SamTestServer setImageServer;
        QVERIFY(setImageServer.listen());
        setImageServer.withholdResponsePaths.insert(QStringLiteral("/set_image"));
        QJsonObject setImageParams = params;
        setImageParams.insert(QStringLiteral("serverUrl"), setImageServer.url());
        QJsonObject pending = controller.handleToolCall(QStringLiteral("sam_predict"), setImageParams);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        QJsonObject result = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(result.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(result.value(QStringLiteral("retryable")).toBool(), false);
        QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("SIDE_EFFECT_UNCERTAIN"));
        QCOMPARE(result.value(QStringLiteral("uncertainStage")).toString(), QStringLiteral("set_image"));
        QVERIFY(result.value(QStringLiteral("error")).toString().contains(QStringLiteral("超时")));
        QCOMPARE(setImageServer.requestLog.count(QStringLiteral("POST /set_image")), 1);
        QCOMPARE(setImageServer.setImageCount, 1);
        QCOMPARE(setImageServer.requestLog.count(QStringLiteral("POST /predict")), 0);
        QVERIFY(!result.contains(QStringLiteral("polygon")));
        QVERIFY(!result.contains(QStringLiteral("score")));
        const QJsonObject setImageCompleted = actionCompleted.last().at(1).toJsonObject();
        QCOMPARE(setImageCompleted.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(setImageCompleted.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(setImageCompleted.value(QStringLiteral("retryable")).toBool(), false);
        QCOMPARE(setImageCompleted.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("SIDE_EFFECT_UNCERTAIN"));
        QCOMPARE(setImageCompleted.value(QStringLiteral("uncertainStage")).toString(), QStringLiteral("set_image"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("uncertain"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);
        controller.clearConversation();
        toolExecuted.clear();
        SamTestServer predictServer;
        QVERIFY(predictServer.listen());
        predictServer.withholdResponsePaths.insert(QStringLiteral("/predict"));
        QJsonObject predictParams = params;
        predictParams.insert(QStringLiteral("serverUrl"), predictServer.url());
        pending = controller.handleToolCall(QStringLiteral("sam_predict"), predictParams);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        result = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(result.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(result.value(QStringLiteral("retryable")).toBool(), false);
        QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("SIDE_EFFECT_UNCERTAIN"));
        QCOMPARE(result.value(QStringLiteral("uncertainStage")).toString(), QStringLiteral("predict"));
        QCOMPARE(predictServer.requestLog.count(QStringLiteral("POST /set_image")), 1);
        QCOMPARE(predictServer.setImageCount, 1);
        QCOMPARE(predictServer.requestLog.count(QStringLiteral("POST /predict")), 1);
        QVERIFY(predictServer.lastPredictBody.contains(QStringLiteral("points_pos")));
        QVERIFY(!result.contains(QStringLiteral("polygon")));
        QVERIFY(!result.contains(QStringLiteral("score")));
        const QJsonObject predictCompleted = actionCompleted.last().at(1).toJsonObject();
        QCOMPARE(predictCompleted.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(predictCompleted.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(predictCompleted.value(QStringLiteral("retryable")).toBool(), false);
        QCOMPARE(predictCompleted.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("SIDE_EFFECT_UNCERTAIN"));
        QCOMPARE(predictCompleted.value(QStringLiteral("uncertainStage")).toString(), QStringLiteral("predict"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("uncertain"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);
        QCoreApplication::processEvents();
        QCOMPARE(predictServer.requestLog.count(QStringLiteral("POST /set_image")), 1);
        QCOMPARE(predictServer.requestLog.count(QStringLiteral("POST /predict")), 1);

        SamTestServer batchServer;
        QVERIFY(batchServer.listen());
        batchServer.withholdResponseAtRequestCount.insert(QStringLiteral("/predict"), 2);
        SamPromptItem firstPrompt;
        firstPrompt.positivePoints = {QPointF(10, 10)};
        SamPromptItem secondPrompt;
        secondPrompt.box = QRectF(1, 1, 10, 10);
        const SamPredictOutcome batchOutcome =
            runSamPredictBatch(batchServer.url(), m_imagePath, {firstPrompt, secondPrompt}, 200);
        QVERIFY(!batchOutcome.ok);
        QCOMPARE(batchOutcome.uncertainStage, QStringLiteral("predict"));
        QCOMPARE(batchServer.requestLog.count(QStringLiteral("POST /set_image")), 1);
        QCOMPARE(batchServer.requestLog.count(QStringLiteral("POST /predict")), 2);

        SamTestServer preflightServer;
        QVERIFY(preflightServer.listen());
        AgentActor actor;
        QJsonObject invalidParams = predictParams;
        invalidParams.insert(QStringLiteral("serverUrl"), preflightServer.url());
        invalidParams.insert(QStringLiteral("imagePath"), QStringLiteral("/missing/sam-image.png"));
        const QJsonObject invalid = actor.executeTool(QStringLiteral("sam_predict"), invalidParams);
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PRECONDITION_FAILED"));
        QVERIFY(!invalid.contains(QStringLiteral("sideEffectStatus")));
        QVERIFY(!invalid.contains(QStringLiteral("uncertainStage")));
        QVERIFY(preflightServer.requestLog.isEmpty());

        QVERIFY(!audit.isEmpty());
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void samAnnotateTimeoutIsUncertainAndDoesNotWriteSession() {
        AgentController& controller = AgentController::instance();
        QVERIFY(controller.initialize());
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        qRegisterMetaType<AgentActionLogEntry>("AgentActionLogEntry");
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy actionCompleted(&controller, &AgentController::agentActionCompleted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);

        const QString sliceDir = QDir(m_dir.path()).filePath(QStringLiteral("slice63"));
        QVERIFY(QDir().mkpath(sliceDir));
        const QString sessionPath = QDir(sliceDir).filePath(QStringLiteral("sam-annotate-preserve.deeplux-anno.json"));
        SamTestServer seedServer;
        QVERIFY(seedServer.listen());
        AgentActor seedActor;
        const QJsonObject seedResult = seedActor.executeTool(
            QStringLiteral("sam_annotate"), annotateParams(seedServer.url(), sessionPath, m_imagePath));
        QVERIFY2(!seedResult.contains(QStringLiteral("error")),
                 qPrintable(seedResult.value(QStringLiteral("error")).toString()));
        QFile originalSession(sessionPath);
        QVERIFY(originalSession.open(QIODevice::ReadOnly));
        const QByteArray originalBytes = originalSession.readAll();
        originalSession.close();
        QString loadError;
        const AnnotationSession original = AnnotationSession::load(sessionPath, &loadError);
        QVERIFY(loadError.isEmpty());
        QVERIFY(!original.annotations.isEmpty());

        QJsonObject params{{"imagePath", m_imagePath},
                           {"sessionPath", sessionPath},
                           {"items", QJsonArray{QJsonObject{{"points", QJsonArray{QJsonArray{32, 24}}},
                                                            {"label", QStringLiteral("timeout-object")}}}},
                           {"timeoutMs", 200}};

        SamTestServer predictServer;
        QVERIFY(predictServer.listen());
        predictServer.withholdResponsePaths.insert(QStringLiteral("/predict"));
        params.insert(QStringLiteral("serverUrl"), predictServer.url());
        QJsonObject pending = controller.handleToolCall(QStringLiteral("sam_annotate"), params);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(predictServer.requestLog.size(), 0);
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        QJsonObject result = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(result.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(result.value(QStringLiteral("retryable")).toBool(), false);
        QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("SIDE_EFFECT_UNCERTAIN"));
        QCOMPARE(result.value(QStringLiteral("uncertainStage")).toString(), QStringLiteral("predict"));
        QVERIFY(!result.contains(QStringLiteral("objectIds")));
        QVERIFY(!result.contains(QStringLiteral("annotated")));
        QCOMPARE(predictServer.requestLog.count(QStringLiteral("POST /set_image")), 1);
        QCOMPARE(predictServer.requestLog.count(QStringLiteral("POST /predict")), 1);
        QFile unchangedFile(sessionPath);
        QVERIFY(unchangedFile.open(QIODevice::ReadOnly));
        QCOMPARE(unchangedFile.readAll(), originalBytes);
        unchangedFile.close();
        const AnnotationSession unchanged = AnnotationSession::load(sessionPath, &loadError);
        QVERIFY(loadError.isEmpty());
        QCOMPARE(unchanged.annotations.size(), original.annotations.size());
        const QJsonObject predictCompleted = actionCompleted.last().at(1).toJsonObject();
        QCOMPARE(predictCompleted.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(predictCompleted.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(predictCompleted.value(QStringLiteral("retryable")).toBool(), false);
        QCOMPARE(predictCompleted.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("SIDE_EFFECT_UNCERTAIN"));
        QCOMPARE(predictCompleted.value(QStringLiteral("uncertainStage")).toString(), QStringLiteral("predict"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("uncertain"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);
        QCoreApplication::processEvents();
        QCOMPARE(predictServer.requestLog.count(QStringLiteral("POST /set_image")), 1);
        QCOMPARE(predictServer.requestLog.count(QStringLiteral("POST /predict")), 1);

        controller.clearConversation();
        toolExecuted.clear();
        const QString newSessionPath = QDir(sliceDir).filePath(QStringLiteral("sam-annotate-new.deeplux-anno.json"));
        SamTestServer setImageServer;
        QVERIFY(setImageServer.listen());
        setImageServer.withholdResponsePaths.insert(QStringLiteral("/set_image"));
        params.insert(QStringLiteral("sessionPath"), newSessionPath);
        params.insert(QStringLiteral("serverUrl"), setImageServer.url());
        pending = controller.handleToolCall(QStringLiteral("sam_annotate"), params);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        result = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(result.value(QStringLiteral("uncertainStage")).toString(), QStringLiteral("set_image"));
        QCOMPARE(setImageServer.requestLog.count(QStringLiteral("POST /set_image")), 1);
        QCOMPARE(setImageServer.requestLog.count(QStringLiteral("POST /predict")), 0);
        QVERIFY(!QFileInfo::exists(newSessionPath));
        const QJsonObject setImageCompleted = actionCompleted.last().at(1).toJsonObject();
        QCOMPARE(setImageCompleted.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(setImageCompleted.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(setImageCompleted.value(QStringLiteral("retryable")).toBool(), false);
        QCOMPARE(setImageCompleted.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("SIDE_EFFECT_UNCERTAIN"));
        QCOMPARE(setImageCompleted.value(QStringLiteral("uncertainStage")).toString(), QStringLiteral("set_image"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("uncertain"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);
        QCoreApplication::processEvents();
        QCOMPARE(setImageServer.requestLog.count(QStringLiteral("POST /set_image")), 1);
        QCOMPARE(setImageServer.requestLog.count(QStringLiteral("POST /predict")), 0);
        QVERIFY(!QFileInfo::exists(newSessionPath));

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
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

    // 批4（C）：sam_predict 工具 candidateIndex 参数——选中候选、候选摘要回传
    // （candidateCount/candidateScores/candidateIndex 回显），参数守卫失败关闭
    void samPredictToolCandidateIndex() {
        SamTestServer server;
        QVERIFY(server.listen());
        AgentActor actor;
        auto r =
            actor.executeTool(QStringLiteral("sam_predict"), QJsonObject{{"imagePath", m_imagePath},
                                                                         {"serverUrl", server.url()},
                                                                         {"points", QJsonArray{QJsonArray{15, 15}}},
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
            r = actor.executeTool(QStringLiteral("sam_predict"), QJsonObject{{"imagePath", m_imagePath},
                                                                             {"serverUrl", server.url()},
                                                                             {"points", QJsonArray{QJsonArray{15, 15}}},
                                                                             {"candidateIndex", bad}});
            QVERIFY2(r.value("error").toString().contains(QStringLiteral("[0,7]")),
                     qPrintable(r.value("error").toString()));
        }
        QVERIFY2(server.requestLog.isEmpty(), "param guard must fail before any network request");

        // 5 在 [0,7] 内但超本次候选数 → 运行时越界失败关闭
        r = actor.executeTool(QStringLiteral("sam_predict"), QJsonObject{{"imagePath", m_imagePath},
                                                                         {"serverUrl", server.url()},
                                                                         {"points", QJsonArray{QJsonArray{15, 15}}},
                                                                         {"candidateIndex", 5},
                                                                         {"timeoutMs", 5000}});
        QVERIFY(r.value("error").toString().contains(QStringLiteral("越界")));
    }

    void samPredictRequiresConfirmationAndDisclosesOnlySafeOrigin() {
        SamTestServer server;
        QVERIFY(server.listen());

        AgentController& controller = AgentController::instance();
        QVERIFY(controller.initialize());
        qRegisterMetaType<AgentActionLogEntry>("AgentActionLogEntry");
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);

        QJsonObject params{{"imagePath", m_imagePath},
                           {"serverUrl", server.url()},
                           {"points", QJsonArray{QJsonArray{32, 24}}},
                           {"label", QStringLiteral("widget")},
                           {"timeoutMs", 5000}};
        QUrl decorated(server.url());
        decorated.setUserName(QStringLiteral("sam-user"));
        decorated.setPassword(QStringLiteral("sam-secret"));
        decorated.setPath(QStringLiteral("/private/model-prefix"));
        decorated.setQuery(QStringLiteral("token=private-query"));
        decorated.setFragment(QStringLiteral("private-fragment"));
        params.insert(QStringLiteral("serverUrl"), decorated.toString());

        QJsonObject pending = controller.handleToolCall(QStringLiteral("sam_predict"), params);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString explicitOrigin = QStringLiteral("http://127.0.0.1:%1").arg(QUrl(server.url()).port());
        const QJsonArray expectedExplicitEffect{
            QStringLiteral("sam_predict:image=%1 server=%2").arg(m_imagePath, explicitOrigin),
            expectedImageEffect(m_imagePath)};
        const QJsonObject explicitContext =
            controller.pendingConfirmationPayload().value(QStringLiteral("context")).toObject();
        const QJsonArray explicitEffects = explicitContext.value(QStringLiteral("sideEffects")).toArray();
        QCOMPARE(explicitEffects, expectedExplicitEffect);
        const QString explicitSummary = explicitContext.value(QStringLiteral("summary")).toString();
        QVERIFY(explicitSummary.contains(explicitOrigin));
        QVERIFY(!explicitSummary.contains(QStringLiteral("sam-user")));
        QVERIFY(!explicitSummary.contains(QStringLiteral("sam-secret")));
        QVERIFY(!explicitSummary.contains(QStringLiteral("private/model-prefix")));
        QVERIFY(!explicitSummary.contains(QStringLiteral("private-query")));
        QVERIFY(!explicitSummary.contains(QStringLiteral("private-fragment")));
        QVERIFY(!explicitSummary.contains(QStringLiteral("widget")));
        QCOMPARE(server.requestLog.size(), 0);
        QCOMPARE(server.setImageCount, 0);
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QCOMPARE(server.requestLog.size(), 0);
        QCOMPARE(server.setImageCount, 0);

        params.insert(QStringLiteral("serverUrl"),
                      QStringLiteral("http://user:secret@127.0.0.1/private-path?token=hidden#hidden"));
        pending = controller.handleToolCall(QStringLiteral("sam_predict"), params);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonArray expectedDefaultOriginEffect{
            QStringLiteral("sam_predict:image=%1 server=http://127.0.0.1:80").arg(m_imagePath),
            expectedImageEffect(m_imagePath)};
        const QJsonObject defaultOriginContext =
            controller.pendingConfirmationPayload().value(QStringLiteral("context")).toObject();
        const QJsonArray defaultOriginEffects = defaultOriginContext.value(QStringLiteral("sideEffects")).toArray();
        QCOMPARE(defaultOriginEffects, expectedDefaultOriginEffect);
        const QString defaultOriginSummary = defaultOriginContext.value(QStringLiteral("summary")).toString();
        QVERIFY(defaultOriginSummary.contains(QStringLiteral("http://127.0.0.1:80")));
        QVERIFY(!defaultOriginSummary.contains(QStringLiteral("user:secret")));
        QVERIFY(!defaultOriginSummary.contains(QStringLiteral("private-path")));
        QVERIFY(!defaultOriginSummary.contains(QStringLiteral("token=hidden")));
        QVERIFY(!defaultOriginSummary.contains(QStringLiteral("#hidden")));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QCOMPARE(server.requestLog.size(), 0);
        QCOMPARE(server.setImageCount, 0);

        params.insert(QStringLiteral("serverUrl"), server.url());
        pending = controller.handleToolCall(QStringLiteral("sam_predict"), params);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        const QJsonObject result = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("predicted"));
        QCOMPARE(result.value(QStringLiteral("label")).toString(), QStringLiteral("widget"));
        QVERIFY(qAbs(result.value(QStringLiteral("score")).toDouble() - 0.97) < 1e-9);
        QCOMPARE(result.value(QStringLiteral("bbox")).toArray(), (QJsonArray{10, 10, 10, 10}));
        QCOMPARE(server.setImageCount, 1);
        QCOMPARE(server.requestLog.count(QStringLiteral("POST /set_image")), 1);
        QCOMPARE(server.requestLog.count(QStringLiteral("POST /predict")), 1);
        QCOMPARE(audit.size(), 3);
        QCOMPARE(audit.at(0).at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QCOMPARE(audit.at(1).at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QCOMPARE(audit.at(2).at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QCOMPARE(QDir(m_dir.path()).entryList({QStringLiteral("*.deeplux-anno.json")}, QDir::Files).size(), 0);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        controller.clearConversation();
    }

    void samPredictConfirmationExpiresWhenInputImageBytesChange() {
        const QString imagePath = m_dir.filePath(QStringLiteral("sam-predict-confirm.png"));
        QImage original(32, 24, QImage::Format_RGB32);
        original.fill(Qt::gray);
        QVERIFY(original.save(imagePath, "PNG"));

        SamTestServer server;
        QVERIFY(server.listen());
        AgentController& controller = AgentController::instance();
        QVERIFY(controller.initialize());
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        qRegisterMetaType<AgentActionLogEntry>("AgentActionLogEntry");
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        const QJsonObject params{{"imagePath", imagePath},
                                 {"serverUrl", server.url()},
                                 {"points", QJsonArray{QJsonArray{8, 8}}},
                                 {"timeoutMs", 5000}};

        QJsonObject pending = controller.handleToolCall(QStringLiteral("sam_predict"), params);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString staleId = controller.pendingConfirmationId();
        const QJsonArray frozenEffects = controller.pendingConfirmationPayload()
                                             .value(QStringLiteral("context"))
                                             .toObject()
                                             .value(QStringLiteral("sideEffects"))
                                             .toArray();
        QVERIFY(frozenEffects.contains(expectedImageEffect(imagePath)));
        QVERIFY(server.requestLog.isEmpty());

        QImage replacement(32, 24, QImage::Format_RGB32);
        replacement.fill(Qt::blue);
        QVERIFY(replacement.save(imagePath, "PNG"));
        QVERIFY(!frozenEffects.contains(expectedImageEffect(imagePath)));
        QVERIFY(controller.confirmPendingTools(staleId));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QVERIFY(server.requestLog.isEmpty());
        QCOMPARE(server.setImageCount, 0);
        QCOMPARE(toolExecuted.count(), 0);

        pending = controller.handleToolCall(QStringLiteral("sam_predict"), params);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString currentId = controller.pendingConfirmationId();
        QVERIFY(!currentId.isEmpty());
        QVERIFY(currentId != staleId);
        const QJsonArray currentEffects = controller.pendingConfirmationPayload()
                                              .value(QStringLiteral("context"))
                                              .toObject()
                                              .value(QStringLiteral("sideEffects"))
                                              .toArray();
        QVERIFY(currentEffects.contains(expectedImageEffect(imagePath)));
        QVERIFY(controller.confirmPendingTools(currentId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        QCOMPARE(toolExecuted.last().at(1).toJsonObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("predicted"));
        QCOMPARE(server.requestLog.count(QStringLiteral("POST /set_image")), 1);
        QCOMPARE(server.requestLog.count(QStringLiteral("POST /predict")), 1);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));

        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    // ---------- sam_annotate ----------

    void samAnnotateConfirmationExpiresWhenInputImageBytesChange() {
        const QString imagePath = m_dir.filePath(QStringLiteral("sam-annotate-confirm.png"));
        const QString sessionPath = m_dir.filePath(QStringLiteral("sam-annotate-confirm.deeplux-anno.json"));
        QImage original(32, 24, QImage::Format_RGB32);
        original.fill(Qt::gray);
        QVERIFY(original.save(imagePath, "PNG"));

        SamTestServer server;
        QVERIFY(server.listen());
        AgentController& controller = AgentController::instance();
        QVERIFY(controller.initialize());
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        qRegisterMetaType<AgentActionLogEntry>("AgentActionLogEntry");
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QJsonObject params = annotateParams(server.url(), sessionPath, imagePath);
        params.insert(QStringLiteral("items"), QJsonArray{QJsonObject{{"points", QJsonArray{QJsonArray{8, 8}}},
                                                                      {"label", QStringLiteral("slice95")}}});

        QJsonObject pending = controller.handleToolCall(QStringLiteral("sam_annotate"), params);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString staleId = controller.pendingConfirmationId();
        const QJsonArray frozenEffects = controller.pendingConfirmationPayload()
                                             .value(QStringLiteral("context"))
                                             .toObject()
                                             .value(QStringLiteral("sideEffects"))
                                             .toArray();
        QVERIFY(frozenEffects.contains(expectedImageEffect(imagePath)));
        QVERIFY(server.requestLog.isEmpty());
        QVERIFY(!QFileInfo::exists(sessionPath));

        QImage replacement(32, 24, QImage::Format_RGB32);
        replacement.fill(Qt::yellow);
        QVERIFY(replacement.save(imagePath, "PNG"));
        QVERIFY(!frozenEffects.contains(expectedImageEffect(imagePath)));
        QVERIFY(controller.confirmPendingTools(staleId));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QVERIFY(server.requestLog.isEmpty());
        QCOMPARE(server.setImageCount, 0);
        QCOMPARE(toolExecuted.count(), 0);
        QVERIFY(!QFileInfo::exists(sessionPath));

        pending = controller.handleToolCall(QStringLiteral("sam_annotate"), params);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString currentId = controller.pendingConfirmationId();
        QVERIFY(!currentId.isEmpty());
        QVERIFY(currentId != staleId);
        QVERIFY(controller.confirmPendingTools(currentId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        QCOMPARE(toolExecuted.last().at(1).toJsonObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("annotated"));
        QCOMPARE(server.requestLog.count(QStringLiteral("POST /set_image")), 1);
        QCOMPARE(server.requestLog.count(QStringLiteral("POST /predict")), 1);
        QVERIFY(QFileInfo::exists(sessionPath));

        QString loadError;
        const AnnotationSession saved = AnnotationSession::load(sessionPath, &loadError);
        QVERIFY2(loadError.isEmpty(), qPrintable(loadError));
        const QImage decodedTarget(imagePath);
        QVERIFY(!decodedTarget.isNull());
        QCOMPARE(saved.imageDigest, AnnotationSession::computeImageDigest(decodedTarget));
        QCOMPARE(saved.annotations.size(), 1);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));

        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void samAnnotateConfirmationBindsSessionSymlinkTarget() {
#ifndef Q_OS_UNIX
        QSKIP("The symlink target fixture currently requires POSIX file symlinks");
#else
        AgentController& controller = AgentController::instance();
        QVERIFY(controller.initialize());
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        qRegisterMetaType<AgentActionLogEntry>("AgentActionLogEntry");
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);

        const QString linkPath = m_dir.filePath(QStringLiteral("sam session link.deeplux-anno.json"));
        const QString targetB = m_dir.filePath(QStringLiteral("sam session target B.deeplux-anno.json"));
        const QString targetC = m_dir.filePath(QStringLiteral("sam session target C.deeplux-anno.json"));
        QVERIFY(!QFileInfo::exists(targetB));
        QVERIFY(!QFileInfo::exists(targetC));
        if (!QFile::link(targetB, linkPath))
            QSKIP("Unable to create POSIX symlink for sam_annotate session target test");

        SamTestServer server;
        QVERIFY(server.listen());
        QJsonObject params = annotateParams(server.url(), linkPath, m_imagePath);
        params.insert(QStringLiteral("items"), QJsonArray{QJsonObject{{"points", QJsonArray{QJsonArray{32, 24}}},
                                                                      {"label", QStringLiteral("symlink-target")}}});
        QJsonObject pending = controller.handleToolCall(QStringLiteral("sam_annotate"), params);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(server.requestLog.size(), 0);
        const auto hasTarget = [&controller](const QString& configured, const QString& target) {
            const QJsonArray effects = controller.pendingConfirmationPayload()
                                           .value(QStringLiteral("context"))
                                           .toObject()
                                           .value(QStringLiteral("sideEffects"))
                                           .toArray();
            const QString expected = QStringLiteral("file_write:%1 resolved_target=%2").arg(configured, target);
            return std::any_of(effects.cbegin(), effects.cend(),
                               [&expected](const QJsonValue& effect) { return effect.toString() == expected; });
        };
        QVERIFY(hasTarget(linkPath, targetB));
        const QString staleId = controller.pendingConfirmationId();
        QVERIFY(!staleId.isEmpty());
        QVERIFY(QFile::remove(linkPath));
        QVERIFY(QFile::link(targetC, linkPath));
        QVERIFY(hasTarget(linkPath, targetB));
        QVERIFY(controller.confirmPendingTools(staleId));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QCOMPARE(server.requestLog.size(), 0);
        QVERIFY(!QFileInfo::exists(targetB));
        QVERIFY(!QFileInfo::exists(targetC));

        pending = controller.handleToolCall(QStringLiteral("sam_annotate"), params);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(hasTarget(linkPath, targetC));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != staleId);
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        QCOMPARE(toolExecuted.last().at(0).toString(), QStringLiteral("sam_annotate"));
        const QJsonObject result = toolExecuted.last().at(1).toJsonObject();
        QVERIFY2(!result.contains(QStringLiteral("error")),
                 qPrintable(result.value(QStringLiteral("error")).toString()));
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("annotated"));
        QCOMPARE(result.value(QStringLiteral("sessionPath")).toString(), linkPath);
        QCOMPARE(server.requestLog.count(QStringLiteral("POST /set_image")), 1);
        QCOMPARE(server.requestLog.count(QStringLiteral("POST /predict")), 1);
        QVERIFY(!QFileInfo::exists(targetB));
        QVERIFY(QFileInfo::exists(targetC));
        QString loadError;
        const AnnotationSession saved = AnnotationSession::load(targetC, &loadError);
        QVERIFY2(loadError.isEmpty(), qPrintable(loadError));
        QCOMPARE(saved.annotations.size(), 1);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));

        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
#endif
    }

    void samAnnotateWritesSessionAndAppends() {
        SamTestServer server;
        QVERIFY(server.listen());
        const QString sessionPath = QDir(m_dir.path()).filePath(QStringLiteral("session.deeplux-anno.json"));
        const QJsonObject params = annotateParams(server.url(), sessionPath, m_imagePath);

        AgentController& controller = AgentController::instance();
        QVERIFY(controller.initialize());
        qRegisterMetaType<AgentActionLogEntry>("AgentActionLogEntry");
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        QJsonObject annotateResult;
        QObject annotateResultContext;
        connect(controller.actor(), &AgentActor::toolExecuted, &annotateResultContext,
                [&annotateResult](const QString& toolName, const QJsonObject& result) {
                    if (toolName == QLatin1String("sam_annotate"))
                        annotateResult = result;
                });

        const QJsonObject rejected = controller.handleToolCall(QStringLiteral("sam_annotate"), params);
        QCOMPARE(rejected.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonArray expectedSideEffects{QStringLiteral("file_write:%1").arg(sessionPath),
                                             expectedImageEffect(m_imagePath), QStringLiteral("inference:sam_server")};
        QCOMPARE(controller.pendingConfirmationPayload()
                     .value(QStringLiteral("context"))
                     .toObject()
                     .value(QStringLiteral("sideEffects"))
                     .toArray(),
                 expectedSideEffects);
        QVERIFY(!QFileInfo::exists(sessionPath));
        QVERIFY(server.requestLog.isEmpty());
        QCOMPARE(server.setImageCount, 0);
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("sam_annotate"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);
        QVERIFY(!QFileInfo::exists(sessionPath));
        QVERIFY(server.requestLog.isEmpty());
        QCOMPARE(server.setImageCount, 0);

        const QJsonObject approved = controller.handleToolCall(QStringLiteral("sam_annotate"), params);
        QCOMPARE(approved.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != rejectedId);
        QVERIFY(controller.confirmPendingTools(approvedId));
        QVERIFY(QFileInfo::exists(sessionPath));
        QCOMPARE(annotateResult.value(QStringLiteral("status")).toString(), QStringLiteral("annotated"));
        QCOMPARE(annotateResult.value(QStringLiteral("added")).toInt(), 2);
        QCOMPARE(annotateResult.value(QStringLiteral("total")).toInt(), 2);
        QCOMPARE(annotateResult.value(QStringLiteral("objectIds")).toArray().size(), 2);
        QCOMPARE(server.setImageCount, 1);
        QCOMPARE(server.requestLog.count(QStringLiteral("POST /predict")), 2);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("sam_annotate"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);

        AgentActor actor;
        QJsonObject r = annotateResult;
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
        auto r =
            actor.executeTool(QStringLiteral("sam_annotate"),
                              QJsonObject{{"imagePath", m_imagePath},
                                          {"sessionPath", sessionPath},
                                          {"serverUrl", server.url()},
                                          {"timeoutMs", 5000},
                                          {"items", QJsonArray{QJsonObject{{"points", QJsonArray{QJsonArray{15, 15}}},
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
        r = actor.executeTool(QStringLiteral("sam_annotate"),
                              QJsonObject{{"imagePath", m_imagePath},
                                          {"sessionPath", session2},
                                          {"serverUrl", server.url()},
                                          {"items", QJsonArray{QJsonObject{{"points", QJsonArray{QJsonArray{15, 15}}},
                                                                           {"candidateIndex", 9}}}}});
        QVERIFY(r.value("error").toString().contains(QStringLiteral("items[0].candidateIndex")));
        QVERIFY2(server.requestLog.isEmpty(), "param guard must fail before any network request");
        QVERIFY(!QFileInfo::exists(session2));

        // 运行时越界：candidateIndex=5（合法参数、超本次候选数）→ 全有或全无
        const QString session3 = QDir(m_dir.path()).filePath(QStringLiteral("cand3.deeplux-anno.json"));
        r = actor.executeTool(QStringLiteral("sam_annotate"),
                              QJsonObject{{"imagePath", m_imagePath},
                                          {"sessionPath", session3},
                                          {"serverUrl", server.url()},
                                          {"timeoutMs", 5000},
                                          {"items", QJsonArray{QJsonObject{{"points", QJsonArray{QJsonArray{15, 15}}},
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

    void grabImagePathSamPredictConfirmationExpiresOnSameSizeImageReplacement() {
        const QByteArray originalImageBytes = [&]() {
            QFile file(m_imagePath);
            if (!file.open(QIODevice::ReadOnly))
                return QByteArray{};
            return file.readAll();
        }();
        QVERIFY(!originalImageBytes.isEmpty());
        ScopeExit restoreSourceImage{[this, originalImageBytes]() {
            QFile file(m_imagePath);
            if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
                file.write(originalImageBytes);
        }};

        QImage oldImage(64, 48, QImage::Format_RGB32);
        oldImage.fill(Qt::gray);
        QVERIFY(oldImage.save(m_imagePath, "PNG"));
        QFile oldSourceFile(m_imagePath);
        QVERIFY(oldSourceFile.open(QIODevice::ReadOnly));
        const QByteArray oldSourceBytes = oldSourceFile.readAll();
        oldSourceFile.close();
        const QString oldHash =
            QString::fromLatin1(QCryptographicHash::hash(oldSourceBytes, QCryptographicHash::Sha256).toHex());

        RunEngine& engine = RunEngine::instance();
        QTemporaryDir appData;
        QVERIFY(appData.isValid());
        const bool hadAppDataOverride = qEnvironmentVariableIsSet("DEEPLUX_APP_DATA_DIR");
        const QByteArray previousAppDataOverride = qgetenv("DEEPLUX_APP_DATA_DIR");
        qputenv("DEEPLUX_APP_DATA_DIR", appData.path().toLocal8Bit());
        ScopeExit restoreAppData{[hadAppDataOverride, previousAppDataOverride]() {
            if (hadAppDataOverride)
                qputenv("DEEPLUX_APP_DATA_DIR", previousAppDataOverride);
            else
                qunsetenv("DEEPLUX_APP_DATA_DIR");
        }};
        const auto installPlugin = [&appData](const QString& name, const char* libraryPath, const char* metadataPath) {
            const QString pluginDirectory = QDir(appData.path()).filePath(QStringLiteral("plugins/") + name);
            if (!QDir().mkpath(pluginDirectory))
                return false;
            return QFile::copy(QString::fromLocal8Bit(metadataPath),
                               QDir(pluginDirectory).filePath(QStringLiteral("metadata.json"))) &&
                   QFile::copy(
                       QString::fromLocal8Bit(libraryPath),
                       QDir(pluginDirectory).filePath(QFileInfo(QString::fromLocal8Bit(libraryPath)).fileName()));
        };
        QVERIFY(installPlugin(QStringLiteral("GrabImage"), AGENT_SAM_G5_GRABIMAGE_LIBRARY,
                              AGENT_SAM_G5_GRABIMAGE_METADATA));
        QVERIFY(installPlugin(QStringLiteral("SamPredict"), AGENT_SAM_G5_SAMPREDICT_LIBRARY,
                              AGENT_SAM_G5_SAMPREDICT_METADATA));

        PluginManager& plugins = PluginManager::instance();
        QVERIFY(plugins.initialize());
        ScopeExit shutdownPlugins{[&plugins]() {
            if (plugins.isInitialized())
                plugins.shutdown();
        }};
        QVERIFY(plugins.loadPlugin(QStringLiteral("GrabImage")));
        QVERIFY(plugins.loadPlugin(QStringLiteral("SamPredict")));

        ProjectManager& projects = ProjectManager::instance();
        projects.closeProject();
        Project* project = projects.newProject();
        QVERIFY(project);
        const QString grabId = QStringLiteral("slice99_grab_image");
        const QString predictId = QStringLiteral("slice99_sam_predict");
        ModuleInstance grab;
        grab.id = grabId;
        grab.moduleId = QStringLiteral("GrabImage");
        grab.name = QStringLiteral("Slice 99 file image source");
        grab.params = QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Path")},
                                  {QStringLiteral("filePath"), m_imagePath}};
        project->addModule(grab);
        const QString unconnectedPath = QDir(m_dir.path()).filePath(QStringLiteral("slice99-unconnected.png"));
        QImage unconnectedImage(64, 48, QImage::Format_RGB32);
        unconnectedImage.fill(Qt::blue);
        QVERIFY(unconnectedImage.save(unconnectedPath, "PNG"));
        ModuleInstance unconnectedGrab;
        unconnectedGrab.id = QStringLiteral("slice99_unconnected_grab_image");
        unconnectedGrab.moduleId = QStringLiteral("GrabImage");
        unconnectedGrab.name = QStringLiteral("Slice 99 unconnected image source");
        unconnectedGrab.params = QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Path")},
                                             {QStringLiteral("filePath"), unconnectedPath}};
        project->addModule(unconnectedGrab);
        const QString disabledPath = QDir(m_dir.path()).filePath(QStringLiteral("slice99-disabled.png"));
        QImage disabledImage(64, 48, QImage::Format_RGB32);
        disabledImage.fill(Qt::yellow);
        QVERIFY(disabledImage.save(disabledPath, "PNG"));
        ModuleInstance disabledGrab;
        disabledGrab.id = QStringLiteral("slice99_disabled_grab_image");
        disabledGrab.moduleId = QStringLiteral("GrabImage");
        disabledGrab.name = QStringLiteral("Slice 99 disabled image source");
        disabledGrab.enabled = false;
        disabledGrab.params = QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Path")},
                                          {QStringLiteral("filePath"), disabledPath}};
        project->addModule(disabledGrab);
        SamTestServer server;
        QVERIFY(server.listen());
        ModuleInstance predict;
        predict.id = predictId;
        predict.moduleId = QStringLiteral("SamPredict");
        predict.name = QStringLiteral("Slice 99 file-backed prediction");
        predict.params = QJsonObject{{QStringLiteral("serverUrl"), server.url()},
                                     {QStringLiteral("label"), QStringLiteral("slice99")},
                                     {QStringLiteral("points"), QJsonArray{QJsonArray{32, 24}}},
                                     {QStringLiteral("timeoutMs"), 5000}};
        project->addModule(predict);
        ModuleConnection imageEdge;
        imageEdge.fromModuleId = grabId;
        imageEdge.toModuleId = predictId;
        imageEdge.fromPort = QStringLiteral("image");
        imageEdge.toPort = QStringLiteral("image");
        imageEdge.edgeType = QStringLiteral("data");
        project->addConnection(imageEdge);

        AgentController& controller = AgentController::instance();
        QVERIFY(controller.initialize());
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        qRegisterMetaType<AgentActionLogEntry>("AgentActionLogEntry");
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        ScopeExit cleanupRuntime{[&appData, &projects, &controller, &engine]() {
            engine.stop();
            QElapsedTimer cleanupWait;
            cleanupWait.start();
            while (engine.isBusy() && cleanupWait.elapsed() < 5000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                QTest::qWait(5);
            }
            if (engine.isBusy()) {
                appData.setAutoRemove(false);
                qFatal("Slice99 RunEngine cleanup exceeded 5000 ms; preserving isolated plugin directory");
            }
            engine.clearModules();
            projects.closeProject();
            controller.clearConversation();
            controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        }};
        engine.stop();
        engine.clearModules();

        const auto requestFlow = [&controller]() {
            return controller.handleToolCall(QStringLiteral("run_flow"),
                                             {{QStringLiteral("mode"), QStringLiteral("once")}});
        };
        const QString origin = QStringLiteral("http://127.0.0.1:%1").arg(QUrl(server.url()).port());
        QUrl decoratedUrl(server.url());
        decoratedUrl.setUserName(QStringLiteral("private-user"));
        decoratedUrl.setPassword(QStringLiteral("private-secret"));
        decoratedUrl.setPath(QStringLiteral("/private/model-prefix"));
        decoratedUrl.setQuery(QStringLiteral("token=private-query"));
        decoratedUrl.setFragment(QStringLiteral("private-fragment"));
        QVERIFY(project->setModuleParam(predictId, QStringLiteral("serverUrl"), decoratedUrl.toString()));
        QJsonObject pending = requestFlow();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonObject privatePayload = controller.pendingConfirmationPayload();
        const QJsonObject privateContext = privatePayload.value(QStringLiteral("context")).toObject();
        const QString privateEffectsSerialized =
            QString::fromUtf8(QJsonDocument(privateContext.value(QStringLiteral("sideEffects")).toArray())
                                  .toJson(QJsonDocument::Compact));
        QVERIFY(privateEffectsSerialized.contains(origin));
        QVERIFY(!privateEffectsSerialized.contains(QStringLiteral("private-user")));
        QVERIFY(!privateEffectsSerialized.contains(QStringLiteral("private-secret")));
        QVERIFY(!privateEffectsSerialized.contains(QStringLiteral("/private/model-prefix")));
        QVERIFY(!privateEffectsSerialized.contains(QStringLiteral("token=private-query")));
        QVERIFY(!privateEffectsSerialized.contains(QStringLiteral("private-fragment")));
        QVERIFY(privateEffectsSerialized.contains(m_imagePath));
        QVERIFY(!privateEffectsSerialized.contains(unconnectedPath));
        QVERIFY(!privateEffectsSerialized.contains(disabledPath));
        int imageEffectCount = 0;
        for (const QJsonValue& effect : privateContext.value(QStringLiteral("sideEffects")).toArray())
            if (effect.toString().startsWith(QStringLiteral("sam_input_image:")))
                ++imageEffectCount;
        QCOMPARE(imageEffectCount, 1);
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QVERIFY(server.requestLog.isEmpty());

        QVERIFY(project->setModuleParam(predictId, QStringLiteral("serverUrl"), server.url()));
        pending = requestFlow();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QJsonObject context = controller.pendingConfirmationPayload().value(QStringLiteral("context")).toObject();
        QJsonArray effects = context.value(QStringLiteral("sideEffects")).toArray();
        const QString imageEffect = QStringLiteral("sam_input_image:%1 bytes=%2 image_sha256=%3")
                                        .arg(m_imagePath)
                                        .arg(oldSourceBytes.size())
                                        .arg(oldHash.left(12));
        QVERIFY(effects.contains(imageEffect));
        QVERIFY(
            effects.contains(QStringLiteral("sam_predict_plugin:%1@%2 upstream_image=sent").arg(predictId, origin)));
        const QString unrelatedDriftId = controller.pendingConfirmationId();
        QVERIFY(!unrelatedDriftId.isEmpty());
        const QString initialEffectsSerialized =
            QString::fromUtf8(QJsonDocument(effects).toJson(QJsonDocument::Compact));
        QVERIFY(!initialEffectsSerialized.contains(unconnectedPath));
        QVERIFY(!initialEffectsSerialized.contains(disabledPath));
        QVERIFY(server.requestLog.isEmpty());
        QCOMPARE(moduleStarted.count(), 0);
        QCOMPARE(moduleFinished.count(), 0);
        QCOMPARE(runFinished.count(), 0);
        QVERIFY(!engine.isBusy());

        QImage changedUnconnected(64, 48, QImage::Format_RGB32);
        changedUnconnected.fill(Qt::cyan);
        QVERIFY(changedUnconnected.save(unconnectedPath, "PNG"));
        QImage changedDisabled(64, 48, QImage::Format_RGB32);
        changedDisabled.fill(Qt::magenta);
        QVERIFY(changedDisabled.save(disabledPath, "PNG"));
        QVERIFY(controller.confirmPendingTools(unrelatedDriftId));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 10000);
        QVERIFY(qvariant_cast<RunResult>(runFinished.last().at(0)).success);
        QCOMPARE(server.requestLog.count(QStringLiteral("POST /set_image")), 1);
        QCOMPARE(server.requestLog.count(QStringLiteral("POST /predict")), 1);
        QVERIFY(server.lastSetImageReadable);
        QCOMPARE(server.lastSetImage.pixelColor(0, 0), QColor(Qt::gray));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));

        server.requestLog.clear();
        server.setImageCount = 0;
        server.lastSetImageReadable = false;
        moduleStarted.clear();
        moduleFinished.clear();
        runFinished.clear();
        toolExecuted.clear();
        audit.clear();
        controller.clearConversation();

        pending = requestFlow();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        context = controller.pendingConfirmationPayload().value(QStringLiteral("context")).toObject();
        effects = context.value(QStringLiteral("sideEffects")).toArray();
        QVERIFY(effects.contains(imageEffect));
        const QString staleId = controller.pendingConfirmationId();
        QVERIFY(!staleId.isEmpty());

        QImage replacement(64, 48, QImage::Format_RGB32);
        replacement.fill(Qt::red);
        QVERIFY(replacement.save(m_imagePath, "PNG"));
        QFile replacementFile(m_imagePath);
        QVERIFY(replacementFile.open(QIODevice::ReadOnly));
        const QByteArray replacementBytes = replacementFile.readAll();
        replacementFile.close();
        const QString replacementHash =
            QString::fromLatin1(QCryptographicHash::hash(replacementBytes, QCryptographicHash::Sha256).toHex());
        QVERIFY(replacementHash != oldHash);
        QVERIFY(controller.confirmPendingTools(staleId));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QVERIFY(server.requestLog.isEmpty());
        QCOMPARE(moduleStarted.count(), 0);
        QCOMPARE(moduleFinished.count(), 0);
        QCOMPARE(runFinished.count(), 0);
        QVERIFY(!engine.isBusy());

        pending = requestFlow();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        context = controller.pendingConfirmationPayload().value(QStringLiteral("context")).toObject();
        effects = context.value(QStringLiteral("sideEffects")).toArray();
        const QString replacementEffect = QStringLiteral("sam_input_image:%1 bytes=%2 image_sha256=%3")
                                              .arg(m_imagePath)
                                              .arg(replacementBytes.size())
                                              .arg(replacementHash.left(12));
        QVERIFY(effects.contains(replacementEffect));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != staleId);
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 10000);
        const QJsonObject started = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(started.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        const QString runId = started.value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QCOMPARE(moduleSignalCount(moduleStarted, grabId), 1);
        QCOMPARE(moduleSignalCount(moduleStarted, predictId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, grabId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, predictId), 1);
        QCOMPARE(server.requestLog.count(QStringLiteral("POST /set_image")), 1);
        QCOMPARE(server.requestLog.count(QStringLiteral("POST /predict")), 1);
        QCOMPARE(server.setImageCount, 1);
        QVERIFY(server.lastSetImageReadable);
        QCOMPARE(server.lastSetImage.size(), QSize(64, 48));
        QCOMPARE(server.lastSetImage.pixelColor(0, 0), QColor(Qt::red));
        QVERIFY(!engine.isBusy());
        QVERIFY(!audit.isEmpty());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
    }

    void samPredictPluginFlowConfirmationBindsNetworkTarget() {
        RunEngine& engine = RunEngine::instance();
        QTemporaryDir appData;
        QVERIFY(appData.isValid());
        const bool hadAppDataOverride = qEnvironmentVariableIsSet("DEEPLUX_APP_DATA_DIR");
        const QByteArray previousAppDataOverride = qgetenv("DEEPLUX_APP_DATA_DIR");
        qputenv("DEEPLUX_APP_DATA_DIR", appData.path().toLocal8Bit());
        ScopeExit restoreAppData{[hadAppDataOverride, previousAppDataOverride]() {
            if (hadAppDataOverride)
                qputenv("DEEPLUX_APP_DATA_DIR", previousAppDataOverride);
            else
                qunsetenv("DEEPLUX_APP_DATA_DIR");
        }};
        const auto installPlugin = [&appData](const QString& name, const char* libraryPath, const char* metadataPath) {
            const QString pluginDirectory = QDir(appData.path()).filePath(QStringLiteral("plugins/") + name);
            if (!QDir().mkpath(pluginDirectory))
                return false;
            return QFile::copy(QString::fromLocal8Bit(metadataPath),
                               QDir(pluginDirectory).filePath(QStringLiteral("metadata.json"))) &&
                   QFile::copy(
                       QString::fromLocal8Bit(libraryPath),
                       QDir(pluginDirectory).filePath(QFileInfo(QString::fromLocal8Bit(libraryPath)).fileName()));
        };
        QVERIFY(installPlugin(QStringLiteral("GrabImage"), AGENT_SAM_G5_GRABIMAGE_LIBRARY,
                              AGENT_SAM_G5_GRABIMAGE_METADATA));
        QVERIFY(installPlugin(QStringLiteral("SamPredict"), AGENT_SAM_G5_SAMPREDICT_LIBRARY,
                              AGENT_SAM_G5_SAMPREDICT_METADATA));

        PluginManager& plugins = PluginManager::instance();
        QVERIFY(plugins.initialize());
        ScopeExit shutdownPlugins{[&plugins]() {
            if (plugins.isInitialized())
                plugins.shutdown();
        }};
        QVERIFY(plugins.loadPlugin(QStringLiteral("GrabImage")));
        QVERIFY(plugins.loadPlugin(QStringLiteral("SamPredict")));
        QCOMPARE(plugins.pluginInfo(QStringLiteral("SamPredict")).id, QStringLiteral("com.deeplux.plugin.sampredict"));

        ProjectManager& projects = ProjectManager::instance();
        projects.closeProject();
        Project* project = projects.newProject();
        QVERIFY(project);
        const QString grabId = QStringLiteral("sam-flow-grab");
        const QString predictId = QStringLiteral("sam-flow-predict");
        ModuleInstance grab;
        grab.id = grabId;
        grab.moduleId = QStringLiteral("GrabImage");
        grab.name = QStringLiteral("SAM flow fixture image");
        grab.params = QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Path")},
                                  {QStringLiteral("filePath"), m_imagePath}};
        project->addModule(grab);
        ModuleInstance predict;
        predict.id = predictId;
        predict.moduleId = QStringLiteral("SamPredict");
        predict.name = QStringLiteral("SAM flow prediction");
        predict.params = QJsonObject{{QStringLiteral("serverUrl"), QStringLiteral("http://127.0.0.1:8990")},
                                     {QStringLiteral("label"), QStringLiteral("fixture")},
                                     {QStringLiteral("points"), QJsonArray{QJsonArray{32, 24}}},
                                     {QStringLiteral("timeoutMs"), 5000}};
        project->addModule(predict);
        ModuleConnection imageEdge;
        imageEdge.fromModuleId = grabId;
        imageEdge.toModuleId = predictId;
        imageEdge.fromPort = QStringLiteral("image");
        imageEdge.toPort = QStringLiteral("image");
        imageEdge.edgeType = QStringLiteral("data");
        project->addConnection(imageEdge);

        SamTestServer peerA;
        SamTestServer peerB;
        QVERIFY(peerA.listen());
        QVERIFY(peerB.listen());
        const QString originA = QStringLiteral("http://127.0.0.1:%1").arg(QUrl(peerA.url()).port());
        const QString originB = QStringLiteral("http://127.0.0.1:%1").arg(QUrl(peerB.url()).port());
        AgentController& controller = AgentController::instance();
        QVERIFY(controller.initialize());
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        qRegisterMetaType<AgentActionLogEntry>("AgentActionLogEntry");
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        QSignalSpy runFinished(&RunEngine::instance(), &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy moduleStarted(&RunEngine::instance(), &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&RunEngine::instance(), &RunEngine::moduleFinished);
        ScopeExit cleanupRuntime{[&appData, &projects, &controller]() {
            RunEngine& engine = RunEngine::instance();
            engine.stop();
            QElapsedTimer cleanupWait;
            cleanupWait.start();
            while (engine.isBusy() && cleanupWait.elapsed() < 5000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                QTest::qWait(5);
            }
            if (engine.isBusy()) {
                appData.setAutoRemove(false);
                qFatal("Slice90 RunEngine cleanup exceeded 5000 ms; preserving isolated plugin directory");
            }
            engine.clearModules();
            projects.closeProject();
            controller.clearConversation();
            controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        }};
        RunEngine::instance().stop();
        RunEngine::instance().clearModules();

        const auto requestFlow = [&controller]() {
            return controller.handleToolCall(QStringLiteral("run_flow"),
                                             {{QStringLiteral("mode"), QStringLiteral("once")}});
        };
        const auto assertSafeConfirmation = [this, &controller, &predictId](const QString& expectedOrigin) {
            const QJsonObject context =
                controller.pendingConfirmationPayload().value(QStringLiteral("context")).toObject();
            const QString serialized = QString::fromUtf8(QJsonDocument(context).toJson(QJsonDocument::Compact));
            const QJsonArray effects = context.value(QStringLiteral("sideEffects")).toArray();
            const QString expectedEffect =
                QStringLiteral("sam_predict_plugin:%1@%2 upstream_image=sent").arg(predictId, expectedOrigin);
            QVERIFY2(effects.contains(expectedEffect), qPrintable(serialized));
            QVERIFY(!serialized.contains(QStringLiteral("sam-user")));
            QVERIFY(!serialized.contains(QStringLiteral("sam-secret")));
            QVERIFY(!serialized.contains(QStringLiteral("private/model-prefix")));
            QVERIFY(!serialized.contains(QStringLiteral("token=private-query")));
            QVERIFY(!serialized.contains(QStringLiteral("private-fragment")));
            QVERIFY(serialized.contains(expectedImageEffect(m_imagePath)));
            QVERIFY(!effects.contains(QStringLiteral("file_write:%1").arg(m_imagePath)));
            QVERIFY(context.value(QStringLiteral("summary")).toString().contains(QStringLiteral("run_flow")));
        };

        QUrl decoratedA(peerA.url());
        decoratedA.setUserName(QStringLiteral("sam-user"));
        decoratedA.setPassword(QStringLiteral("sam-secret"));
        decoratedA.setPath(QStringLiteral("/private/model-prefix"));
        decoratedA.setQuery(QStringLiteral("token=private-query"));
        decoratedA.setFragment(QStringLiteral("private-fragment"));
        QVERIFY(project->setModuleParam(predictId, QStringLiteral("serverUrl"), decoratedA.toString()));
        QJsonObject pending = requestFlow();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(!pending.contains(QStringLiteral("runId")));
        assertSafeConfirmation(originA);
        QCOMPARE(peerA.requestLog.size(), 0);
        QCOMPARE(peerB.requestLog.size(), 0);
        QCOMPARE(moduleStarted.count(), 0);
        QVERIFY(!RunEngine::instance().isBusy());
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(peerA.requestLog.size(), 0);
        QCOMPARE(peerB.requestLog.size(), 0);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));

        QVERIFY(project->setModuleParam(predictId, QStringLiteral("serverUrl"), peerA.url()));
        pending = requestFlow();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(!pending.contains(QStringLiteral("runId")));
        assertSafeConfirmation(originA);
        const QString staleConfirmationId = controller.pendingConfirmationId();
        QVERIFY(!staleConfirmationId.isEmpty());
        QVERIFY(project->setModuleParam(predictId, QStringLiteral("serverUrl"), peerB.url()));
        QVERIFY(controller.confirmPendingTools(staleConfirmationId));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QCOMPARE(peerA.requestLog.size(), 0);
        QCOMPARE(peerB.requestLog.size(), 0);
        QCOMPARE(moduleStarted.count(), 0);
        QVERIFY(!RunEngine::instance().isBusy());

        pending = requestFlow();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        assertSafeConfirmation(originB);
        const QString approvedConfirmationId = controller.pendingConfirmationId();
        QVERIFY(!approvedConfirmationId.isEmpty());
        QVERIFY(approvedConfirmationId != staleConfirmationId);
        QVERIFY(controller.confirmPendingTools(approvedConfirmationId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        const QJsonObject started = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(started.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        const QString runId = started.value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 10000);
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QCOMPARE(moduleStarted.count(), 2); // GrabImage 与 SamPredict 各启动一次
        QVERIFY(peerA.requestLog.isEmpty());
        QCOMPARE(peerB.requestLog.count(QStringLiteral("POST /set_image")), 1);
        QCOMPARE(peerB.requestLog.count(QStringLiteral("POST /predict")), 1);
        const QJsonObject runStatus =
            controller.handleToolCall(QStringLiteral("get_run_results"), {{QStringLiteral("runId"), runId}});
        QCOMPARE(runStatus.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        QCOMPARE(runStatus.value(QStringLiteral("runId")).toString(), runId);

        const int requestCountBeforeOutputReadback = peerB.requestLog.size();
        const QStringList fixtureFilesBeforeOutputReadback =
            QDir(m_dir.path()).entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
        const QJsonObject regionOutput = controller.handleToolCall(QStringLiteral("get_module_output"),
                                                                   {{QStringLiteral("runId"), runId},
                                                                    {QStringLiteral("instanceId"), predictId},
                                                                    {QStringLiteral("key"), QStringLiteral("region")},
                                                                    {QStringLiteral("preview"), true}});
        QCOMPARE(regionOutput.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(regionOutput.value(QStringLiteral("runId")).toString(), runId);
        const QJsonObject region = regionOutput.value(QStringLiteral("value")).toObject();
        QCOMPARE(region.value(QStringLiteral("label")).toString(), QStringLiteral("fixture"));
        QCOMPARE(region.value(QStringLiteral("bbox")).toArray(), (QJsonArray{10, 10, 10, 10}));
        QCOMPARE(region.value(QStringLiteral("polygonPointCount")).toInt(), 4);
        QCOMPARE(region.value(QStringLiteral("score")).toDouble(), 0.97);
        QCOMPARE(region.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
        QVERIFY(!region.contains(QStringLiteral("maskRle")));
        const QJsonArray expectedPolygon{QJsonArray{10, 10}, QJsonArray{20, 10}, QJsonArray{20, 20},
                                         QJsonArray{10, 20}};
        QCOMPARE(regionOutput.value(QStringLiteral("preview")).toObject().value(QStringLiteral("polygon")).toArray(),
                 expectedPolygon);
        const QJsonObject scoreOutput = controller.handleToolCall(QStringLiteral("get_module_output"),
                                                                  {{QStringLiteral("runId"), runId},
                                                                   {QStringLiteral("instanceId"), predictId},
                                                                   {QStringLiteral("key"), QStringLiteral("score")}});
        QCOMPARE(scoreOutput.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(scoreOutput.value(QStringLiteral("runId")).toString(), runId);
        QCOMPARE(scoreOutput.value(QStringLiteral("value")).toDouble(), 0.97);
        QCOMPARE(peerB.requestLog.size(), requestCountBeforeOutputReadback);
        QCOMPARE(project->moduleById(predictId)->params.value(QStringLiteral("serverUrl")).toString(), peerB.url());
        QCOMPARE(QDir(m_dir.path()).entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name),
                 fixtureFilesBeforeOutputReadback);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));

        // The plugin path must preserve upload-after-timeout uncertainty and never replay it.
        QVERIFY(project->setModuleParam(predictId, QStringLiteral("timeoutMs"), 300));
        peerB.withholdResponsePaths.insert(QStringLiteral("/predict"));
        const int requestCountBeforeTimeout = peerB.requestLog.size();
        const int moduleStartsBeforeTimeout = moduleStarted.count();
        pending = requestFlow();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(peerB.requestLog.size(), requestCountBeforeTimeout);
        QCOMPARE(moduleStarted.count(), moduleStartsBeforeTimeout);
        QCOMPARE(runFinished.count(), 1);

        pending = requestFlow();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        assertSafeConfirmation(originB);
        const int toolEventsBeforeTimeoutApproval = toolExecuted.count();
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), toolEventsBeforeTimeoutApproval + 1, 5000);
        const QJsonObject timeoutStarted = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(timeoutStarted.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        const QString timeoutRunId = timeoutStarted.value(QStringLiteral("runId")).toString();
        QVERIFY(!timeoutRunId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(peerB.requestLog.count(QStringLiteral("POST /predict")), 2, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 2, 10000);
        const RunResult timeoutResult = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(timeoutResult.runId, timeoutRunId);
        QVERIFY(!timeoutResult.success);
        QCOMPARE(timeoutResult.failedModule, predictId);
        QCOMPARE(timeoutResult.errorCode, ExecError::SideEffectUncertain);
        QVERIFY(timeoutResult.diagnostics.contains(QStringLiteral("uncertainStage=predict")));
        QCOMPARE(peerB.requestLog.count(QStringLiteral("POST /set_image")), 2);
        QCOMPARE(peerB.requestLog.count(QStringLiteral("POST /predict")), 2);
        QCOMPARE(moduleStarted.count(), moduleStartsBeforeTimeout + 2);
        QCOMPARE(moduleFinished.count(), moduleStarted.count());
        QCOMPARE(moduleFinished.last().at(0).toString(), predictId);
        QVERIFY(!moduleFinished.last().at(1).toBool());

        const QJsonObject uncertainStatus =
            controller.handleToolCall(QStringLiteral("get_run_results"), {{QStringLiteral("runId"), timeoutRunId}});
        QCOMPARE(uncertainStatus.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(uncertainStatus.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
        QVERIFY(!uncertainStatus.value(QStringLiteral("retryable")).toBool());
        QVERIFY(uncertainStatus.value(QStringLiteral("lastError"))
                    .toObject()
                    .value(QStringLiteral("diagnostics"))
                    .toString()
                    .contains(QStringLiteral("uncertainStage=predict")));
        const QJsonObject unavailableRegion = controller.handleToolCall(
            QStringLiteral("get_module_output"), {{QStringLiteral("runId"), timeoutRunId},
                                                  {QStringLiteral("instanceId"), predictId},
                                                  {QStringLiteral("key"), QStringLiteral("region")}});
        QCOMPARE(unavailableRegion.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(unavailableRegion.value(QStringLiteral("runId")).toString(), timeoutRunId);
        QVERIFY(!unavailableRegion.value(QStringLiteral("retryable")).toBool());
        const int requestsAfterTimeout = peerB.requestLog.size();
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(
                controller.handleToolCall(QStringLiteral("get_run_results"), {{QStringLiteral("runId"), timeoutRunId}})
                    .value(QStringLiteral("status"))
                    .toString(),
                QStringLiteral("uncertain"));
            QCOMPARE(controller
                         .handleToolCall(QStringLiteral("get_module_output"),
                                         {{QStringLiteral("runId"), timeoutRunId},
                                          {QStringLiteral("instanceId"), predictId},
                                          {QStringLiteral("key"), QStringLiteral("region")}})
                         .value(QStringLiteral("status"))
                         .toString(),
                     QStringLiteral("uncertain"));
        }
        QTest::qWait(350);
        QCOMPARE(peerB.requestLog.size(), requestsAfterTimeout);
        QCOMPARE(moduleStarted.count(), moduleStartsBeforeTimeout + 2);
        QCOMPARE(runFinished.count(), 2);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));

        // Cancellation after the fake peer has parsed the complete POST /predict body
        // leaves the remote inference outcome uncertain and must not be reported retryable.
        QVERIFY(project->setModuleParam(predictId, QStringLiteral("timeoutMs"), 120000));
        const int requestCountBeforeCancel = peerB.requestLog.size();
        const int moduleStartsBeforeCancel = moduleStarted.count();
        pending = requestFlow();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(peerB.requestLog.size(), requestCountBeforeCancel);
        QCOMPARE(moduleStarted.count(), moduleStartsBeforeCancel);
        QCOMPARE(runFinished.count(), 2);

        pending = requestFlow();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        assertSafeConfirmation(originB);
        const int toolEventsBeforeCancelApproval = toolExecuted.count();
        const int runFinishesBeforeCancel = runFinished.count();
        const int setImageRequestsBeforeCancel = peerB.requestLog.count(QStringLiteral("POST /set_image"));
        const int predictRequestsBeforeCancel = peerB.requestLog.count(QStringLiteral("POST /predict"));
        bool cancellationRequested = false;
        QTimer cancelAfterCompleteUpload;
        cancelAfterCompleteUpload.setInterval(1);
        connect(&cancelAfterCompleteUpload, &QTimer::timeout, &cancelAfterCompleteUpload, [&]() {
            if (!cancellationRequested &&
                peerB.requestLog.count(QStringLiteral("POST /predict")) == predictRequestsBeforeCancel + 1) {
                cancellationRequested = true;
                cancelAfterCompleteUpload.stop();
                engine.stop();
            }
        });
        cancelAfterCompleteUpload.start();
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_VERIFY_WITH_TIMEOUT(cancellationRequested, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), toolEventsBeforeCancelApproval + 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), runFinishesBeforeCancel + 1, 5000);
        cancelAfterCompleteUpload.stop();

        const QJsonObject cancelStarted = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(cancelStarted.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        const QString cancelRunId = cancelStarted.value(QStringLiteral("runId")).toString();
        QVERIFY(!cancelRunId.isEmpty());
        const RunResult cancelledRun = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(cancelledRun.runId, cancelRunId);
        QVERIFY(!cancelledRun.success);
        QCOMPARE(cancelledRun.failedModule, predictId);
        QCOMPARE(cancelledRun.errorCode, ExecError::SideEffectUncertain);
        QVERIFY(cancelledRun.diagnostics.contains(QStringLiteral("uncertainStage=predict")));
        QCOMPARE(peerB.requestLog.count(QStringLiteral("POST /set_image")), setImageRequestsBeforeCancel + 1);
        QCOMPARE(peerB.requestLog.count(QStringLiteral("POST /predict")), predictRequestsBeforeCancel + 1);
        QVERIFY(!peerB.lastPredictBody.isEmpty());
        QCOMPARE(moduleStarted.count(), moduleStartsBeforeCancel + 2);
        QCOMPARE(moduleFinished.count(), moduleStarted.count());

        const QJsonObject cancelledStatus =
            controller.handleToolCall(QStringLiteral("get_run_results"), {{QStringLiteral("runId"), cancelRunId}});
        QCOMPARE(cancelledStatus.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(cancelledStatus.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
        QVERIFY(!cancelledStatus.value(QStringLiteral("retryable")).toBool());
        QVERIFY(cancelledStatus.value(QStringLiteral("lastError"))
                    .toObject()
                    .value(QStringLiteral("diagnostics"))
                    .toString()
                    .contains(QStringLiteral("uncertainStage=predict")));
        const QJsonObject cancelledRegion = controller.handleToolCall(
            QStringLiteral("get_module_output"), {{QStringLiteral("runId"), cancelRunId},
                                                  {QStringLiteral("instanceId"), predictId},
                                                  {QStringLiteral("key"), QStringLiteral("region")}});
        QCOMPARE(cancelledRegion.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        const int requestsAfterCancel = peerB.requestLog.size();
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(
                controller.handleToolCall(QStringLiteral("get_run_results"), {{QStringLiteral("runId"), cancelRunId}})
                    .value(QStringLiteral("status"))
                    .toString(),
                QStringLiteral("uncertain"));
            QCOMPARE(controller
                         .handleToolCall(QStringLiteral("get_module_output"),
                                         {{QStringLiteral("runId"), cancelRunId},
                                          {QStringLiteral("instanceId"), predictId},
                                          {QStringLiteral("key"), QStringLiteral("region")}})
                         .value(QStringLiteral("status"))
                         .toString(),
                     QStringLiteral("uncertain"));
            QCOMPARE(peerB.requestLog.size(), requestsAfterCancel);
            QCOMPARE(moduleStarted.count(), moduleStartsBeforeCancel + 2);
        }
        QTest::qWait(350);
        QCOMPARE(peerB.requestLog.size(), requestsAfterCancel);
        QCOMPARE(moduleStarted.count(), moduleStartsBeforeCancel + 2);
        QCOMPARE(runFinished.count(), runFinishesBeforeCancel + 1);
    }

    void samPredictPluginStepOnceAndResumeConfirmInputAndNetworkTargets() {
        QFile sourceImage(m_imagePath);
        QVERIFY(sourceImage.open(QIODevice::ReadOnly));
        const QByteArray originalImageBytes = sourceImage.readAll();
        QVERIFY(!originalImageBytes.isEmpty());
        ScopeExit restoreSourceImage{[this, originalImageBytes]() {
            QFile restoreFile(m_imagePath);
            if (restoreFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
                restoreFile.write(originalImageBytes);
        }};
        const auto saveSolidImage = [this](const QColor& color) {
            QImage image(64, 48, QImage::Format_RGB32);
            image.fill(color);
            return image.save(m_imagePath, "PNG");
        };
        QVERIFY(saveSolidImage(Qt::gray));

        QTemporaryDir appData;
        QVERIFY(appData.isValid());
        const bool hadAppDataOverride = qEnvironmentVariableIsSet("DEEPLUX_APP_DATA_DIR");
        const QByteArray previousAppDataOverride = qgetenv("DEEPLUX_APP_DATA_DIR");
        qputenv("DEEPLUX_APP_DATA_DIR", appData.path().toLocal8Bit());
        ScopeExit restoreAppData{[hadAppDataOverride, previousAppDataOverride]() {
            if (hadAppDataOverride)
                qputenv("DEEPLUX_APP_DATA_DIR", previousAppDataOverride);
            else
                qunsetenv("DEEPLUX_APP_DATA_DIR");
        }};
        const auto installPlugin = [&appData](const QString& name, const char* libraryPath, const char* metadataPath) {
            const QString pluginDirectory = QDir(appData.path()).filePath(QStringLiteral("plugins/") + name);
            if (!QDir().mkpath(pluginDirectory))
                return false;
            return QFile::copy(QString::fromLocal8Bit(metadataPath),
                               QDir(pluginDirectory).filePath(QStringLiteral("metadata.json"))) &&
                   QFile::copy(
                       QString::fromLocal8Bit(libraryPath),
                       QDir(pluginDirectory).filePath(QFileInfo(QString::fromLocal8Bit(libraryPath)).fileName()));
        };
        QVERIFY(installPlugin(QStringLiteral("GrabImage"), AGENT_SAM_G5_GRABIMAGE_LIBRARY,
                              AGENT_SAM_G5_GRABIMAGE_METADATA));
        QVERIFY(installPlugin(QStringLiteral("CropImage"), AGENT_SAM_G5_CROPIMAGE_LIBRARY,
                              AGENT_SAM_G5_CROPIMAGE_METADATA));
        QVERIFY(installPlugin(QStringLiteral("Delay"), AGENT_SAM_G5_DELAY_LIBRARY, AGENT_SAM_G5_DELAY_METADATA));
        QVERIFY(installPlugin(QStringLiteral("SamPredict"), AGENT_SAM_G5_SAMPREDICT_LIBRARY,
                              AGENT_SAM_G5_SAMPREDICT_METADATA));

        PluginManager& plugins = PluginManager::instance();
        QVERIFY(plugins.initialize());
        ScopeExit shutdownPlugins{[&plugins]() {
            if (plugins.isInitialized())
                plugins.shutdown();
        }};
        QVERIFY(plugins.loadPlugin(QStringLiteral("GrabImage")));
        QVERIFY(plugins.loadPlugin(QStringLiteral("CropImage")));
        QVERIFY(plugins.loadPlugin(QStringLiteral("延时")));
        QVERIFY(plugins.loadPlugin(QStringLiteral("SamPredict")));

        ProjectManager& projects = ProjectManager::instance();
        RunEngine& engine = RunEngine::instance();
        AgentController& controller = AgentController::instance();
        SamTestServer peerA;
        SamTestServer peerB;
        QVERIFY(peerA.listen());
        QVERIFY(peerB.listen());
        QVERIFY(controller.initialize());
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy cycleStopped(&engine, &RunEngine::cycleStopped);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy moduleSkipped(&engine, &RunEngine::moduleSkipped);
        qRegisterMetaType<AgentActionLogEntry>("AgentActionLogEntry");

        projects.closeProject();
        engine.stop();
        engine.clearModules();
        Project* project = projects.newProject();
        QVERIFY(project);
        const QString grabId = QStringLiteral("slice78_sam_grab");
        const QString predictId = QStringLiteral("slice78_sam_predict");
        ModuleInstance grab;
        grab.id = grabId;
        grab.moduleId = QStringLiteral("GrabImage");
        grab.name = QStringLiteral("Slice 78 fixture image");
        grab.params = QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Path")},
                                  {QStringLiteral("filePath"), m_imagePath}};
        project->addModule(grab);
        ModuleInstance predict;
        predict.id = predictId;
        predict.moduleId = QStringLiteral("SamPredict");
        predict.name = QStringLiteral("Slice 78 SAM prediction");
        predict.params = QJsonObject{{QStringLiteral("serverUrl"), peerA.url()},
                                     {QStringLiteral("label"), QStringLiteral("slice78-fixture")},
                                     {QStringLiteral("points"), QJsonArray{QJsonArray{32, 24}}},
                                     {QStringLiteral("timeoutMs"), 5000}};
        project->addModule(predict);
        ModuleConnection imageEdge;
        imageEdge.fromModuleId = grabId;
        imageEdge.toModuleId = predictId;
        imageEdge.fromPort = QStringLiteral("image");
        imageEdge.toPort = QStringLiteral("image");
        imageEdge.edgeType = QStringLiteral("data");
        project->addConnection(imageEdge);

        ScopeExit cleanupRuntime{[&appData, &projects, &controller, &engine]() {
            engine.stop();
            QElapsedTimer wait;
            wait.start();
            while (engine.isBusy() && wait.elapsed() < 5000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                QTest::qWait(5);
            }
            if (engine.isBusy()) {
                appData.setAutoRemove(false);
                qFatal("Slice78 RunEngine cleanup exceeded 5000 ms; preserving isolated plugin directory");
            }
            engine.clearModules();
            projects.closeProject();
            controller.clearConversation();
            controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        }};

        const auto samEffects = [predictId](const QString& origin) {
            return QStringLiteral("sam_predict_plugin:%1@%2 upstream_image=sent").arg(predictId, origin);
        };
        const auto assertSamConfirmation = [this, &controller, &samEffects](const QString& origin,
                                                                            const QString& actionToken) {
            const QJsonObject payload = controller.pendingConfirmationPayload();
            const QJsonObject context = payload.value(QStringLiteral("context")).toObject();
            const QJsonArray effects = context.value(QStringLiteral("sideEffects")).toArray();
            const QByteArray serialized = QJsonDocument(payload).toJson(QJsonDocument::Compact);
            return effects.contains(samEffects(origin)) && effects.contains(expectedImageEffect(m_imagePath)) &&
                   context.value(QStringLiteral("summary")).toString().contains(actionToken) &&
                   !serialized.contains("token=");
        };

        QJsonObject pending = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(expectedImageEffect(m_imagePath)));
        const QString staleFirstStepConfirmation = controller.pendingConfirmationId();
        QVERIFY(!staleFirstStepConfirmation.isEmpty());
        const RunState engineStateBeforeStaleStep = engine.state();
        QVERIFY(saveSolidImage(Qt::red));
        QVERIFY(controller.confirmPendingTools(staleFirstStepConfirmation));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QCOMPARE(engine.state(), engineStateBeforeStaleStep);
        QVERIFY(!engine.isBusy());
        QCOMPARE(moduleStarted.count(), 0);
        QCOMPARE(moduleFinished.count(), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);
        QVERIFY(peerA.requestLog.isEmpty());
        QVERIFY(peerB.requestLog.isEmpty());

        pending = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(assertSamConfirmation(peerA.url(), QStringLiteral("step_once")));
        const QString firstStepConfirmation = controller.pendingConfirmationId();
        QVERIFY(!firstStepConfirmation.isEmpty());
        QVERIFY(controller.confirmPendingTools(firstStepConfirmation));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        const QJsonObject firstStep = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(firstStep.value(QStringLiteral("status")).toString(), QStringLiteral("stepped"));
        const QString stepRunId = firstStep.value(QStringLiteral("runId")).toString();
        QVERIFY(!stepRunId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QCOMPARE(qvariant_cast<RunResult>(runFinished.last().at(0)).runId, stepRunId);
        QCOMPARE(moduleStarted.count(), 1);
        QCOMPARE(moduleStarted.at(0).at(0).toString(), grabId);
        QCOMPARE(moduleFinished.count(), 1);
        QCOMPARE(moduleFinished.at(0).at(0).toString(), grabId);
        QCOMPARE(peerA.requestLog.size(), 0);
        QCOMPARE(peerB.requestLog.size(), 0);

        // GrabImage already ran. Confirmation must bind its cached red pixels even if
        // the configured source file changes before the SamPredict step.
        QVERIFY(saveSolidImage(Qt::blue));
        pending = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonArray cachedStepEffects = controller.pendingConfirmationPayload()
                                                 .value(QStringLiteral("context"))
                                                 .toObject()
                                                 .value(QStringLiteral("sideEffects"))
                                                 .toArray();
        QVERIFY(cachedStepEffects.contains(samEffects(peerA.url())));
        QVERIFY(cachedStepEffects.contains(expectedCachedImageEffect(m_imagePath, predictId, 64, 48, Qt::red)));
        QVERIFY(!cachedStepEffects.contains(expectedImageEffect(m_imagePath)));
        const QString rejectedStepConfirmation = controller.pendingConfirmationId();
        QVERIFY(!rejectedStepConfirmation.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedStepConfirmation));
        QCOMPARE(peerA.requestLog.size(), 0);
        QCOMPARE(peerB.requestLog.size(), 0);
        QCOMPARE(moduleStarted.count(), 1);
        QCOMPARE(moduleFinished.count(), 1);
        QCOMPARE(runFinished.count(), 1);
        QCOMPARE(toolExecuted.count(), 1);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);

        pending = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonArray approvedCachedStepEffects = controller.pendingConfirmationPayload()
                                                         .value(QStringLiteral("context"))
                                                         .toObject()
                                                         .value(QStringLiteral("sideEffects"))
                                                         .toArray();
        QVERIFY(approvedCachedStepEffects.contains(expectedCachedImageEffect(m_imagePath, predictId, 64, 48, Qt::red)));
        const QString stepApprovedId = controller.pendingConfirmationId();
        QVERIFY(!stepApprovedId.isEmpty());
        QVERIFY(stepApprovedId != rejectedStepConfirmation);
        QVERIFY(controller.confirmPendingTools(stepApprovedId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 2, 5000);
        const QJsonObject stepResult = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(stepResult.value(QStringLiteral("status")).toString(), QStringLiteral("stepped"));
        QCOMPARE(stepResult.value(QStringLiteral("runId")).toString(), stepRunId);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 2, 10000);
        QCOMPARE(qvariant_cast<RunResult>(runFinished.last().at(0)).runId, stepRunId);
        QVERIFY(qvariant_cast<RunResult>(runFinished.last().at(0)).success);
        QCOMPARE(moduleStarted.count(), 2);
        QCOMPARE(moduleStarted.at(1).at(0).toString(), predictId);
        QCOMPARE(moduleFinished.count(), 2);
        QCOMPARE(moduleFinished.at(1).at(0).toString(), predictId);
        QCOMPARE(peerA.requestLog.count(QStringLiteral("POST /set_image")), 1);
        QCOMPARE(peerA.requestLog.count(QStringLiteral("POST /predict")), 1);
        QVERIFY(peerB.requestLog.isEmpty());
        QVERIFY(peerA.lastSetImageReadable);
        QCOMPARE(peerA.lastSetImage.pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);

        const QJsonObject stepRegion = controller.handleToolCall(QStringLiteral("get_module_output"),
                                                                 {{QStringLiteral("runId"), stepRunId},
                                                                  {QStringLiteral("instanceId"), predictId},
                                                                  {QStringLiteral("key"), QStringLiteral("region")},
                                                                  {QStringLiteral("preview"), true}});
        QCOMPARE(stepRegion.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(stepRegion.value(QStringLiteral("runId")).toString(), stepRunId);
        const QJsonObject stepRegionValue = stepRegion.value(QStringLiteral("value")).toObject();
        QCOMPARE(stepRegionValue.value(QStringLiteral("label")).toString(), QStringLiteral("slice78-fixture"));
        QCOMPARE(stepRegionValue.value(QStringLiteral("score")).toDouble(), 0.97);
        QCOMPARE(stepRegionValue.value(QStringLiteral("polygonPointCount")).toInt(), 4);
        const QJsonArray expectedPolygon{QJsonArray{10, 10}, QJsonArray{20, 10}, QJsonArray{20, 20},
                                         QJsonArray{10, 20}};
        QCOMPARE(stepRegion.value(QStringLiteral("preview")).toObject().value(QStringLiteral("polygon")).toArray(),
                 expectedPolygon);
        const QJsonObject stepScore = controller.handleToolCall(QStringLiteral("get_module_output"),
                                                                {{QStringLiteral("runId"), stepRunId},
                                                                 {QStringLiteral("instanceId"), predictId},
                                                                 {QStringLiteral("key"), QStringLiteral("score")}});
        QCOMPARE(stepScore.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(stepScore.value(QStringLiteral("value")).toDouble(), 0.97);
        QCOMPARE(peerA.requestLog.size(), 2);
        const int toolExecutionsBeforeResume = toolExecuted.count();

        // Load a fresh paused session at the same source→SAM boundary.
        engine.clearModules();
        projects.closeProject();
        project = projects.newProject();
        QVERIFY(project);
        grab.id = grabId;
        grab.breakpoint = true;
        project->addModule(grab);
        predict.params.insert(QStringLiteral("serverUrl"), peerA.url());
        predict.breakpoint = false;
        project->addModule(predict);
        project->addConnection(imageEdge);
        QVERIFY(engine.loadProject(project));
        const int predictStartedBeforeResume = moduleSignalCount(moduleStarted, predictId);
        const int predictFinishedBeforeResume = moduleSignalCount(moduleFinished, predictId);
        engine.runOnce();
        QCOMPARE(engine.state(), RunState::Paused);
        const QString pausedRunId = engine.runObservation().runId;
        QVERIFY(!pausedRunId.isEmpty());
        QCOMPARE(moduleStarted.count(), 2); // breakpoint precedes GrabImage moduleStarted
        QCOMPARE(moduleSignalCount(moduleStarted, grabId), 1);
        QCOMPARE(moduleSignalCount(moduleStarted, predictId), predictStartedBeforeResume);
        QCOMPARE(moduleSignalCount(moduleFinished, predictId), predictFinishedBeforeResume);
        QCOMPARE(runFinished.count(), 2);
        peerA.requestLog.clear();
        peerB.requestLog.clear();

        pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString resumeOrigin = peerA.url();
        QVERIFY(assertSamConfirmation(resumeOrigin, QStringLiteral("resume")));
        const QString staleResumeId = controller.pendingConfirmationId();
        QVERIFY(!staleResumeId.isEmpty());
        QVERIFY(project->setModuleParam(predictId, QStringLiteral("serverUrl"), peerB.url()));
        QVERIFY(controller.confirmPendingTools(staleResumeId));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(engine.runObservation().runId, pausedRunId);
        QVERIFY(peerA.requestLog.isEmpty());
        QVERIFY(peerB.requestLog.isEmpty());
        QCOMPARE(moduleSignalCount(moduleStarted, predictId), predictStartedBeforeResume);
        QCOMPARE(moduleSignalCount(moduleFinished, predictId), predictFinishedBeforeResume);
        QCOMPARE(runFinished.count(), 2);
        QCOMPARE(toolExecuted.count(), toolExecutionsBeforeResume);
        QVERIFY(project->setModuleParam(predictId, QStringLiteral("serverUrl"), peerA.url()));

        pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(assertSamConfirmation(resumeOrigin, QStringLiteral("resume")));
        const QString staleResumeImageId = controller.pendingConfirmationId();
        QVERIFY(!staleResumeImageId.isEmpty());
        QVERIFY(saveSolidImage(Qt::green));
        QVERIFY(controller.confirmPendingTools(staleResumeImageId));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("resume_flow"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(engine.runObservation().runId, pausedRunId);
        QVERIFY(peerA.requestLog.isEmpty());
        QVERIFY(peerB.requestLog.isEmpty());
        QCOMPARE(moduleSignalCount(moduleStarted, grabId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, grabId), 1);
        QCOMPARE(moduleSignalCount(moduleStarted, predictId), predictStartedBeforeResume);
        QCOMPARE(moduleSignalCount(moduleFinished, predictId), predictFinishedBeforeResume);
        QCOMPARE(runFinished.count(), 2);
        QCOMPARE(toolExecuted.count(), toolExecutionsBeforeResume);

        pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(assertSamConfirmation(resumeOrigin, QStringLiteral("resume")));
        const QString rejectedResumeId = controller.pendingConfirmationId();
        QVERIFY(!rejectedResumeId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedResumeId));
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(engine.runObservation().runId, pausedRunId);
        QVERIFY(peerA.requestLog.isEmpty());
        QVERIFY(peerB.requestLog.isEmpty());
        QCOMPARE(moduleSignalCount(moduleStarted, predictId), predictStartedBeforeResume);
        QCOMPARE(moduleSignalCount(moduleFinished, predictId), predictFinishedBeforeResume);
        QCOMPARE(runFinished.count(), 2);
        QCOMPARE(toolExecuted.count(), toolExecutionsBeforeResume);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));

        pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(assertSamConfirmation(resumeOrigin, QStringLiteral("resume")));
        const QString approvedResumeId = controller.pendingConfirmationId();
        QVERIFY(!approvedResumeId.isEmpty());
        QVERIFY(controller.confirmPendingTools(approvedResumeId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), toolExecutionsBeforeResume + 1, 10000);
        QCOMPARE(toolExecuted.last().at(1).toJsonObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("resumed"));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 3, 10000);
        const RunResult resumeResult = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(resumeResult.runId, pausedRunId);
        QVERIFY2(resumeResult.success, qPrintable(resumeResult.errorMessage));
        QCOMPARE(engine.state(), RunState::Idle);
        QCOMPARE(engine.runObservation().runId, pausedRunId);
        QCOMPARE(moduleSignalCount(moduleStarted, predictId), predictStartedBeforeResume + 1);
        QCOMPARE(moduleSignalCount(moduleFinished, predictId), predictFinishedBeforeResume + 1);
        QCOMPARE(peerA.requestLog.count(QStringLiteral("POST /set_image")), 1);
        QCOMPARE(peerA.requestLog.count(QStringLiteral("POST /predict")), 1);
        QVERIFY(peerA.lastSetImageReadable);
        QCOMPARE(peerA.lastSetImage.size(), QSize(64, 48));
        QCOMPARE(peerA.lastSetImage.pixelColor(0, 0), QColor(Qt::green));
        QVERIFY(peerB.requestLog.isEmpty());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("resume_flow"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);
        const QJsonObject resumeRegion = controller.handleToolCall(QStringLiteral("get_module_output"),
                                                                   {{QStringLiteral("runId"), pausedRunId},
                                                                    {QStringLiteral("instanceId"), predictId},
                                                                    {QStringLiteral("key"), QStringLiteral("region")},
                                                                    {QStringLiteral("preview"), true}});
        QCOMPARE(resumeRegion.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(resumeRegion.value(QStringLiteral("runId")).toString(), pausedRunId);
        const QJsonObject resumeRegionValue = resumeRegion.value(QStringLiteral("value")).toObject();
        QCOMPARE(resumeRegionValue.value(QStringLiteral("label")).toString(), QStringLiteral("slice78-fixture"));
        QCOMPARE(resumeRegionValue.value(QStringLiteral("score")).toDouble(), 0.97);
        QCOMPARE(resumeRegionValue.value(QStringLiteral("polygonPointCount")).toInt(), 4);
        QCOMPARE(resumeRegion.value(QStringLiteral("preview")).toObject().value(QStringLiteral("polygon")).toArray(),
                 expectedPolygon);
        const QJsonObject resumeScore = controller.handleToolCall(QStringLiteral("get_module_output"),
                                                                  {{QStringLiteral("runId"), pausedRunId},
                                                                   {QStringLiteral("instanceId"), predictId},
                                                                   {QStringLiteral("key"), QStringLiteral("score")}});
        QCOMPARE(resumeScore.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(resumeScore.value(QStringLiteral("runId")).toString(), pausedRunId);
        QCOMPARE(resumeScore.value(QStringLiteral("value")).toDouble(), 0.97);
        QCOMPARE(peerA.requestLog.size(), 2);

        // Resume after GrabImage must bind the paused run's cached image. Changing the
        // file while the run is paused must not change the input SamPredict will consume.
        engine.clearModules();
        projects.closeProject();
        project = projects.newProject();
        QVERIFY(project);
        grab.breakpoint = false;
        predict.breakpoint = true;
        project->addModule(grab);
        project->addModule(predict);
        project->addConnection(imageEdge);
        QVERIFY(engine.loadProject(project));
        const int cachedResumePredictStarted = moduleSignalCount(moduleStarted, predictId);
        const int cachedResumePredictFinished = moduleSignalCount(moduleFinished, predictId);
        const int cachedResumeRunFinished = runFinished.count();
        engine.runOnce();
        QCOMPARE(engine.state(), RunState::Paused);
        const QString cachedResumeRunId = engine.runObservation().runId;
        QVERIFY(!cachedResumeRunId.isEmpty());
        QCOMPARE(moduleSignalCount(moduleStarted, predictId), cachedResumePredictStarted);
        QCOMPARE(moduleSignalCount(moduleFinished, predictId), cachedResumePredictFinished);
        peerA.requestLog.clear();
        peerB.requestLog.clear();
        QVERIFY(saveSolidImage(Qt::blue));

        pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonArray cachedResumeEffects = controller.pendingConfirmationPayload()
                                                   .value(QStringLiteral("context"))
                                                   .toObject()
                                                   .value(QStringLiteral("sideEffects"))
                                                   .toArray();
        QVERIFY(cachedResumeEffects.contains(samEffects(peerA.url())));
        QVERIFY(cachedResumeEffects.contains(expectedCachedImageEffect(m_imagePath, predictId, 64, 48, Qt::green)));
        QVERIFY(!cachedResumeEffects.contains(expectedImageEffect(m_imagePath)));
        const QString cachedResumeConfirmation = controller.pendingConfirmationId();
        QVERIFY(!cachedResumeConfirmation.isEmpty());
        const int toolExecutionsBeforeCachedResume = toolExecuted.count();
        QVERIFY(controller.confirmPendingTools(cachedResumeConfirmation));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), toolExecutionsBeforeCachedResume + 1, 10000);
        QCOMPARE(toolExecuted.last().at(1).toJsonObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("resumed"));
        QCOMPARE(engine.runObservation().runId, cachedResumeRunId);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), cachedResumeRunFinished + 1, 10000);
        QCOMPARE(qvariant_cast<RunResult>(runFinished.last().at(0)).runId, cachedResumeRunId);
        QVERIFY(qvariant_cast<RunResult>(runFinished.last().at(0)).success);
        QCOMPARE(moduleSignalCount(moduleStarted, predictId), cachedResumePredictStarted + 1);
        QCOMPARE(moduleSignalCount(moduleFinished, predictId), cachedResumePredictFinished + 1);
        QCOMPARE(peerA.requestLog.count(QStringLiteral("POST /set_image")), 1);
        QCOMPARE(peerA.requestLog.count(QStringLiteral("POST /predict")), 1);
        QVERIFY(peerA.lastSetImageReadable);
        QCOMPARE(peerA.lastSetImage.pixelColor(0, 0), QColor(Qt::green));
        QVERIFY(peerB.requestLog.isEmpty());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));

        // An intermediate image transform must be reflected in confirmation. The
        // consumed SAM input is the CropImage output, not GrabImage's original frame.
        peerA.requestLog.clear();
        peerB.requestLog.clear();
        engine.clearModules();
        projects.closeProject();
        project = projects.newProject();
        QVERIFY(project);
        QVERIFY(saveSolidImage(Qt::red));
        grab.breakpoint = false;
        predict.breakpoint = false;
        ModuleInstance crop;
        crop.id = QStringLiteral("slice109_sam_crop");
        crop.moduleId = QStringLiteral("CropImage");
        crop.name = QStringLiteral("Slice 109 crop");
        crop.params = QJsonObject{{QStringLiteral("rectangles"), QJsonArray{QJsonArray{32, 24, 10, 10, 0}}},
                                  {QStringLiteral("outputFirstAsImage"), true}};
        project->addModule(grab);
        project->addModule(crop);
        project->addModule(predict);
        ModuleConnection cropInputEdge;
        cropInputEdge.fromModuleId = grabId;
        cropInputEdge.toModuleId = crop.id;
        cropInputEdge.fromPort = QStringLiteral("image");
        cropInputEdge.toPort = QStringLiteral("image");
        cropInputEdge.edgeType = QStringLiteral("data");
        project->addConnection(cropInputEdge);
        ModuleConnection cropOutputEdge;
        cropOutputEdge.fromModuleId = crop.id;
        cropOutputEdge.toModuleId = predictId;
        cropOutputEdge.fromPort = QStringLiteral("image");
        cropOutputEdge.toPort = QStringLiteral("image");
        cropOutputEdge.edgeType = QStringLiteral("data");
        project->addConnection(cropOutputEdge);
        QVERIFY(engine.loadProject(project));

        int multiStepToolBaseline = toolExecuted.count();
        int multiStepRunBaseline = runFinished.count();
        pending = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(expectedImageEffect(m_imagePath)));
        QString multiStepConfirmation = controller.pendingConfirmationId();
        QVERIFY(!multiStepConfirmation.isEmpty());
        QVERIFY(controller.confirmPendingTools(multiStepConfirmation));
        ++multiStepToolBaseline;
        ++multiStepRunBaseline;
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), multiStepToolBaseline, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), multiStepRunBaseline, 10000);
        QVERIFY(qvariant_cast<RunResult>(runFinished.last().at(0)).success);

        pending = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonArray transformPendingEffects = controller.pendingConfirmationPayload()
                                                       .value(QStringLiteral("context"))
                                                       .toObject()
                                                       .value(QStringLiteral("sideEffects"))
                                                       .toArray();
        QVERIFY(transformPendingEffects.contains(QStringLiteral("sam_input_image:%1 "
                                                                "source=pipeline_input_pending:%2 "
                                                                "image_snapshot=not_materialized")
                                                     .arg(m_imagePath, predictId)));
        multiStepConfirmation = controller.pendingConfirmationId();
        QVERIFY(!multiStepConfirmation.isEmpty());
        QVERIFY(controller.confirmPendingTools(multiStepConfirmation));
        ++multiStepToolBaseline;
        ++multiStepRunBaseline;
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), multiStepToolBaseline, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), multiStepRunBaseline, 10000);
        QVERIFY(qvariant_cast<RunResult>(runFinished.last().at(0)).success);
        QVERIFY(peerA.requestLog.isEmpty());
        QVERIFY(peerB.requestLog.isEmpty());

        QVERIFY(saveSolidImage(Qt::blue));
        pending = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonArray transformedStepEffects = controller.pendingConfirmationPayload()
                                                      .value(QStringLiteral("context"))
                                                      .toObject()
                                                      .value(QStringLiteral("sideEffects"))
                                                      .toArray();
        QVERIFY(transformedStepEffects.contains(expectedCachedImageEffect(m_imagePath, predictId, 20, 20, Qt::red)));
        QVERIFY(!transformedStepEffects.contains(expectedImageEffect(m_imagePath)));
        multiStepConfirmation = controller.pendingConfirmationId();
        QVERIFY(!multiStepConfirmation.isEmpty());
        QVERIFY(controller.confirmPendingTools(multiStepConfirmation));
        ++multiStepToolBaseline;
        ++multiStepRunBaseline;
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), multiStepToolBaseline, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), multiStepRunBaseline, 10000);
        QVERIFY(qvariant_cast<RunResult>(runFinished.last().at(0)).success);
        QCOMPARE(peerA.requestLog.count(QStringLiteral("POST /set_image")), 1);
        QCOMPARE(peerA.requestLog.count(QStringLiteral("POST /predict")), 1);
        QVERIFY(peerA.lastSetImageReadable);
        QCOMPARE(peerA.lastSetImage.size(), QSize(20, 20));
        QCOMPARE(peerA.lastSetImage.pixelColor(0, 0), QColor(Qt::red));
        QVERIFY(peerB.requestLog.isEmpty());

        // The same binding is required for a paused resume after the transform has run.
        engine.clearModules();
        projects.closeProject();
        project = projects.newProject();
        QVERIFY(project);
        QVERIFY(saveSolidImage(Qt::green));
        grab.breakpoint = false;
        crop.breakpoint = false;
        predict.breakpoint = true;
        project->addModule(grab);
        project->addModule(crop);
        project->addModule(predict);
        project->addConnection(cropInputEdge);
        project->addConnection(cropOutputEdge);
        QVERIFY(engine.loadProject(project));
        const int transformedPredictStarted = moduleSignalCount(moduleStarted, predictId);
        const int transformedPredictFinished = moduleSignalCount(moduleFinished, predictId);
        const int transformedRunFinished = runFinished.count();
        engine.runOnce();
        QCOMPARE(engine.state(), RunState::Paused);
        const QString transformedRunId = engine.runObservation().runId;
        QVERIFY(!transformedRunId.isEmpty());
        QCOMPARE(moduleSignalCount(moduleStarted, predictId), transformedPredictStarted);
        QCOMPARE(moduleSignalCount(moduleFinished, predictId), transformedPredictFinished);
        QVERIFY(saveSolidImage(Qt::yellow));
        peerA.requestLog.clear();
        pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonArray transformedResumeEffects = controller.pendingConfirmationPayload()
                                                        .value(QStringLiteral("context"))
                                                        .toObject()
                                                        .value(QStringLiteral("sideEffects"))
                                                        .toArray();
        QVERIFY(
            transformedResumeEffects.contains(expectedCachedImageEffect(m_imagePath, predictId, 20, 20, Qt::green)));
        QVERIFY(!transformedResumeEffects.contains(expectedImageEffect(m_imagePath)));
        const QString transformedResumeConfirmation = controller.pendingConfirmationId();
        QVERIFY(!transformedResumeConfirmation.isEmpty());
        QVERIFY(controller.confirmPendingTools(transformedResumeConfirmation));
        ++multiStepToolBaseline;
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), multiStepToolBaseline, 10000);
        QCOMPARE(engine.runObservation().runId, transformedRunId);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), transformedRunFinished + 1, 10000);
        QCOMPARE(qvariant_cast<RunResult>(runFinished.last().at(0)).runId, transformedRunId);
        QVERIFY(qvariant_cast<RunResult>(runFinished.last().at(0)).success);
        QCOMPARE(moduleSignalCount(moduleStarted, predictId), transformedPredictStarted + 1);
        QCOMPARE(moduleSignalCount(moduleFinished, predictId), transformedPredictFinished + 1);
        QCOMPARE(peerA.requestLog.count(QStringLiteral("POST /set_image")), 1);
        QCOMPARE(peerA.requestLog.count(QStringLiteral("POST /predict")), 1);
        QVERIFY(peerA.lastSetImageReadable);
        QCOMPARE(peerA.lastSetImage.size(), QSize(20, 20));
        QCOMPARE(peerA.lastSetImage.pixelColor(0, 0), QColor(Qt::green));
        const QJsonObject transformedOutput = controller.handleToolCall(
            QStringLiteral("get_module_output"), {{QStringLiteral("runId"), transformedRunId},
                                                  {QStringLiteral("instanceId"), predictId},
                                                  {QStringLiteral("key"), QStringLiteral("score")}});
        QCOMPARE(transformedOutput.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(transformedOutput.value(QStringLiteral("runId")).toString(), transformedRunId);
        QCOMPARE(transformedOutput.value(QStringLiteral("value")).toDouble(), 0.97);

        // Disabled intermediate modules transparently bypass the image in RunEngine;
        // their image edges must therefore remain in confirmation-source discovery.
        engine.clearModules();
        projects.closeProject();
        project = projects.newProject();
        QVERIFY(project);
        QVERIFY(saveSolidImage(Qt::cyan));
        grab.breakpoint = false;
        crop.enabled = false;
        crop.breakpoint = false;
        predict.breakpoint = false;
        project->addModule(grab);
        project->addModule(crop);
        project->addModule(predict);
        project->addConnection(cropInputEdge);
        project->addConnection(cropOutputEdge);
        QVERIFY(engine.loadProject(project));
        peerA.requestLog.clear();
        peerB.requestLog.clear();
        const int disabledStepToolBaseline = toolExecuted.count();
        const int disabledRunBaseline = runFinished.count();
        const int disabledGrabStarted = moduleSignalCount(moduleStarted, grabId);
        const int disabledPredictStarted = moduleSignalCount(moduleStarted, predictId);
        const int disabledCropSkipped = moduleSignalCount(moduleSkipped, crop.id);
        pending = controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), "once"}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonArray disabledBypassEffects = controller.pendingConfirmationPayload()
                                                     .value(QStringLiteral("context"))
                                                     .toObject()
                                                     .value(QStringLiteral("sideEffects"))
                                                     .toArray();
        QVERIFY(disabledBypassEffects.contains(expectedImageEffect(m_imagePath)));
        const QString disabledBypassConfirmation = controller.pendingConfirmationId();
        QVERIFY(!disabledBypassConfirmation.isEmpty());
        QVERIFY(controller.confirmPendingTools(disabledBypassConfirmation));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), disabledStepToolBaseline + 1, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), disabledRunBaseline + 1, 10000);
        const RunResult disabledBypassResult = qvariant_cast<RunResult>(runFinished.last().at(0));
        QVERIFY2(disabledBypassResult.success, qPrintable(disabledBypassResult.errorMessage));
        QCOMPARE(moduleSignalCount(moduleStarted, grabId), disabledGrabStarted + 1);
        QCOMPARE(moduleSignalCount(moduleStarted, predictId), disabledPredictStarted + 1);
        QCOMPARE(moduleSignalCount(moduleSkipped, crop.id), disabledCropSkipped + 1);
        QCOMPARE(peerA.requestLog.count(QStringLiteral("POST /set_image")), 1);
        QCOMPARE(peerA.requestLog.count(QStringLiteral("POST /predict")), 1);
        QVERIFY(peerA.lastSetImageReadable);
        QCOMPARE(peerA.lastSetImage.size(), QSize(64, 48));
        QCOMPARE(peerA.lastSetImage.pixelColor(0, 0), QColor(Qt::cyan));
        QVERIFY(peerB.requestLog.isEmpty());

        // Bounded Autopilot cycles bind the reachable GrabImage(Path) bytes before
        // approval. A same-path replacement expires the old batch without starting
        // any cycle; an approved fresh batch executes exactly two rounds.
        engine.clearModules();
        projects.closeProject();
        project = projects.newProject();
        QVERIFY(project);
        QVERIFY(saveSolidImage(Qt::gray));
        grab.breakpoint = false;
        crop.enabled = true;
        predict.breakpoint = false;
        predict.params.insert(QStringLiteral("serverUrl"), peerA.url());
        project->addModule(grab);
        project->addModule(predict);
        ModuleConnection cycleImageEdge;
        cycleImageEdge.fromModuleId = grabId;
        cycleImageEdge.toModuleId = predictId;
        cycleImageEdge.fromPort = QStringLiteral("image");
        cycleImageEdge.toPort = QStringLiteral("image");
        cycleImageEdge.edgeType = QStringLiteral("data");
        project->addConnection(cycleImageEdge);
        QVERIFY(engine.loadProject(project));
        peerA.requestLog.clear();
        peerB.requestLog.clear();
        peerA.setImages.clear();

        constexpr int boundedCycles = 2;
        const QJsonObject cycleParams{{QStringLiteral("mode"), QStringLiteral("cycle")},
                                      {QStringLiteral("maxCycles"), boundedCycles}};
        const int cycleGrabStarted = moduleSignalCount(moduleStarted, grabId);
        const int cyclePredictStarted = moduleSignalCount(moduleStarted, predictId);
        const int cycleToolBaseline = toolExecuted.count();
        const int cycleRunBaseline = runFinished.count();
        const int cycleStoppedBaseline = cycleStopped.count();
        pending = controller.handleToolCall(QStringLiteral("run_flow"), cycleParams);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QJsonArray cycleEffects = controller.pendingConfirmationPayload()
                                      .value(QStringLiteral("context"))
                                      .toObject()
                                      .value(QStringLiteral("sideEffects"))
                                      .toArray();
        QVERIFY(cycleEffects.contains(expectedImageEffect(m_imagePath)));
        const QString staleCycleConfirmation = controller.pendingConfirmationId();
        QVERIFY(!staleCycleConfirmation.isEmpty());
        QVERIFY(saveSolidImage(Qt::red));
        QVERIFY(controller.confirmPendingTools(staleCycleConfirmation));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QVERIFY(!engine.isBusy());
        QVERIFY(engine.cycleSessionId().isEmpty());
        QCOMPARE(moduleSignalCount(moduleStarted, grabId), cycleGrabStarted);
        QCOMPARE(moduleSignalCount(moduleStarted, predictId), cyclePredictStarted);
        QCOMPARE(runFinished.count(), cycleRunBaseline);
        QCOMPARE(cycleStopped.count(), cycleStoppedBaseline);
        QCOMPARE(toolExecuted.count(), cycleToolBaseline);
        QVERIFY(peerA.requestLog.isEmpty());
        QVERIFY(peerB.requestLog.isEmpty());
        QVERIFY(peerA.setImages.isEmpty());

        pending = controller.handleToolCall(QStringLiteral("run_flow"), cycleParams);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        cycleEffects = controller.pendingConfirmationPayload()
                           .value(QStringLiteral("context"))
                           .toObject()
                           .value(QStringLiteral("sideEffects"))
                           .toArray();
        QVERIFY(cycleEffects.contains(expectedImageEffect(m_imagePath)));
        const QString rejectedCycleConfirmation = controller.pendingConfirmationId();
        QVERIFY(!rejectedCycleConfirmation.isEmpty());
        QVERIFY(rejectedCycleConfirmation != staleCycleConfirmation);
        QVERIFY(controller.rejectPendingTools(rejectedCycleConfirmation));
        QVERIFY(engine.cycleSessionId().isEmpty());
        QCOMPARE(moduleSignalCount(moduleStarted, grabId), cycleGrabStarted);
        QCOMPARE(moduleSignalCount(moduleStarted, predictId), cyclePredictStarted);
        QCOMPARE(runFinished.count(), cycleRunBaseline);
        QCOMPARE(cycleStopped.count(), cycleStoppedBaseline);
        QCOMPARE(toolExecuted.count(), cycleToolBaseline);
        QVERIFY(peerA.requestLog.isEmpty());
        QVERIFY(peerB.requestLog.isEmpty());
        QVERIFY(peerA.setImages.isEmpty());

        pending = controller.handleToolCall(QStringLiteral("run_flow"), cycleParams);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        cycleEffects = controller.pendingConfirmationPayload()
                           .value(QStringLiteral("context"))
                           .toObject()
                           .value(QStringLiteral("sideEffects"))
                           .toArray();
        QVERIFY(cycleEffects.contains(expectedImageEffect(m_imagePath)));
        const QString approvedCycleConfirmation = controller.pendingConfirmationId();
        QVERIFY(!approvedCycleConfirmation.isEmpty());
        QVERIFY(approvedCycleConfirmation != rejectedCycleConfirmation);
        QString cycleSessionId;
        QObject cycleSessionContext;
        connect(controller.actor(), &AgentActor::toolExecuted, &cycleSessionContext,
                [&cycleSessionId](const QString& toolName, const QJsonObject& result) {
                    if (toolName == QLatin1String("run_flow"))
                        cycleSessionId = result.value(QStringLiteral("sessionId")).toString();
                });
        QVERIFY(controller.confirmPendingTools(approvedCycleConfirmation));
        QTRY_COMPARE_WITH_TIMEOUT(cycleStopped.count(), cycleStoppedBaseline + 1, 10000);
        QCOMPARE(toolExecuted.count(), cycleToolBaseline + 1);
        QCOMPARE(toolExecuted.last().at(1).toJsonObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("started"));
        QVERIFY(!cycleSessionId.isEmpty());
        QCOMPARE(runFinished.count(), cycleRunBaseline + boundedCycles);
        QCOMPARE(moduleSignalCount(moduleStarted, grabId), cycleGrabStarted + boundedCycles);
        QCOMPARE(moduleSignalCount(moduleFinished, grabId), moduleSignalCount(moduleStarted, grabId));
        QCOMPARE(moduleSignalCount(moduleStarted, predictId), cyclePredictStarted + boundedCycles);
        QCOMPARE(moduleSignalCount(moduleFinished, predictId), moduleSignalCount(moduleStarted, predictId));
        QCOMPARE(peerA.requestLog.count(QStringLiteral("POST /set_image")), boundedCycles);
        QCOMPARE(peerA.requestLog.count(QStringLiteral("POST /predict")), boundedCycles);
        QCOMPARE(peerA.setImages.size(), boundedCycles);
        for (const QImage& cycleImage : peerA.setImages) {
            QVERIFY(!cycleImage.isNull());
            QCOMPARE(cycleImage.size(), QSize(64, 48));
            QCOMPARE(cycleImage.pixelColor(0, 0), QColor(Qt::red));
        }
        QVERIFY(peerA.lastSetImageReadable);
        QCOMPARE(peerA.lastSetImage.size(), QSize(64, 48));
        QCOMPARE(peerA.lastSetImage.pixelColor(0, 0), QColor(Qt::red));
        QVERIFY(peerB.requestLog.isEmpty());
        for (int i = cycleRunBaseline; i < runFinished.count(); ++i)
            QVERIFY2(qvariant_cast<RunResult>(runFinished.at(i).at(0)).success,
                     qPrintable(qvariant_cast<RunResult>(runFinished.at(i).at(0)).errorMessage));

        QJsonObject cycleStatus =
            controller.handleToolCall(QStringLiteral("get_run_results"), {{QStringLiteral("runId"), cycleSessionId}});
        QCOMPARE(cycleStatus.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        QCOMPARE(cycleStatus.value(QStringLiteral("maxCycles")).toInt(), boundedCycles);
        QCOMPARE(cycleStatus.value(QStringLiteral("completedCycles")).toInt(), boundedCycles);
        const QString lastCycleRunId = qvariant_cast<RunResult>(runFinished.last().at(0)).runId;
        QCOMPARE(cycleStatus.value(QStringLiteral("lastSuccessfulRunId")).toString(), lastCycleRunId);
        const QJsonObject cycleScore = controller.handleToolCall(QStringLiteral("get_module_output"),
                                                                 {{QStringLiteral("runId"), lastCycleRunId},
                                                                  {QStringLiteral("instanceId"), predictId},
                                                                  {QStringLiteral("key"), QStringLiteral("score")}});
        QCOMPARE(cycleScore.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QVERIFY(qAbs(cycleScore.value(QStringLiteral("value")).toDouble() - 0.97) < 1e-9);

        QTest::qWait(350);
        cycleStatus =
            controller.handleToolCall(QStringLiteral("get_run_results"), {{QStringLiteral("runId"), cycleSessionId}});
        QCOMPARE(cycleStatus.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        QCOMPARE(cycleStatus.value(QStringLiteral("completedCycles")).toInt(), boundedCycles);
        QCOMPARE(cycleStopped.count(), cycleStoppedBaseline + 1);
        QCOMPARE(runFinished.count(), cycleRunBaseline + boundedCycles);
        QCOMPARE(moduleSignalCount(moduleStarted, grabId), cycleGrabStarted + boundedCycles);
        QCOMPARE(moduleSignalCount(moduleStarted, predictId), cyclePredictStarted + boundedCycles);
        QCOMPARE(peerA.requestLog.count(QStringLiteral("POST /set_image")), boundedCycles);
        QCOMPARE(peerA.requestLog.count(QStringLiteral("POST /predict")), boundedCycles);
        QVERIFY(!engine.isBusy());

        // A step that starts before GrabImage must not bind or disclose the future
        // image source. Replacing that source while the Delay confirmation is open
        // must leave the confirmation valid.
        engine.clearModules();
        projects.closeProject();
        project = projects.newProject();
        QVERIFY(project);
        QVERIFY(saveSolidImage(Qt::gray));
        ModuleInstance delay;
        delay.id = QStringLiteral("slice116_future_delay");
        delay.moduleId = QStringLiteral("延时");
        delay.name = QStringLiteral("Slice 116 future-source guard");
        delay.params = QJsonObject{{QStringLiteral("delayMs"), 0}};
        grab.breakpoint = false;
        predict.breakpoint = false;
        project->addModule(delay);
        project->addModule(grab);
        project->addModule(predict);
        project->addConnection(cycleImageEdge);
        QVERIFY(engine.loadProject(project));
        peerA.requestLog.clear();
        peerB.requestLog.clear();

        const int futureStepTools = toolExecuted.count();
        const int futureStepRuns = runFinished.count();
        pending = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonArray futureStepEffects = controller.pendingConfirmationPayload()
                                                 .value(QStringLiteral("context"))
                                                 .toObject()
                                                 .value(QStringLiteral("sideEffects"))
                                                 .toArray();
        for (const QJsonValue& effect : futureStepEffects)
            QVERIFY(!effect.toString().startsWith(QStringLiteral("sam_input_image:")));
        const QString futureSourceConfirmation = controller.pendingConfirmationId();
        QVERIFY(!futureSourceConfirmation.isEmpty());
        QVERIFY(saveSolidImage(Qt::blue));
        QVERIFY(controller.confirmPendingTools(futureSourceConfirmation));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), futureStepTools + 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), futureStepRuns + 1, 5000);
        QCOMPARE(toolExecuted.last().at(1).toJsonObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("stepped"));
        QVERIFY(qvariant_cast<RunResult>(runFinished.last().at(0)).success);
        QCOMPARE(moduleSignalCount(moduleStarted, delay.id), 1);
        QCOMPARE(peerA.requestLog.size(), 0);
        QCOMPARE(peerB.requestLog.size(), 0);
        QVERIFY(!engine.isBusy());
    }
};

QTEST_MAIN(TestAgentSamTools)
#include "test_agent_sam_tools.moc"
