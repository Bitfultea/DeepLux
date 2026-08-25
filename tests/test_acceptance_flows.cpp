#include <QtTest/QtTest>

#include <core/display/DisplayData.h>
#include <core/engine/RunEngine.h>
#include <core/manager/PluginManager.h>
#include <core/model/ImageData.h>
#include <core/model/Project.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cmath>

using namespace DeepLux;

/// 阶段 0.1 固定验收工程测试。
///
/// 设计约束：
/// - 测试数据固定落在 tests/acceptance/data/，不依赖用户目录临时文件。
/// - 验收工程固定落在 tests/acceptance/projects/，图像路径用 @ACCEPTANCE_DATA@ 占位，
///   测试运行时替换为真实数据目录，保证工程文件可移植。
/// - 预期结果与允许误差落在 tests/acceptance/expected/*.json。
class TestAcceptanceFlows : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();
    void testFindCircleFlow();
    void testPointToPointDistanceFlow();
    void testPointCloudPointToPlaneDistanceFlow();
    void testFitLineFlow();

private:
    QTemporaryDir m_tempDir;
    QString m_acceptanceRoot; // tests/acceptance 绝对路径

    bool installPlugin(const QString& dirName, const QString& metadataRel, const QString& libName);
    QString loadProjectWithPlaceholder(const QString& projectRel, const QString& dataDir, Project& out);
};

void TestAcceptanceFlows::initTestCase() {
    QVERIFY(m_tempDir.isValid());
    qputenv("DEEPLUX_APP_DATA_DIR", m_tempDir.filePath("appdata").toLocal8Bit());

    // build/bin -> 仓库根/tests/acceptance
    m_acceptanceRoot = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../../tests/acceptance");
    QVERIFY2(QFileInfo::exists(m_acceptanceRoot), qPrintable("missing acceptance root: " + m_acceptanceRoot));

    PluginManager::instance().shutdown();
    PluginManager::instance().addPluginPath(m_tempDir.filePath("plugins"));

    QVERIFY2(installPlugin("GrabImage", "src/plugins/image_processing/GrabImage/metadata.json", "libGrabImagePlugin.so"),
             "install GrabImage");
    QVERIFY2(installPlugin("FindCircle", "src/plugins/detection/FindCircle/metadata.json", "libFindCirclePlugin.so"),
             "install FindCircle");
    QVERIFY2(installPlugin("MeasurementInput", "src/plugins/geometry/MeasurementInput/metadata.json",
                           "libMeasurementInputPlugin.so"),
             "install MeasurementInput");
    QVERIFY2(installPlugin("DistancePP", "src/plugins/geometry/DistancePP/metadata.json", "libDistancePPPlugin.so"),
             "install DistancePP");
    QVERIFY2(installPlugin("LoadPointCloud", "src/plugins/image_processing/LoadPointCloud/metadata.json",
                           "libLoadPointCloudPlugin.so"),
             "install LoadPointCloud");
    QVERIFY2(installPlugin("PointSurfaceDistance", "src/plugins/geometry/PointSurfaceDistance/metadata.json",
                           "libPointSurfaceDistancePlugin.so"),
             "install PointSurfaceDistance");
    QVERIFY2(installPlugin("FitLine", "src/plugins/geometry/FitLine/metadata.json", "libFitLinePlugin.so"),
             "install FitLine");

    QVERIFY(PluginManager::instance().initialize());
    QVERIFY(PluginManager::instance().loadPlugin("GrabImage"));
    QVERIFY(PluginManager::instance().loadPlugin("FindCircle"));
    QVERIFY(PluginManager::instance().loadPlugin("MeasurementInput"));
    QVERIFY(PluginManager::instance().loadPlugin("DistancePP"));
    QVERIFY(PluginManager::instance().loadPlugin("LoadPointCloud"));
    QVERIFY(PluginManager::instance().loadPlugin("PointSurfaceDistance"));
    QVERIFY(PluginManager::instance().loadPlugin("FitLine"));
}

void TestAcceptanceFlows::cleanup() {
    RunEngine::instance().stop();
    RunEngine::instance().clearModules();
    RunEngine::instance().clearOutputs();
}

bool TestAcceptanceFlows::installPlugin(const QString& dirName, const QString& metadataRel, const QString& libName) {
    const QString repoRoot = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../..");
    const QString pluginDir = m_tempDir.filePath("plugins/" + dirName);
    if (!QDir().mkpath(pluginDir))
        return false;

    const QString metaSrc = QDir(repoRoot).filePath(metadataRel);
    const QString libSrc = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../lib/" + libName);
    if (!QFileInfo::exists(metaSrc) || !QFileInfo::exists(libSrc))
        return false;

    QFile::remove(pluginDir + "/metadata.json");
    QFile::remove(pluginDir + "/" + libName);
    return QFile::copy(metaSrc, pluginDir + "/metadata.json") && QFile::copy(libSrc, pluginDir + "/" + libName);
}

QString TestAcceptanceFlows::loadProjectWithPlaceholder(const QString& projectRel, const QString& dataDir,
                                                        Project& out) {
    QFile f(QDir(m_acceptanceRoot).filePath(projectRel));
    if (!f.open(QIODevice::ReadOnly))
        return QString("cannot open project: %1").arg(f.fileName());
    QString text = QString::fromUtf8(f.readAll());
    f.close();
    text.replace(QStringLiteral("@ACCEPTANCE_DATA@"), dataDir);

    QJsonParseError err{};
    QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return QString("invalid project json: %1").arg(err.errorString());
    out.fromJson(doc.object());
    return QString();
}

void TestAcceptanceFlows::testFindCircleFlow() {
    const QString dataDir = QDir(m_acceptanceRoot).filePath("data");

    Project project;
    QString loadErr = loadProjectWithPlaceholder("projects/accept_findcircle.json", dataDir, project);
    QVERIFY2(loadErr.isEmpty(), qPrintable(loadErr));

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "loadProject into RunEngine failed");

    engine.runOnce();

    const ImageData out = engine.moduleOutput(QStringLiteral("findcircle"));
    QVERIFY2(out.isValid(), "FindCircle produced no output");

    // 读取预期结果与允许误差
    QFile ef(QDir(m_acceptanceRoot).filePath("expected/circle_640x480.json"));
    QVERIFY(ef.open(QIODevice::ReadOnly));
    QJsonObject expected = QJsonDocument::fromJson(ef.readAll()).object();
    ef.close();

    const double expCx = expected["circle_center_x"].toDouble();
    const double expCy = expected["circle_center_y"].toDouble();
    const double expR = expected["circle_radius"].toDouble();
    const double tolC = expected["tolerance_center_px"].toDouble();
    const double tolR = expected["tolerance_radius_px"].toDouble();

    const double gotCx = out.data("circle_center_x").toDouble();
    const double gotCy = out.data("circle_center_y").toDouble();
    const double gotR = out.data("circle_radius").toDouble();

    QVERIFY2(std::abs(gotCx - expCx) <= tolC,
             qPrintable(QString("circle_center_x off: got %1 want %2 ±%3").arg(gotCx).arg(expCx).arg(tolC)));
    QVERIFY2(std::abs(gotCy - expCy) <= tolC,
             qPrintable(QString("circle_center_y off: got %1 want %2 ±%3").arg(gotCy).arg(expCy).arg(tolC)));
    QVERIFY2(std::abs(gotR - expR) <= tolR,
             qPrintable(QString("circle_radius off: got %1 want %2 ±%3").arg(gotR).arg(expR).arg(tolR)));

    // 步4: 强类型 Circle2D 输出应存在且与标量值一致
    QVERIFY2(out.data("circle").canConvert<Circle2D>(), "FindCircle must emit typed Circle2D");
    const Circle2D circle = out.data("circle").value<Circle2D>();
    QVERIFY(circle.isValid());
    QVERIFY2(std::abs(circle.centerX - expCx) <= tolC, "Circle2D.centerX mismatch");
    QVERIFY2(std::abs(circle.radius - expR) <= tolR, "Circle2D.radius mismatch");
}

void TestAcceptanceFlows::testPointToPointDistanceFlow() {
    Project project;
    QString loadErr = loadProjectWithPlaceholder("projects/accept_distancepp.json", QString(), project);
    QVERIFY2(loadErr.isEmpty(), qPrintable(loadErr));

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "load point-to-point project");
    engine.runOnce();

    const ImageData out = engine.moduleOutput(QStringLiteral("distancepp"));
    QVERIFY2(out.data("distance").isValid(), "DistancePP produced no distance output");

    QFile expectedFile(QDir(m_acceptanceRoot).filePath("expected/two_points_640x480.json"));
    QVERIFY(expectedFile.open(QIODevice::ReadOnly));
    const QJsonObject expected = QJsonDocument::fromJson(expectedFile.readAll()).object();
    const double actual = out.data("distance").toDouble();
    QVERIFY2(std::abs(actual - expected["distance"].toDouble()) <= expected["tolerance_distance_px"].toDouble(),
             qPrintable(QString("distance off: got %1").arg(actual)));
}

void TestAcceptanceFlows::testPointCloudPointToPlaneDistanceFlow() {
    const QString dataDir = QDir(m_acceptanceRoot).filePath("data");
    Project project;
    QString loadErr = loadProjectWithPlaceholder("projects/accept_point_surface.json", dataDir, project);
    QVERIFY2(loadErr.isEmpty(), qPrintable(loadErr));

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "load point-cloud measurement project");
    engine.runOnce();

    const ImageData loaded = engine.moduleOutput(QStringLiteral("loadcloud"));
    QVERIFY2(loaded.data("point_count").toInt() == 273, "point cloud vertex count mismatch");
    const ImageData out = engine.moduleOutput(QStringLiteral("pointsurface"));
    QVERIFY2(out.data("distance").isValid(), "PointSurfaceDistance produced no distance output");

    QFile expectedFile(QDir(m_acceptanceRoot).filePath("expected/plane_z5.json"));
    QVERIFY(expectedFile.open(QIODevice::ReadOnly));
    const QJsonObject expected = QJsonDocument::fromJson(expectedFile.readAll()).object();
    const double actual = out.data("distance").toDouble();
    QVERIFY2(std::abs(actual - expected["point_surface_distance"].toDouble()) <=
                 expected["tolerance_distance"].toDouble(),
             qPrintable(QString("point-surface distance off: got %1").arg(actual)));
}

void TestAcceptanceFlows::testFitLineFlow() {
    Project project;
    QString loadErr = loadProjectWithPlaceholder("projects/accept_fitline.json", QString(), project);
    QVERIFY2(loadErr.isEmpty(), qPrintable(loadErr));

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "load fit-line project");
    engine.runOnce();

    const ImageData out = engine.moduleOutput(QStringLiteral("fitline"));
    QVERIFY2(out.data("line_error").isValid(), "FitLine produced no line_error output");

    QFile expectedFile(QDir(m_acceptanceRoot).filePath("expected/fitline_points.json"));
    QVERIFY(expectedFile.open(QIODevice::ReadOnly));
    const QJsonObject expected = QJsonDocument::fromJson(expectedFile.readAll()).object();

    // 共线点 LS 拟合，误差应接近 0
    const double fitError = out.data("line_error").toDouble();
    QVERIFY2(fitError <= expected["tolerance_fit_error"].toDouble(),
             qPrintable(QString("fit error too large: got %1").arg(fitError)));

    // 拟合直线应经过已知线段中点 (320,240) 附近：用拟合直线方程验证
    // line_row/col 为拟合线上两点，验证中点到拟合线距离很小
    const double row1 = out.data("line_row1").toDouble();
    const double col1 = out.data("line_col1").toDouble();
    const double row2 = out.data("line_row2").toDouble();
    const double col2 = out.data("line_col2").toDouble();
    // 拟合线方向 (col2-col1, row2-row1)，中点 (320,240) 到线的距离
    const double dx = col2 - col1;
    const double dy = row2 - row1;
    const double len = std::sqrt(dx * dx + dy * dy);
    QVERIFY(len > 1e-6);
    // 点 (x=320, y=240)；线过 (col1, row1)，方向 (dx,dy)
    const double distToLine = std::abs(dy * 320.0 - dx * 240.0 + (dx * row1 - dy * col1)) / len;
    QVERIFY2(distToLine <= expected["tolerance_endpoint_px"].toDouble(),
             qPrintable(QString("fitted line far from known midpoint: %1").arg(distToLine)));
}

QTEST_MAIN(TestAcceptanceFlows)
#include "test_acceptance_flows.moc"
