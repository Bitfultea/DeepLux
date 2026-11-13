#include "core/agent/AgentActor.h"
#include "core/agent/AgentController.h"
#include "core/agent/ToolSchema.h"
#include "core/common/UserManager.h"
#include "core/engine/RunEngine.h"
#include "core/manager/GlobalVarManager.h"
#include "core/manager/PluginManager.h"
#include "core/manager/ProjectManager.h"
#include "core/model/Project.h"

#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUuid>
#include <QtTest/QtTest>
#include <algorithm>
#include <opencv2/cvconfig.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect.hpp>

using namespace DeepLux;

class TestAgentVisualPluginsG8 : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_appDataDir;

    static bool installPlugin(const QString& root, const QString& name, const QString& library,
                              const QString& metadata) {
        const QString pluginDirectory = QDir(root).filePath(name);
        if (!QDir().mkpath(pluginDirectory))
            return false;
        return QFile::copy(library, QDir(pluginDirectory).filePath(QFileInfo(library).fileName())) &&
               QFile::copy(metadata, QDir(pluginDirectory).filePath(QStringLiteral("metadata.json")));
    }

    static QJsonObject setParams(AgentActor& actor, const QString& instanceId, const QJsonObject& values) {
        return actor.executeTool(
            QStringLiteral("set_module_params"),
            QJsonObject{{QStringLiteral("instanceId"), instanceId}, {QStringLiteral("values"), values}});
    }

    static QJsonObject setEnabled(AgentActor& actor, const QString& instanceId, bool enabled) {
        return actor.executeTool(
            QStringLiteral("set_module_enabled"),
            QJsonObject{{QStringLiteral("instanceId"), instanceId}, {QStringLiteral("enabled"), enabled}});
    }

    static QJsonObject runToTerminal(AgentActor& actor) {
        const QJsonObject started =
            actor.executeTool(QStringLiteral("run_flow"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")},
                                                                      {QStringLiteral("wait"), false}});
        if (started.value(QStringLiteral("status")).toString() != QLatin1String("started"))
            return started;

        const QString runId = started.value(QStringLiteral("runId")).toString();
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 5000) {
            QJsonObject result =
                actor.executeTool(QStringLiteral("get_run_results"), QJsonObject{{QStringLiteral("runId"), runId}});
            const QString status = result.value(QStringLiteral("status")).toString();
            if (status == QLatin1String("completed") || status == QLatin1String("failed") ||
                status == QLatin1String("cancelled") || status == QLatin1String("uncertain")) {
                result.insert(QStringLiteral("runId"), runId);
                return result;
            }
            QTest::qWait(5);
        }
        return QJsonObject{{QStringLiteral("status"), QStringLiteral("test_timeout")}};
    }

    static QJsonObject waitForRun(AgentActor& actor, const QString& runId) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 5000) {
            const QJsonObject result =
                actor.executeTool(QStringLiteral("get_run_results"), QJsonObject{{QStringLiteral("runId"), runId}});
            const QString status = result.value(QStringLiteral("status")).toString();
            if (status == QLatin1String("completed") || status == QLatin1String("failed") ||
                status == QLatin1String("cancelled") || status == QLatin1String("uncertain"))
                return result;
            QTest::qWait(5);
        }
        return QJsonObject{{QStringLiteral("status"), QStringLiteral("test_timeout")}};
    }

    static QString createQrFixture(const QString& path) {
        const QString expected = QStringLiteral("DEEPLUX-G8-QR-2026");
        if (!QFile::copy(QStringLiteral(G8_QRCODE_FIXTURE), path))
            return {};
        return expected;
    }

private slots:
    void initTestCase() {
        QVERIFY(m_appDataDir.isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", m_appDataDir.path().toLocal8Bit());
        const QString pluginRoot = QDir(m_appDataDir.path()).filePath(QStringLiteral("plugins"));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("GrabImage"), QStringLiteral(G8_GRABIMAGE_PLUGIN),
                              QStringLiteral(G8_GRABIMAGE_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("QRCode"), QStringLiteral(G8_QRCODE_PLUGIN),
                              QStringLiteral(G8_QRCODE_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("ShowImage"), QStringLiteral(G8_SHOWIMAGE_PLUGIN),
                              QStringLiteral(G8_SHOWIMAGE_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("SaveImage"), QStringLiteral(G8_SAVEIMAGE_PLUGIN),
                              QStringLiteral(G8_SAVEIMAGE_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("DisplayData"), QStringLiteral(G8_DISPLAYDATA_PLUGIN),
                              QStringLiteral(G8_DISPLAYDATA_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("ColorRecognition"),
                              QStringLiteral(G8_COLORRECOGNITION_PLUGIN),
                              QStringLiteral(G8_COLORRECOGNITION_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("Blob"), QStringLiteral(G8_BLOB_PLUGIN),
                              QStringLiteral(G8_BLOB_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("FindCircle"), QStringLiteral(G8_FINDCIRCLE_PLUGIN),
                              QStringLiteral(G8_FINDCIRCLE_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("CropImage"), QStringLiteral(G8_CROPIMAGE_PLUGIN),
                              QStringLiteral(G8_CROPIMAGE_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("MeasureCircle"), QStringLiteral(G8_MEASURECIRCLE_PLUGIN),
                              QStringLiteral(G8_MEASURECIRCLE_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("MeasurementInput"),
                              QStringLiteral(G8_MEASUREMENTINPUT_PLUGIN),
                              QStringLiteral(G8_MEASUREMENTINPUT_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("DistancePP"), QStringLiteral(G8_DISTANCEPP_PLUGIN),
                              QStringLiteral(G8_DISTANCEPP_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("DistancePL"), QStringLiteral(G8_DISTANCEPL_PLUGIN),
                              QStringLiteral(G8_DISTANCEPL_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("FitCircle"), QStringLiteral(G8_FITCIRCLE_PLUGIN),
                              QStringLiteral(G8_FITCIRCLE_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("FitLine"), QStringLiteral(G8_FITLINE_PLUGIN),
                              QStringLiteral(G8_FITLINE_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("FitEllipse"), QStringLiteral(G8_FITELLIPSE_PLUGIN),
                              QStringLiteral(G8_FITELLIPSE_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("CalculateOffset"), QStringLiteral(G8_CALCULATEOFFSET_PLUGIN),
                              QStringLiteral(G8_CALCULATEOFFSET_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("FitPlane"), QStringLiteral(G8_FITPLANE_PLUGIN),
                              QStringLiteral(G8_FITPLANE_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("MeasureLine"), QStringLiteral(G8_MEASURELINE_PLUGIN),
                              QStringLiteral(G8_MEASURELINE_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("MeasureRect"), QStringLiteral(G8_MEASURERECT_PLUGIN),
                              QStringLiteral(G8_MEASURERECT_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("LinesDistance"), QStringLiteral(G8_LINESDISTANCE_PLUGIN),
                              QStringLiteral(G8_LINESDISTANCE_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("MeasureGap"), QStringLiteral(G8_MEASUREGAP_PLUGIN),
                              QStringLiteral(G8_MEASUREGAP_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("PointSurfaceDistance"),
                              QStringLiteral(G8_POINTSURFACEDISTANCE_PLUGIN),
                              QStringLiteral(G8_POINTSURFACEDISTANCE_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("ImageScript"), QStringLiteral(G8_IMAGESCRIPT_PLUGIN),
                              QStringLiteral(G8_IMAGESCRIPT_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("LoadPointCloud"), QStringLiteral(G8_LOADPOINTCLOUD_PLUGIN),
                              QStringLiteral(G8_LOADPOINTCLOUD_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("FreeformSurface"), QStringLiteral(G8_FREEFORMSURFACE_PLUGIN),
                              QStringLiteral(G8_FREEFORMSURFACE_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("JigsawPuzzle"), QStringLiteral(G8_JIGSAWPUZZLE_PLUGIN),
                              QStringLiteral(G8_JIGSAWPUZZLE_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("QueueOut"), QStringLiteral(G8_QUEUEOUT_PLUGIN),
                              QStringLiteral(G8_QUEUEOUT_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("QueueIn"), QStringLiteral(G8_QUEUEIN_PLUGIN),
                              QStringLiteral(G8_QUEUEIN_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("CreateString"), QStringLiteral(G8_CREATESTRING_PLUGIN),
                              QStringLiteral(G8_CREATESTRING_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("GapMeasure3D"), QStringLiteral(G8_GAPMEASURE3D_PLUGIN),
                              QStringLiteral(G8_GAPMEASURE3D_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("PerProcessing"), QStringLiteral(G8_PERPROCESSING_PLUGIN),
                              QStringLiteral(G8_PERPROCESSING_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("3DPreProcessing"), QStringLiteral(G8_PREPROCESSING3D_PLUGIN),
                              QStringLiteral(G8_PREPROCESSING3D_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("While"), QStringLiteral(G8_WHILE_PLUGIN),
                              QStringLiteral(G8_WHILE_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("StopWhile"), QStringLiteral(G8_STOPWHILE_PLUGIN),
                              QStringLiteral(G8_STOPWHILE_METADATA)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("Delay"), QStringLiteral(G8_DELAY_PLUGIN),
                              QStringLiteral(G8_DELAY_METADATA)));

        PluginManager& plugins = PluginManager::instance();
        QVERIFY(plugins.initialize());
        QVERIFY(plugins.pluginPaths().contains(QDir(pluginRoot).absolutePath()));
        QCOMPARE(plugins.availableModules().size(), 36);
        ToolSchema::instance().registerDefaultTools();
        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
        qRegisterMetaType<RunResult>("DeepLux::RunResult");
    }

    void cleanupTestCase() {
        RunEngine& engine = RunEngine::instance();
        if (engine.isBusy()) {
            engine.stop();
            QElapsedTimer timer;
            timer.start();
            while (engine.isBusy() && timer.elapsed() < 5000)
                QTest::qWait(10);
            if (engine.isBusy()) {
                QFAIL("RunEngine did not stop within 5 seconds; leaving project and plugin resources intact");
                return;
            }
        }
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
        PluginManager::instance().shutdown();
        qunsetenv("DEEPLUX_APP_DATA_DIR");
    }

    void testAgentImageScriptInvertAndRejectsInvalidConfigurationAndMissingImage() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();

        const QString sourcePath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-imagescript-source.png"));
        cv::Mat source(3, 4, CV_8UC3);
        for (int row = 0; row < source.rows; ++row) {
            for (int col = 0; col < source.cols; ++col) {
                source.at<cv::Vec3b>(row, col) =
                    cv::Vec3b(static_cast<uchar>(10 + row * 17 + col * 3), static_cast<uchar>(40 + row * 11 + col * 5),
                              static_cast<uchar>(90 + row * 7 + col * 9));
            }
        }
        QVERIFY(cv::imwrite(sourcePath.toStdString(), source));

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 ImageScript")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        for (const QString& plugin : {QStringLiteral("GrabImage"), QStringLiteral("ImageScript")}) {
            const QString status =
                actor.executeTool(QStringLiteral("load_module_plugin"), QJsonObject{{QStringLiteral("plugin"), plugin}})
                    .value(QStringLiteral("status"))
                    .toString();
            QVERIFY(status == QLatin1String("loaded") || status == QLatin1String("already_loaded"));
        }

        const QJsonObject grab =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.grabimage")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_grab_image")}});
        QCOMPARE(grab.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString grabId = grab.value(QStringLiteral("instanceId")).toString();
        const QJsonObject script =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.imagescript")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_image_script")}});
        QCOMPARE(script.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString scriptId = script.value(QStringLiteral("instanceId")).toString();

        QCOMPARE(setParams(actor, grabId,
                           QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Path")},
                                       {QStringLiteral("filePath"), sourcePath}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        QCOMPARE(setParams(actor, scriptId, QJsonObject{{QStringLiteral("scriptType"), 0}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject connected = actor.executeTool(
            QStringLiteral("connect_modules"), QJsonObject{{QStringLiteral("fromId"), grabId},
                                                           {QStringLiteral("toId"), scriptId},
                                                           {QStringLiteral("fromPort"), QStringLiteral("image")},
                                                           {QStringLiteral("toPort"), QStringLiteral("image")}});
        QCOMPARE(connected.value(QStringLiteral("status")).toString(), QStringLiteral("connected"));

        const QJsonObject successfulRun = runToTerminal(actor);
        QCOMPARE(successfulRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = successfulRun.value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());
        for (const auto& expected : {qMakePair(QStringLiteral("script_executed"), QJsonValue(true)),
                                     qMakePair(QStringLiteral("script_type"), QJsonValue(0))}) {
            const QJsonObject value = actor.executeTool(QStringLiteral("get_module_output"),
                                                        QJsonObject{{QStringLiteral("runId"), runId},
                                                                    {QStringLiteral("instanceId"), scriptId},
                                                                    {QStringLiteral("key"), expected.first}});
            QCOMPARE(value.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(value.value(QStringLiteral("value")), expected.second);
        }

        ImageData grabOutput;
        ImageData scriptOutput;
        QMap<QString, QVariant> grabValues;
        QMap<QString, QVariant> scriptValues;
        PortValueMap grabPorts;
        PortValueMap scriptPorts;
        QVERIFY(RunEngine::instance().runOutputSnapshot(runId, grabId, grabOutput, grabValues, grabPorts));
        QVERIFY(RunEngine::instance().runOutputSnapshot(runId, scriptId, scriptOutput, scriptValues, scriptPorts));
        QCOMPARE(scriptOutput.width(), source.cols);
        QCOMPARE(scriptOutput.height(), source.rows);
        const ImageCoordinateProvenance inputProvenance = grabOutput.coordinateProvenance();
        const ImageCoordinateProvenance outputProvenance = scriptOutput.coordinateProvenance();
        QCOMPARE(outputProvenance.sourceImageId, inputProvenance.sourceImageId);
        QCOMPARE(outputProvenance.sourceSize, inputProvenance.sourceSize);
        QCOMPARE(outputProvenance.localToSourceMatrix(), inputProvenance.localToSourceMatrix());
        cv::Mat expected;
        cv::bitwise_not(source, expected);
        cv::Mat actual = scriptOutput.toMat();
        QVERIFY(!actual.empty());
        QCOMPARE(actual.size(), expected.size());
        cv::Mat difference;
        cv::absdiff(actual, expected, difference);
        QCOMPARE(cv::countNonZero(difference.reshape(1)), 0);

        for (int mode = 1; mode <= 3; ++mode) {
            cv::Mat modeSource(9, 9, CV_8UC3);
            cv::Mat modeExpected(9, 9, CV_8UC3);
            if (mode == 1) {
                modeSource.setTo(cv::Scalar(20, 80, 180));
                modeExpected.setTo(cv::Scalar(103, 103, 103));
            } else if (mode == 2) {
                modeSource.setTo(cv::Scalar(80, 80, 80));
                modeSource.at<cv::Vec3b>(4, 4) = cv::Vec3b(180, 180, 180);
                modeExpected.setTo(cv::Scalar(80, 80, 80));
                modeExpected(cv::Rect(2, 2, 5, 5)).setTo(cv::Scalar(84, 84, 84));
            } else {
                modeSource.setTo(cv::Scalar(80, 80, 80));
                modeSource.at<cv::Vec3b>(4, 4) = cv::Vec3b(100, 100, 100);
                modeExpected.setTo(cv::Scalar(80, 80, 80));
                modeExpected.at<cv::Vec3b>(4, 4) = cv::Vec3b(180, 180, 180);
                modeExpected.at<cv::Vec3b>(3, 4) = cv::Vec3b(60, 60, 60);
                modeExpected.at<cv::Vec3b>(5, 4) = cv::Vec3b(60, 60, 60);
                modeExpected.at<cv::Vec3b>(4, 3) = cv::Vec3b(60, 60, 60);
                modeExpected.at<cv::Vec3b>(4, 5) = cv::Vec3b(60, 60, 60);
            }

            const QString modeSourcePath =
                QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-imagescript-mode-%1.png").arg(mode));
            QVERIFY(cv::imwrite(modeSourcePath.toStdString(), modeSource));
            QCOMPARE(setParams(actor, grabId, QJsonObject{{QStringLiteral("filePath"), modeSourcePath}})
                         .value(QStringLiteral("status"))
                         .toString(),
                     QStringLiteral("params_set"));
            QCOMPARE(setParams(actor, scriptId, QJsonObject{{QStringLiteral("scriptType"), mode}})
                         .value(QStringLiteral("status"))
                         .toString(),
                     QStringLiteral("params_set"));

            const QJsonObject modeRun = runToTerminal(actor);
            QCOMPARE(modeRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
            const QString modeRunId = modeRun.value(QStringLiteral("runId")).toString();
            QVERIFY(!modeRunId.isEmpty());
            QCOMPARE(actor
                         .executeTool(QStringLiteral("get_module_output"),
                                      QJsonObject{{QStringLiteral("runId"), modeRunId},
                                                  {QStringLiteral("instanceId"), scriptId},
                                                  {QStringLiteral("key"), QStringLiteral("script_executed")}})
                         .value(QStringLiteral("value")),
                     QJsonValue(true));
            QCOMPARE(actor
                         .executeTool(QStringLiteral("get_module_output"),
                                      QJsonObject{{QStringLiteral("runId"), modeRunId},
                                                  {QStringLiteral("instanceId"), scriptId},
                                                  {QStringLiteral("key"), QStringLiteral("script_type")}})
                         .value(QStringLiteral("value")),
                     QJsonValue(mode));

            ImageData modeGrabOutput;
            ImageData modeScriptOutput;
            QMap<QString, QVariant> modeGrabValues;
            QMap<QString, QVariant> modeScriptValues;
            PortValueMap modeGrabPorts;
            PortValueMap modeScriptPorts;
            QVERIFY(RunEngine::instance().runOutputSnapshot(modeRunId, grabId, modeGrabOutput, modeGrabValues,
                                                            modeGrabPorts));
            QVERIFY(RunEngine::instance().runOutputSnapshot(modeRunId, scriptId, modeScriptOutput, modeScriptValues,
                                                            modeScriptPorts));
            const cv::Mat modeActual = modeScriptOutput.toMat();
            QCOMPARE(modeActual.size(), modeExpected.size());
            QCOMPARE(modeActual.type(), modeExpected.type());
            cv::Mat modeDifference;
            cv::absdiff(modeActual, modeExpected, modeDifference);
            QCOMPARE(cv::countNonZero(modeDifference.reshape(1)), 0);

            const ImageCoordinateProvenance modeInputProvenance = modeGrabOutput.coordinateProvenance();
            const ImageCoordinateProvenance modeOutputProvenance = modeScriptOutput.coordinateProvenance();
            QCOMPARE(modeOutputProvenance.sourceImageId, modeInputProvenance.sourceImageId);
            QCOMPARE(modeOutputProvenance.sourceSize, modeInputProvenance.sourceSize);
            QCOMPARE(modeOutputProvenance.localToSourceMatrix(), modeInputProvenance.localToSourceMatrix());
        }

        QSignalSpy moduleStarted(&RunEngine::instance(), &RunEngine::moduleStarted);
        const QJsonObject rejected = setParams(actor, scriptId, QJsonObject{{QStringLiteral("scriptType"), 4}});
        QCOMPARE(rejected.value(QStringLiteral("errorCode")).toString(), QStringLiteral("VALIDATION_FAILED"));
        QCOMPARE(rejected.value(QStringLiteral("error")).toString(),
                 QStringLiteral("scriptType 必须是 0–3 的整数（内置操作类型）"));
        const std::optional<ModuleInstance> currentScript =
            ProjectManager::instance().currentProject()->moduleById(scriptId);
        QVERIFY(currentScript.has_value());
        QCOMPARE(currentScript->params.value(QStringLiteral("scriptType")).toInt(), 3);
        QVERIFY(!RunEngine::instance().isBusy());
        QCOMPARE(moduleStarted.count(), 0);

        const QJsonObject disconnected = actor.executeTool(
            QStringLiteral("disconnect_modules"), QJsonObject{{QStringLiteral("fromId"), grabId},
                                                              {QStringLiteral("toId"), scriptId},
                                                              {QStringLiteral("fromPort"), QStringLiteral("image")},
                                                              {QStringLiteral("toPort"), QStringLiteral("image")}});
        QCOMPARE(disconnected.value(QStringLiteral("status")).toString(), QStringLiteral("disconnected"));
        QCOMPARE(setEnabled(actor, grabId, false).value(QStringLiteral("status")).toString(),
                 QStringLiteral("updated"));
        const QJsonObject failedRunStart = actor.executeTool(
            QStringLiteral("run_flow"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}});
        const QString failedRunId = failedRunStart.value(QStringLiteral("runId")).toString();
        QVERIFY(!failedRunId.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(RunEngine::instance().runObservation(failedRunId).finished, 5000);
        const RunObservation failedRun = RunEngine::instance().runObservation(failedRunId);
        QVERIFY(!failedRun.result.success);
        QCOMPARE(failedRun.result.failedModule, scriptId);
        QCOMPARE(failedRun.result.errorMessage, QStringLiteral("输入图像无效"));
        QCOMPARE(
            actor.executeTool(QStringLiteral("get_run_results"), QJsonObject{{QStringLiteral("runId"), failedRunId}})
                .value(QStringLiteral("status"))
                .toString(),
            QStringLiteral("failed"));
        const QJsonObject unavailable =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{QStringLiteral("runId"), failedRunId},
                                          {QStringLiteral("instanceId"), scriptId},
                                          {QStringLiteral("key"), QStringLiteral("script_executed")}});
        QCOMPARE(unavailable.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
        QCOMPARE(unavailable.value(QStringLiteral("reason")).toString(), QStringLiteral("run_failed"));

        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }

    void testAgentLoadPointCloudFeedsFreeformSurfaceAndRejectsLegacyParameter() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();

        const QString pointCloudPath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-freeformsurface.ply"));
        QFile pointCloudFile(pointCloudPath);
        QVERIFY(pointCloudFile.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream stream(&pointCloudFile);
        stream << "ply\nformat ascii 1.0\nelement vertex 4\n"
                  "property float x\nproperty float y\nproperty float z\nend_header\n"
                  "0 0 0\n1 0 0\n1 1 0\n0 1 0\n";
        pointCloudFile.close();

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 FreeformSurface")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        for (const QString& plugin : {QStringLiteral("LoadPointCloud"), QStringLiteral("FreeformSurface")}) {
            const QString status =
                actor.executeTool(QStringLiteral("load_module_plugin"), QJsonObject{{QStringLiteral("plugin"), plugin}})
                    .value(QStringLiteral("status"))
                    .toString();
            QVERIFY(status == QLatin1String("loaded") || status == QLatin1String("already_loaded"));
        }

        const QJsonObject load = actor.executeTool(
            QStringLiteral("add_module"),
            QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.loadpointcloud")},
                        {QStringLiteral("instanceName"), QStringLiteral("g8_load_point_cloud")}});
        QCOMPARE(load.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString loadId = load.value(QStringLiteral("instanceId")).toString();
        const QJsonObject surface = actor.executeTool(
            QStringLiteral("add_module"),
            QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.freeformsurface")},
                        {QStringLiteral("instanceName"), QStringLiteral("g8_freeform_surface")}});
        QCOMPARE(surface.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString surfaceId = surface.value(QStringLiteral("instanceId")).toString();
        QCOMPARE(setParams(actor, loadId,
                           QJsonObject{{QStringLiteral("filePath"), pointCloudPath},
                                       {QStringLiteral("scaleX"), 1.0},
                                       {QStringLiteral("scaleY"), 1.0},
                                       {QStringLiteral("scaleZ"), 1.0}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));

        const QJsonObject surfaceSchema = actor.executeTool(QStringLiteral("get_module_params_schema"),
                                                            QJsonObject{{QStringLiteral("instanceId"), surfaceId}});
        QVERIFY(
            !surfaceSchema.value(QStringLiteral("parameters")).toObject().contains(QStringLiteral("samplingInterval")));
        const QJsonObject connected = actor.executeTool(
            QStringLiteral("connect_modules"), QJsonObject{{QStringLiteral("fromId"), loadId},
                                                           {QStringLiteral("toId"), surfaceId},
                                                           {QStringLiteral("fromPort"), QStringLiteral("image")},
                                                           {QStringLiteral("toPort"), QStringLiteral("image")}});
        QCOMPARE(connected.value(QStringLiteral("status")).toString(), QStringLiteral("connected"));

        QSignalSpy moduleStarted(&RunEngine::instance(), &RunEngine::moduleStarted);
        const QJsonObject successfulRun = runToTerminal(actor);
        QCOMPARE(successfulRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = successfulRun.value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());
        const QJsonObject loadedPointCount = actor.executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                             {QStringLiteral("instanceId"), loadId},
                                                             {QStringLiteral("key"), QStringLiteral("point_count")}});
        QCOMPARE(loadedPointCount.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(loadedPointCount.value(QStringLiteral("value")).toInt(), 4);
        const QJsonObject count = actor.executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                             {QStringLiteral("instanceId"), surfaceId},
                                                             {QStringLiteral("key"), QStringLiteral("point_count")}});
        const QJsonObject area = actor.executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                             {QStringLiteral("instanceId"), surfaceId},
                                                             {QStringLiteral("key"), QStringLiteral("surface_area")}});
        const QJsonObject roughness =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{QStringLiteral("runId"), runId},
                                          {QStringLiteral("instanceId"), surfaceId},
                                          {QStringLiteral("key"), QStringLiteral("surface_roughness")}});
        QCOMPARE(count.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(count.value(QStringLiteral("value")).toInt(), 4);
        QCOMPARE(area.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QVERIFY(qAbs(area.value(QStringLiteral("value")).toDouble() - 1.0) < 1e-5);
        QCOMPARE(roughness.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QVERIFY(qAbs(roughness.value(QStringLiteral("value")).toDouble()) < 1e-5);
        const int startedCount = moduleStarted.count();
        QCOMPARE(startedCount, 2);

        const QJsonObject rejected =
            setParams(actor, surfaceId, QJsonObject{{QStringLiteral("samplingInterval"), 2.0}});
        QCOMPARE(rejected.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        QCOMPARE(rejected.value(QStringLiteral("error")).toString(),
                 QStringLiteral("Unknown parameter 'samplingInterval'"));
        const std::optional<ModuleInstance> currentSurface =
            ProjectManager::instance().currentProject()->moduleById(surfaceId);
        QVERIFY(currentSurface.has_value());
        QVERIFY(!currentSurface->params.contains(QStringLiteral("samplingInterval")));
        QVERIFY(!RunEngine::instance().isBusy());
        QCOMPARE(moduleStarted.count(), startedCount);

        const QString missingPath = QDir(m_appDataDir.path()).filePath(QStringLiteral("missing-freeformsurface.ply"));
        QCOMPARE(setParams(actor, loadId, QJsonObject{{QStringLiteral("filePath"), missingPath}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject missingCloudRun = runToTerminal(actor);
        QCOMPARE(missingCloudRun.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QString missingCloudRunId = missingCloudRun.value(QStringLiteral("runId")).toString();
        const RunObservation missingCloudObservation = RunEngine::instance().runObservation(missingCloudRunId);
        QCOMPARE(missingCloudObservation.result.failedModule, loadId);
        QCOMPARE(missingCloudObservation.result.errorMessage, QStringLiteral("文件不存在: %1").arg(missingPath));
        const QJsonObject unavailableLoadOutput = actor.executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), missingCloudRunId},
                                                             {QStringLiteral("instanceId"), loadId},
                                                             {QStringLiteral("key"), QStringLiteral("point_count")}});
        QCOMPARE(unavailableLoadOutput.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
        QCOMPARE(unavailableLoadOutput.value(QStringLiteral("reason")).toString(), QStringLiteral("run_failed"));

        QCOMPARE(setEnabled(actor, loadId, false).value(QStringLiteral("status")).toString(),
                 QStringLiteral("updated"));
        const QJsonObject failedRun = runToTerminal(actor);
        QCOMPARE(failedRun.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QString failedRunId = failedRun.value(QStringLiteral("runId")).toString();
        const RunObservation observation = RunEngine::instance().runObservation(failedRunId);
        QCOMPARE(observation.result.failedModule, surfaceId);
        QCOMPARE(observation.result.errorMessage, QStringLiteral("未提供点云数据"));
        const QJsonObject unavailable = actor.executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), failedRunId},
                                                             {QStringLiteral("instanceId"), surfaceId},
                                                             {QStringLiteral("key"), QStringLiteral("surface_area")}});
        QCOMPARE(unavailable.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
        QCOMPARE(unavailable.value(QStringLiteral("reason")).toString(), QStringLiteral("run_failed"));

        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }

    void testAgentJigsawPuzzleSplitsAndReassemblesGridAndRejectsUndersizedInput() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();

        const QString sourcePath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-jigsaw-source.png"));
        const QString smallPath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-jigsaw-small.png"));
        cv::Mat source(17, 19, CV_8UC3);
        for (int row = 0; row < source.rows; ++row) {
            for (int col = 0; col < source.cols; ++col) {
                source.at<cv::Vec3b>(row, col) =
                    cv::Vec3b(static_cast<uchar>(row * 9 + col), static_cast<uchar>(row * 3 + col * 7),
                              static_cast<uchar>(row * 11 + col * 5));
            }
        }
        QVERIFY(cv::imwrite(sourcePath.toStdString(), source));
        QVERIFY(cv::imwrite(smallPath.toStdString(), cv::Mat(2, 2, CV_8UC3, cv::Scalar(17, 83, 191))));

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 JigsawPuzzle")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        for (const QString& plugin : {QStringLiteral("GrabImage"), QStringLiteral("JigsawPuzzle")}) {
            const QString status =
                actor.executeTool(QStringLiteral("load_module_plugin"), QJsonObject{{QStringLiteral("plugin"), plugin}})
                    .value(QStringLiteral("status"))
                    .toString();
            QVERIFY(status == QLatin1String("loaded") || status == QLatin1String("already_loaded"));
        }

        const QJsonObject grab =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.grabimage")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_jigsaw_grab")}});
        QCOMPARE(grab.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString grabId = grab.value(QStringLiteral("instanceId")).toString();
        const QJsonObject jigsaw =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.jigsawsolver")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_jigsaw_puzzle")}});
        QCOMPARE(jigsaw.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString jigsawId = jigsaw.value(QStringLiteral("instanceId")).toString();
        QCOMPARE(setParams(actor, grabId,
                           QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Path")},
                                       {QStringLiteral("filePath"), sourcePath}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        QCOMPARE(setParams(actor, jigsawId, QJsonObject{{QStringLiteral("rows"), 3}, {QStringLiteral("cols"), 4}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject connected = actor.executeTool(
            QStringLiteral("connect_modules"), QJsonObject{{QStringLiteral("fromId"), grabId},
                                                           {QStringLiteral("toId"), jigsawId},
                                                           {QStringLiteral("fromPort"), QStringLiteral("image")},
                                                           {QStringLiteral("toPort"), QStringLiteral("image")}});
        QCOMPARE(connected.value(QStringLiteral("status")).toString(), QStringLiteral("connected"));

        const QJsonObject successfulRun = runToTerminal(actor);
        QCOMPARE(successfulRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = successfulRun.value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());
        for (const auto& expected :
             {qMakePair(QStringLiteral("puzzle_rows"), 3), qMakePair(QStringLiteral("puzzle_cols"), 4),
              qMakePair(QStringLiteral("puzzle_piece_count"), 12)}) {
            const QJsonObject value = actor.executeTool(QStringLiteral("get_module_output"),
                                                        QJsonObject{{QStringLiteral("runId"), runId},
                                                                    {QStringLiteral("instanceId"), jigsawId},
                                                                    {QStringLiteral("key"), expected.first}});
            QCOMPARE(value.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(value.value(QStringLiteral("value")).toInt(), expected.second);
        }
        ImageData jigsawOutput;
        QMap<QString, QVariant> namedValues;
        PortValueMap ports;
        QVERIFY(RunEngine::instance().runOutputSnapshot(runId, jigsawId, jigsawOutput, namedValues, ports));
        QCOMPARE(jigsawOutput.width(), 16);
        QCOMPARE(jigsawOutput.height(), 15);
        cv::Mat actual = jigsawOutput.toMat();
        const cv::Mat expected = source(cv::Rect(0, 0, 16, 15)).clone();
        QCOMPARE(actual.size(), expected.size());
        cv::Mat difference;
        cv::absdiff(actual, expected, difference);
        QCOMPARE(cv::countNonZero(difference.reshape(1)), 0);

        QCOMPARE(setParams(actor, grabId, QJsonObject{{QStringLiteral("filePath"), smallPath}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject failedRun = runToTerminal(actor);
        QCOMPARE(failedRun.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QString failedRunId = failedRun.value(QStringLiteral("runId")).toString();
        const RunObservation observation = RunEngine::instance().runObservation(failedRunId);
        QCOMPARE(observation.result.failedModule, jigsawId);
        QCOMPARE(observation.result.errorMessage, QStringLiteral("图像尺寸不足以按当前行列数切分"));
        const QJsonObject unavailable =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{QStringLiteral("runId"), failedRunId},
                                          {QStringLiteral("instanceId"), jigsawId},
                                          {QStringLiteral("key"), QStringLiteral("puzzle_piece_count")}});
        QCOMPARE(unavailable.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
        QCOMPARE(unavailable.value(QStringLiteral("reason")).toString(), QStringLiteral("run_failed"));

        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }

    void testAgentLoadsDecodesAndDisplaysRealImageOutput() {
        const QString qrPath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-qr.png"));
        const QString blankPath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-blank.png"));
        const QString expectedQr = createQrFixture(qrPath);
        QVERIFY(!expectedQr.isEmpty());
#ifndef HAVE_QUIRC
        QSKIP("OpenCV was built without QUIRC QR decoding support");
#endif
        cv::QRCodeDetector qrCapabilityProbe;
        const std::string decodedQr = qrCapabilityProbe.detectAndDecode(cv::imread(qrPath.toStdString()));
        QVERIFY2(decodedQr == expectedQr.toStdString(), "OpenCV with QUIRC failed to decode the generated QR fixture");
        QVERIFY(cv::imwrite(blankPath.toStdString(), cv::Mat(296, 296, CV_8UC1, cv::Scalar(255))));

        AgentActor actor;
        const QJsonObject created =
            actor.executeTool(QStringLiteral("create_project"),
                              QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 visual plugins")}});
        QCOMPARE(created.value(QStringLiteral("status")).toString(), QStringLiteral("created"));

        for (const QString& plugin :
             {QStringLiteral("GrabImage"), QStringLiteral("QRCode"), QStringLiteral("ShowImage")}) {
            const QJsonObject loaded = actor.executeTool(QStringLiteral("load_module_plugin"),
                                                         QJsonObject{{QStringLiteral("plugin"), plugin}});
            const QString status = loaded.value(QStringLiteral("status")).toString();
            QVERIFY(status == QLatin1String("loaded") || status == QLatin1String("already_loaded"));
        }

        const QJsonObject grab =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.grabimage")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_grab_image")}});
        QCOMPARE(grab.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString grabId = grab.value(QStringLiteral("instanceId")).toString();
        const QJsonObject qr =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("QRCode")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_qr_decode")}});
        QCOMPARE(qr.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString qrId = qr.value(QStringLiteral("instanceId")).toString();
        const QJsonObject show =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.showimage")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_show_image")}});
        QCOMPARE(show.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString showId = show.value(QStringLiteral("instanceId")).toString();

        const QJsonObject grabConfigured = setParams(
            actor, grabId,
            QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Path")}, {QStringLiteral("filePath"), qrPath}});
        QCOMPARE(grabConfigured.value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
        const QJsonObject qrConfigured =
            setParams(actor, qrId, QJsonObject{{QStringLiteral("codeType"), QStringLiteral("QR_Code")}});
        QCOMPARE(qrConfigured.value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
        const QJsonObject showConfigured =
            setParams(actor, showId,
                      QJsonObject{{QStringLiteral("windowTitle"), QStringLiteral("G8 QR Fixture")},
                                  {QStringLiteral("delay"), 0}});
        QCOMPARE(showConfigured.value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));

        for (const auto& edge : {qMakePair(grabId, qrId), qMakePair(qrId, showId)}) {
            const QJsonObject connected = actor.executeTool(
                QStringLiteral("connect_modules"), QJsonObject{{QStringLiteral("fromId"), edge.first},
                                                               {QStringLiteral("toId"), edge.second},
                                                               {QStringLiteral("fromPort"), QStringLiteral("image")},
                                                               {QStringLiteral("toPort"), QStringLiteral("image")}});
            QCOMPARE(connected.value(QStringLiteral("status")).toString(), QStringLiteral("connected"));
        }

        const QJsonObject successRun = runToTerminal(actor);
        QCOMPARE(successRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = successRun.value(QStringLiteral("runId")).toString();
        const QJsonObject decoded = actor.executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                             {QStringLiteral("instanceId"), qrId},
                                                             {QStringLiteral("key"), QStringLiteral("qr_result")}});
        QCOMPARE(decoded.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(decoded.value(QStringLiteral("value")).toString(), expectedQr);
        const QJsonObject displayed = actor.executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                             {QStringLiteral("instanceId"), showId},
                                                             {QStringLiteral("key"), QStringLiteral("image_info")}});
        QCOMPARE(displayed.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QVERIFY(displayed.value(QStringLiteral("value")).toString().contains(QStringLiteral("296x296")));

        const QJsonObject missingQrConfigured =
            setParams(actor, grabId, QJsonObject{{QStringLiteral("filePath"), blankPath}});
        QCOMPARE(missingQrConfigured.value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
        const QJsonObject qrFailure = runToTerminal(actor);
        QCOMPARE(qrFailure.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QJsonObject qrError = qrFailure.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(qrError.value(QStringLiteral("moduleId")).toString(), qrId);
        QVERIFY(!qrError.value(QStringLiteral("message")).toString().isEmpty());

        QCOMPARE(setEnabled(actor, grabId, false).value(QStringLiteral("status")).toString(),
                 QStringLiteral("updated"));
        QCOMPARE(setEnabled(actor, qrId, false).value(QStringLiteral("status")).toString(), QStringLiteral("updated"));
        const QJsonObject showFailure = runToTerminal(actor);
        QCOMPARE(showFailure.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QJsonObject showError = showFailure.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(showError.value(QStringLiteral("moduleId")).toString(), showId);
        QVERIFY(!showError.value(QStringLiteral("message")).toString().isEmpty());
    }

    void testAgentConfirmsSaveImageAndReportsEncoderFailure() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
        const QString sourcePath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-save-image-source.png"));
        const QString outputPath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-save-image-output.png"));
        cv::Mat source(48, 64, CV_8UC3, cv::Scalar(12, 80, 190));
        cv::rectangle(source, cv::Rect(8, 8, 24, 18), cv::Scalar(220, 40, 30), cv::FILLED);
        QVERIFY(cv::imwrite(sourcePath.toStdString(), source));

        AgentActor actor;
        AgentController& controller = AgentController::instance();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 SaveImage")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        for (const QString& plugin : {QStringLiteral("GrabImage"), QStringLiteral("SaveImage")}) {
            const QString status =
                actor.executeTool(QStringLiteral("load_module_plugin"), QJsonObject{{QStringLiteral("plugin"), plugin}})
                    .value(QStringLiteral("status"))
                    .toString();
            QVERIFY(status == QLatin1String("loaded") || status == QLatin1String("already_loaded"));
        }
        const QJsonObject grab =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.grabimage")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_save_image_source")}});
        const QJsonObject save =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.saveimage")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_save_image_sink")}});
        QCOMPARE(grab.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        QCOMPARE(save.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString grabId = grab.value(QStringLiteral("instanceId")).toString();
        const QString saveId = save.value(QStringLiteral("instanceId")).toString();
        QCOMPARE(setParams(actor, grabId,
                           QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Path")},
                                       {QStringLiteral("filePath"), sourcePath}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const auto configureSave = [&](const QString& path) {
            return setParams(actor, saveId,
                             QJsonObject{{QStringLiteral("filePath"), path},
                                         {QStringLiteral("format"), QStringLiteral("png")},
                                         {QStringLiteral("quality"), 100}});
        };
        QCOMPARE(configureSave(outputPath).value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
        QCOMPARE(actor
                     .executeTool(QStringLiteral("connect_modules"),
                                  QJsonObject{{QStringLiteral("fromId"), grabId},
                                              {QStringLiteral("toId"), saveId},
                                              {QStringLiteral("fromPort"), QStringLiteral("image")},
                                              {QStringLiteral("toPort"), QStringLiteral("image")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("connected"));

        QSignalSpy runFinished(&RunEngine::instance(), &RunEngine::runFinished);
        QJsonObject pending = controller.handleToolCall(QStringLiteral("run_flow"),
                                                        QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(!QFileInfo::exists(outputPath));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const RunResult savedResult = qvariant_cast<RunResult>(runFinished.takeFirst().at(0));
        QVERIFY2(savedResult.success, qPrintable(savedResult.errorMessage));
        const cv::Mat saved = cv::imread(outputPath.toStdString(), cv::IMREAD_COLOR);
        QVERIFY(!saved.empty());
        QCOMPARE(saved.rows, source.rows);
        QCOMPARE(saved.cols, source.cols);
        cv::Mat difference;
        cv::absdiff(saved, source, difference);
        QCOMPARE(cv::countNonZero(difference.reshape(1)), 0);

        const QString badTarget =
            QDir(m_appDataDir.path()).filePath(QStringLiteral("missing-parent/g8-save-image-output.invalid"));
        QVERIFY(QFile::remove(outputPath));
        QCOMPARE(configureSave(badTarget).value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
        pending = controller.handleToolCall(QStringLiteral("run_flow"),
                                            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const RunResult failedResult = qvariant_cast<RunResult>(runFinished.takeFirst().at(0));
        QVERIFY(!failedResult.success);
        QCOMPARE(failedResult.failedModule, saveId);
        QVERIFY(!failedResult.errorMessage.isEmpty());
        QVERIFY(!QFileInfo::exists(QFileInfo(badTarget).absolutePath()));

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }

    void testAgentOverlaysDisplayDataAndAttributesMissingImageFailure() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
        const QString sourcePath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-display-source.png"));
        QVERIFY(cv::imwrite(sourcePath.toStdString(), cv::Mat(80, 100, CV_8UC3, cv::Scalar(60, 60, 60))));

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 DisplayData")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        for (const QString& plugin : {QStringLiteral("GrabImage"), QStringLiteral("DisplayData")}) {
            const QString status =
                actor.executeTool(QStringLiteral("load_module_plugin"), QJsonObject{{QStringLiteral("plugin"), plugin}})
                    .value(QStringLiteral("status"))
                    .toString();
            QVERIFY(status == QLatin1String("loaded") || status == QLatin1String("already_loaded"));
        }
        const QJsonObject grab =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.grabimage")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_display_source")}});
        const QJsonObject display =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.displaydata")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_display_overlay")}});
        QCOMPARE(grab.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        QCOMPARE(display.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString grabId = grab.value(QStringLiteral("instanceId")).toString();
        const QString displayId = display.value(QStringLiteral("instanceId")).toString();
        QCOMPARE(setParams(actor, grabId,
                           QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Path")},
                                       {QStringLiteral("filePath"), sourcePath}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        QCOMPARE(setParams(actor, displayId,
                           QJsonObject{{QStringLiteral("displayText"), QStringLiteral("lot=42\nOK")},
                                       {QStringLiteral("fontSize"), 20},
                                       {QStringLiteral("positionX"), 5},
                                       {QStringLiteral("positionY"), 24}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        QCOMPARE(actor
                     .executeTool(QStringLiteral("connect_modules"),
                                  QJsonObject{{QStringLiteral("fromId"), grabId},
                                              {QStringLiteral("toId"), displayId},
                                              {QStringLiteral("fromPort"), QStringLiteral("image")},
                                              {QStringLiteral("toPort"), QStringLiteral("image")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("connected"));

        QJsonObject result = runToTerminal(actor);
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = result.value(QStringLiteral("runId")).toString();
        const QJsonObject textOutput =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{QStringLiteral("runId"), runId},
                                          {QStringLiteral("instanceId"), displayId},
                                          {QStringLiteral("key"), QStringLiteral("displayed_text")}});
        QCOMPARE(textOutput.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(textOutput.value(QStringLiteral("value")).toString(), QStringLiteral("lot=42\nOK"));
        const QJsonObject lineCount =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{QStringLiteral("runId"), runId},
                                          {QStringLiteral("instanceId"), displayId},
                                          {QStringLiteral("key"), QStringLiteral("display_line_count")}});
        QCOMPARE(lineCount.value(QStringLiteral("value")).toInt(), 2);

        QCOMPARE(setEnabled(actor, grabId, false).value(QStringLiteral("status")).toString(),
                 QStringLiteral("updated"));
        result = runToTerminal(actor);
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QJsonObject error = result.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(error.value(QStringLiteral("moduleId")).toString(), displayId);
        QCOMPARE(error.value(QStringLiteral("message")).toString(), QStringLiteral("输入图像无效"));

        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }

    void testAgentRecognizesConfiguredColorAndReportsAbsentTarget() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
        const QString blankPath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-color-blank.png"));
        struct ColorCase {
            const char* targetColor;
            const char* fileName;
            cv::Scalar bgr;
        };
        const ColorCase colorCases[] = {
            {"红色", "g8-color-red-low.png", cv::Scalar(0, 0, 255)},
            {"绿色", "g8-color-green.png", cv::Scalar(0, 255, 0)},
            {"蓝色", "g8-color-blue.png", cv::Scalar(255, 0, 0)},
            {"黄色", "g8-color-yellow.png", cv::Scalar(0, 255, 255)},
            {"橙色", "g8-color-orange.png", cv::Scalar(0, 102, 255)},
            {"紫色", "g8-color-purple.png", cv::Scalar(255, 0, 255)},
            {"红色", "g8-color-red-wrap.png", cv::Scalar(42, 0, 255)},
        };
        for (const ColorCase& colorCase : colorCases) {
            cv::Mat source(100, 120, CV_8UC3, cv::Scalar(20, 20, 20));
            cv::rectangle(source, cv::Rect(20, 25, 30, 20), colorCase.bgr, cv::FILLED);
            const QString imagePath = QDir(m_appDataDir.path()).filePath(QString::fromLatin1(colorCase.fileName));
            QVERIFY(cv::imwrite(imagePath.toStdString(), source));
        }
        QVERIFY(cv::imwrite(blankPath.toStdString(), cv::Mat(100, 120, CV_8UC3, cv::Scalar(255, 255, 255))));

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 ColorRecognition")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        for (const QString& plugin : {QStringLiteral("GrabImage"), QStringLiteral("ColorRecognition")}) {
            const QString status =
                actor.executeTool(QStringLiteral("load_module_plugin"), QJsonObject{{QStringLiteral("plugin"), plugin}})
                    .value(QStringLiteral("status"))
                    .toString();
            QVERIFY(status == QLatin1String("loaded") || status == QLatin1String("already_loaded"));
        }
        const QJsonObject grab =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.grabimage")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_color_source")}});
        const QJsonObject recognize = actor.executeTool(
            QStringLiteral("add_module"),
            QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.colorrecognition")},
                        {QStringLiteral("instanceName"), QStringLiteral("g8_color_recognition")}});
        QCOMPARE(grab.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        QCOMPARE(recognize.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString grabId = grab.value(QStringLiteral("instanceId")).toString();
        const QString recognizeId = recognize.value(QStringLiteral("instanceId")).toString();
        const auto configureGrab = [&](const QString& path) {
            return setParams(actor, grabId,
                             QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Path")},
                                         {QStringLiteral("filePath"), path}});
        };
        QCOMPARE(actor
                     .executeTool(QStringLiteral("connect_modules"),
                                  QJsonObject{{QStringLiteral("fromId"), grabId},
                                              {QStringLiteral("toId"), recognizeId},
                                              {QStringLiteral("fromPort"), QStringLiteral("image")},
                                              {QStringLiteral("toPort"), QStringLiteral("image")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("connected"));

        const auto parameterUpdateAccepted = [](const QString& status) {
            return status == QLatin1String("params_set") || status == QLatin1String("no_op");
        };
        QJsonObject result;
        for (const ColorCase& colorCase : colorCases) {
            const QString sourcePath = QDir(m_appDataDir.path()).filePath(QString::fromLatin1(colorCase.fileName));
            QVERIFY(parameterUpdateAccepted(configureGrab(sourcePath).value(QStringLiteral("status")).toString()));
            QVERIFY(parameterUpdateAccepted(
                setParams(actor, recognizeId,
                          QJsonObject{{QStringLiteral("targetColor"), QString::fromUtf8(colorCase.targetColor)}})
                    .value(QStringLiteral("status"))
                    .toString()));

            result = runToTerminal(actor);
            QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
            const QString runId = result.value(QStringLiteral("runId")).toString();
            const auto output = [&](const QString& instanceId, const QString& key) {
                return actor.executeTool(QStringLiteral("get_module_output"),
                                         QJsonObject{{QStringLiteral("runId"), runId},
                                                     {QStringLiteral("instanceId"), instanceId},
                                                     {QStringLiteral("key"), key}});
            };
            const QJsonObject sourceOutput = output(grabId, QStringLiteral("source_path"));
            QCOMPARE(sourceOutput.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(sourceOutput.value(QStringLiteral("value")).toString(), QFileInfo(sourcePath).absoluteFilePath());
            const QJsonObject colorName = output(recognizeId, QStringLiteral("color_name"));
            QCOMPARE(colorName.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(colorName.value(QStringLiteral("value")).toString(), QString::fromUtf8(colorCase.targetColor));
            const QJsonObject colorCount = output(recognizeId, QStringLiteral("color_count"));
            QCOMPARE(colorCount.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(colorCount.value(QStringLiteral("value")).toInt(), 1);
            const QJsonObject colorArea = output(recognizeId, QStringLiteral("color_area"));
            QCOMPARE(colorArea.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QVERIFY(colorArea.value(QStringLiteral("value")).toDouble() > 500.0);
            const QJsonObject centerX = output(recognizeId, QStringLiteral("color_center_x"));
            const QJsonObject centerY = output(recognizeId, QStringLiteral("color_center_y"));
            QCOMPARE(centerX.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(centerY.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QVERIFY(qAbs(centerX.value(QStringLiteral("value")).toInt() - 34) <= 1);
            QVERIFY(qAbs(centerY.value(QStringLiteral("value")).toInt() - 34) <= 1);
        }

        QCOMPARE(configureGrab(blankPath).value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
        result = runToTerminal(actor);
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QJsonObject error = result.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(error.value(QStringLiteral("moduleId")).toString(), recognizeId);
        QCOMPARE(error.value(QStringLiteral("message")).toString(), QStringLiteral("未检测到目标颜色"));

        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }

    void testAgentLoadsGrabImagePathDemoAndFolderModes() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
        const QString sourcePath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-grab-source.png"));
        const QString missingPath = QDir(m_appDataDir.path()).filePath(QStringLiteral("missing-g8-image.png"));
        QVERIFY(cv::imwrite(sourcePath.toStdString(), cv::Mat(37, 59, CV_8UC3, cv::Scalar(15, 90, 175))));

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 GrabImage")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        const QString loadStatus =
            actor
                .executeTool(QStringLiteral("load_module_plugin"),
                             QJsonObject{{QStringLiteral("plugin"), QStringLiteral("GrabImage")}})
                .value(QStringLiteral("status"))
                .toString();
        QVERIFY(loadStatus == QLatin1String("loaded") || loadStatus == QLatin1String("already_loaded"));
        const QJsonObject grab =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.grabimage")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_grab_image_path")}});
        QCOMPARE(grab.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString instanceId = grab.value(QStringLiteral("instanceId")).toString();
        const auto configurePath = [&](const QString& path) {
            return setParams(actor, instanceId,
                             QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Path")},
                                         {QStringLiteral("filePath"), path}});
        };
        QCOMPARE(configurePath(sourcePath).value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
        QJsonObject result = runToTerminal(actor);
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = result.value(QStringLiteral("runId")).toString();
        const auto output = [&](const QString& key) {
            return actor.executeTool(QStringLiteral("get_module_output"),
                                     QJsonObject{{QStringLiteral("runId"), runId},
                                                 {QStringLiteral("instanceId"), instanceId},
                                                 {QStringLiteral("key"), key}});
        };
        QCOMPARE(output(QStringLiteral("source_path")).value(QStringLiteral("value")).toString(),
                 QFileInfo(sourcePath).absoluteFilePath());
        QCOMPARE(output(QStringLiteral("source_file_name")).value(QStringLiteral("value")).toString(),
                 QStringLiteral("g8-grab-source.png"));

        QCOMPARE(configurePath(missingPath).value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
        result = runToTerminal(actor);
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QJsonObject error = result.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(error.value(QStringLiteral("moduleId")).toString(), instanceId);
        QCOMPARE(error.value(QStringLiteral("message")).toString(), QStringLiteral("文件不存在: %1").arg(missingPath));

        QCOMPARE(setParams(actor, instanceId, QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Demo")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject demoSchema = actor.executeTool(QStringLiteral("get_module_params_schema"),
                                                         QJsonObject{{QStringLiteral("instanceId"), instanceId}});
        QCOMPARE(demoSchema.value(QStringLiteral("effectiveParams"))
                     .toObject()
                     .value(QStringLiteral("grabSource"))
                     .toString(),
                 QStringLiteral("Demo"));
        const QJsonObject demoStep = actor.executeTool(QStringLiteral("step_once"), QJsonObject());
        QCOMPARE(demoStep.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        const QString demoRunId = demoStep.value(QStringLiteral("runId")).toString();
        QVERIFY(!demoRunId.isEmpty());
        QCOMPARE(waitForRun(actor, demoRunId).value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QJsonObject demoImageSummary = actor.executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), demoRunId},
                                                             {QStringLiteral("instanceId"), instanceId},
                                                             {QStringLiteral("port"), QStringLiteral("image")}});
        QCOMPARE(demoImageSummary.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        const QJsonObject demoImage = demoImageSummary.value(QStringLiteral("value")).toObject();
        QCOMPARE(demoImage.value(QStringLiteral("width")).toInt(), 640);
        QCOMPARE(demoImage.value(QStringLiteral("height")).toInt(), 480);
        ImageData demoSnapshot;
        QMap<QString, QVariant> demoNamedValues;
        PortValueMap demoPorts;
        QVERIFY(
            RunEngine::instance().runOutputSnapshot(demoRunId, instanceId, demoSnapshot, demoNamedValues, demoPorts));
        const cv::Mat demoPixels = demoSnapshot.toMat();
        QCOMPARE(demoPixels.cols, 640);
        QCOMPARE(demoPixels.rows, 480);
        QCOMPARE(demoPixels.channels(), 1);
        QCOMPARE(demoPixels.at<uchar>(0, 0), static_cast<uchar>(0));
        QCOMPARE(demoPixels.at<uchar>(0, 40), static_cast<uchar>(255));
        QCOMPARE(demoPixels.at<uchar>(40, 0), static_cast<uchar>(255));
        QCOMPARE(demoPixels.at<uchar>(40, 40), static_cast<uchar>(0));

        const QString folderPath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-grab-folder"));
        QVERIFY(QDir().mkpath(folderPath));
        const QString firstPath = QDir(folderPath).filePath(QStringLiteral("a.png"));
        const QString secondPath = QDir(folderPath).filePath(QStringLiteral("b.png"));
        QVERIFY(cv::imwrite(firstPath.toStdString(), cv::Mat(9, 13, CV_8UC1, cv::Scalar(31))));
        QVERIFY(cv::imwrite(secondPath.toStdString(), cv::Mat(11, 17, CV_8UC1, cv::Scalar(97))));
        QFile unsupportedFile(QDir(folderPath).filePath(QStringLiteral("ignored.txt")));
        QVERIFY(unsupportedFile.open(QIODevice::WriteOnly));
        QCOMPARE(unsupportedFile.write("not an image"), qint64(12));
        unsupportedFile.close();
        QCOMPARE(setParams(actor, instanceId,
                           QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Path")},
                                       {QStringLiteral("filePath"), folderPath},
                                       {QStringLiteral("folderLoop"), false}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject folderSchema = actor.executeTool(QStringLiteral("get_module_params_schema"),
                                                           QJsonObject{{QStringLiteral("instanceId"), instanceId}});
        const QJsonObject effectiveFolderParams = folderSchema.value(QStringLiteral("effectiveParams")).toObject();
        QCOMPARE(effectiveFolderParams.value(QStringLiteral("grabSource")).toString(), QStringLiteral("Path"));
        QCOMPARE(effectiveFolderParams.value(QStringLiteral("filePath")).toString(), folderPath);
        QCOMPARE(effectiveFolderParams.value(QStringLiteral("folderLoop")).toBool(), false);

        ModuleBase* folderRuntime = nullptr;
        for (int position = 1; position <= 2; ++position) {
            const QJsonObject folderStep = actor.executeTool(QStringLiteral("step_once"), QJsonObject());
            QCOMPARE(folderStep.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
            ModuleBase* const currentRuntime = RunEngine::instance().getModule(instanceId);
            QVERIFY(currentRuntime);
            if (position == 1)
                folderRuntime = currentRuntime;
            else
                QCOMPARE(currentRuntime, folderRuntime);
            const QString folderRunId = folderStep.value(QStringLiteral("runId")).toString();
            QVERIFY(!folderRunId.isEmpty());
            QCOMPARE(waitForRun(actor, folderRunId).value(QStringLiteral("status")).toString(),
                     QStringLiteral("completed"));

            const QString expectedPath = position == 1 ? firstPath : secondPath;
            const QString expectedName = position == 1 ? QStringLiteral("a.png") : QStringLiteral("b.png");
            const int expectedWidth = position == 1 ? 13 : 17;
            const int expectedHeight = position == 1 ? 9 : 11;
            const int expectedPixel = position == 1 ? 31 : 97;
            const auto output = [&](const QString& key) {
                return actor.executeTool(QStringLiteral("get_module_output"),
                                         QJsonObject{{QStringLiteral("runId"), folderRunId},
                                                     {QStringLiteral("instanceId"), instanceId},
                                                     {QStringLiteral("key"), key}});
            };
            QCOMPARE(output(QStringLiteral("source_path")).value(QStringLiteral("value")).toString(),
                     QFileInfo(expectedPath).absoluteFilePath());
            QCOMPARE(output(QStringLiteral("source_file_name")).value(QStringLiteral("value")).toString(),
                     expectedName);
            QCOMPARE(output(QStringLiteral("folder_path")).value(QStringLiteral("value")).toString(),
                     QFileInfo(folderPath).absoluteFilePath());
            QCOMPARE(output(QStringLiteral("folder_position")).value(QStringLiteral("value")).toInt(), position);
            QCOMPARE(output(QStringLiteral("folder_count")).value(QStringLiteral("value")).toInt(), 2);
            const QJsonObject folderImage = actor.executeTool(
                QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), folderRunId},
                                                                 {QStringLiteral("instanceId"), instanceId},
                                                                 {QStringLiteral("port"), QStringLiteral("image")}});
            QCOMPARE(folderImage.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            const QJsonObject imageSummary = folderImage.value(QStringLiteral("value")).toObject();
            QCOMPARE(imageSummary.value(QStringLiteral("width")).toInt(), expectedWidth);
            QCOMPARE(imageSummary.value(QStringLiteral("height")).toInt(), expectedHeight);
            ImageData folderSnapshot;
            QMap<QString, QVariant> folderNamedValues;
            PortValueMap folderPorts;
            QVERIFY(RunEngine::instance().runOutputSnapshot(folderRunId, instanceId, folderSnapshot, folderNamedValues,
                                                            folderPorts));
            const cv::Mat pixels = folderSnapshot.toMat();
            QCOMPARE(pixels.cols, expectedWidth);
            QCOMPARE(pixels.rows, expectedHeight);
            QCOMPARE(pixels.at<uchar>(0, 0), static_cast<uchar>(expectedPixel));
        }

        const QJsonObject exhaustedStep = actor.executeTool(QStringLiteral("step_once"), QJsonObject());
        QCOMPARE(exhaustedStep.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        QCOMPARE(RunEngine::instance().getModule(instanceId), folderRuntime);
        const QString exhaustedRunId = exhaustedStep.value(QStringLiteral("runId")).toString();
        QVERIFY(!exhaustedRunId.isEmpty());
        const QJsonObject exhaustedRun = waitForRun(actor, exhaustedRunId);
        QCOMPARE(exhaustedRun.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QJsonObject exhaustedError = exhaustedRun.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(exhaustedError.value(QStringLiteral("moduleId")).toString(), instanceId);
        QCOMPARE(exhaustedError.value(QStringLiteral("message")).toString(), QStringLiteral("文件夹图像已读取完毕"));
        const QJsonObject failedOutput =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{QStringLiteral("runId"), exhaustedRunId},
                                          {QStringLiteral("instanceId"), instanceId},
                                          {QStringLiteral("key"), QStringLiteral("folder_position")}});
        QCOMPARE(failedOutput.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
        QCOMPARE(failedOutput.value(QStringLiteral("reason")).toString(), QStringLiteral("run_failed"));

        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }

    void testAgentDetectsConfiguredBlobAndReportsEmptyDetection() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
        const QString otsuPath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-blob-otsu.png"));
        const QString fixedPath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-blob-fixed.png"));
        const QString adaptivePath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-blob-adaptive.png"));
        const QString blankPath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-blob-blank.png"));
        cv::Mat otsuSource(100, 120, CV_8UC1, cv::Scalar(20));
        cv::circle(otsuSource, cv::Point(35, 40), 15, cv::Scalar(100), cv::FILLED);
        QVERIFY(cv::imwrite(otsuPath.toStdString(), otsuSource));
        cv::Mat gradient(100, 120, CV_8UC1);
        for (int x = 0; x < gradient.cols; ++x) {
            const uchar value = cv::saturate_cast<uchar>(20.0 + 160.0 * x / 119.0);
            gradient.col(x).setTo(cv::Scalar(value));
        }
        cv::Mat fixedSource = gradient.clone();
        cv::circle(fixedSource, cv::Point(15, 50), 5, cv::Scalar(80), cv::FILLED);
        QVERIFY(cv::imwrite(fixedPath.toStdString(), fixedSource));
        cv::Mat adaptiveSource = gradient.clone();
        cv::circle(adaptiveSource, cv::Point(15, 50), 5, cv::Scalar(80), cv::FILLED);
        QVERIFY(cv::imwrite(adaptivePath.toStdString(), adaptiveSource));
        QVERIFY(cv::imwrite(blankPath.toStdString(), cv::Mat(100, 120, CV_8UC1, cv::Scalar(20))));

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 Blob")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        for (const QString& plugin : {QStringLiteral("GrabImage"), QStringLiteral("Blob")}) {
            const QString status =
                actor.executeTool(QStringLiteral("load_module_plugin"), QJsonObject{{QStringLiteral("plugin"), plugin}})
                    .value(QStringLiteral("status"))
                    .toString();
            QVERIFY(status == QLatin1String("loaded") || status == QLatin1String("already_loaded"));
        }
        const QJsonObject grab =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.grabimage")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_blob_source")}});
        const QJsonObject blob =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.blob")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_blob_detection")}});
        QCOMPARE(grab.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        QCOMPARE(blob.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString grabId = grab.value(QStringLiteral("instanceId")).toString();
        const QString blobId = blob.value(QStringLiteral("instanceId")).toString();
        const auto configureGrab = [&](const QString& path) {
            return setParams(actor, grabId,
                             QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Path")},
                                         {QStringLiteral("filePath"), path}});
        };
        QCOMPARE(actor
                     .executeTool(QStringLiteral("connect_modules"),
                                  QJsonObject{{QStringLiteral("fromId"), grabId},
                                              {QStringLiteral("toId"), blobId},
                                              {QStringLiteral("fromPort"), QStringLiteral("image")},
                                              {QStringLiteral("toPort"), QStringLiteral("image")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("connected"));

        const auto parameterUpdateAccepted = [](const QString& status) {
            return status == QLatin1String("params_set") || status == QLatin1String("no_op");
        };
        struct ThresholdCase {
            const char* thresholdType;
            const char* filePath;
            int fixedThreshold;
            int adaptiveBlockSize;
            int adaptiveC;
            double minArea;
            double expectedCenterX;
            double expectedCenterY;
        };
        const ThresholdCase thresholdCases[] = {
            {"Otsu", "g8-blob-otsu.png", 127, 11, 2, 600.0, 35.0, 40.0},
            {"Fixed", "g8-blob-fixed.png", 60, 11, 2, 50.0, 15.0, 50.0},
            {"Adaptive", "g8-blob-adaptive.png", 90, 11, -10, 50.0, 15.0, 50.0},
        };
        QJsonObject result;
        for (const ThresholdCase& thresholdCase : thresholdCases) {
            const QString sourcePath = QDir(m_appDataDir.path()).filePath(QString::fromLatin1(thresholdCase.filePath));
            QVERIFY(parameterUpdateAccepted(configureGrab(sourcePath).value(QStringLiteral("status")).toString()));
            QVERIFY(parameterUpdateAccepted(
                setParams(
                    actor, blobId,
                    QJsonObject{{QStringLiteral("minArea"), 10.0},
                                {QStringLiteral("maxArea"), 1000.0},
                                {QStringLiteral("minCircularity"), 0.5},
                                {QStringLiteral("thresholdType"), QString::fromLatin1(thresholdCase.thresholdType)},
                                {QStringLiteral("fixedThreshold"), thresholdCase.fixedThreshold},
                                {QStringLiteral("adaptiveBlockSize"), thresholdCase.adaptiveBlockSize},
                                {QStringLiteral("adaptiveC"), thresholdCase.adaptiveC}})
                    .value(QStringLiteral("status"))
                    .toString()));

            result = runToTerminal(actor);
            QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
            const QString runId = result.value(QStringLiteral("runId")).toString();
            const auto output = [&](const QString& instanceId, const QString& key) {
                return actor.executeTool(QStringLiteral("get_module_output"),
                                         QJsonObject{{QStringLiteral("runId"), runId},
                                                     {QStringLiteral("instanceId"), instanceId},
                                                     {QStringLiteral("key"), key}});
            };
            const QJsonObject sourceOutput = output(grabId, QStringLiteral("source_path"));
            QCOMPARE(sourceOutput.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(sourceOutput.value(QStringLiteral("value")).toString(), QFileInfo(sourcePath).absoluteFilePath());
            const QJsonObject blobCount = output(blobId, QStringLiteral("blob_count"));
            QCOMPARE(blobCount.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(blobCount.value(QStringLiteral("value")).toInt(), 1);
            const QJsonObject blobArea = output(blobId, QStringLiteral("blob_area"));
            QCOMPARE(blobArea.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QVERIFY(blobArea.value(QStringLiteral("value")).toDouble() > thresholdCase.minArea);
            const QJsonObject centerX = output(blobId, QStringLiteral("blob_center_x"));
            const QJsonObject centerY = output(blobId, QStringLiteral("blob_center_y"));
            QCOMPARE(centerX.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(centerY.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QVERIFY(qAbs(centerX.value(QStringLiteral("value")).toDouble() - thresholdCase.expectedCenterX) <= 1.0);
            QVERIFY(qAbs(centerY.value(QStringLiteral("value")).toDouble() - thresholdCase.expectedCenterY) <= 1.0);
        }

        QCOMPARE(configureGrab(blankPath).value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
        result = runToTerminal(actor);
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QJsonObject error = result.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(error.value(QStringLiteral("moduleId")).toString(), blobId);
        QCOMPARE(error.value(QStringLiteral("message")).toString(), QStringLiteral("未检测到Blob"));

        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }

    void testAgentFindsConfiguredCircleAndReportsNoCircle() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
        const QString sourcePath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-circle-source.png"));
        const QString blankPath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-circle-blank.png"));
        cv::Mat source(200, 200, CV_8UC3, cv::Scalar(0, 0, 0));
        cv::circle(source, cv::Point(100, 100), 50, cv::Scalar(255, 255, 255), cv::FILLED);
        QVERIFY(cv::imwrite(sourcePath.toStdString(), source));
        QVERIFY(cv::imwrite(blankPath.toStdString(), cv::Mat(200, 200, CV_8UC3, cv::Scalar(0, 0, 0))));

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 FindCircle")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        for (const QString& plugin : {QStringLiteral("GrabImage"), QStringLiteral("FindCircle")}) {
            const QString status =
                actor.executeTool(QStringLiteral("load_module_plugin"), QJsonObject{{QStringLiteral("plugin"), plugin}})
                    .value(QStringLiteral("status"))
                    .toString();
            QVERIFY(status == QLatin1String("loaded") || status == QLatin1String("already_loaded"));
        }
        const QJsonObject grab =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.grabimage")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_circle_source")}});
        const QJsonObject circle =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.findcircle")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_circle_detection")}});
        QCOMPARE(grab.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        QCOMPARE(circle.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString grabId = grab.value(QStringLiteral("instanceId")).toString();
        const QString circleId = circle.value(QStringLiteral("instanceId")).toString();
        const auto configureGrab = [&](const QString& path) {
            return setParams(actor, grabId,
                             QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Path")},
                                         {QStringLiteral("filePath"), path}});
        };
        QCOMPARE(configureGrab(sourcePath).value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
        QCOMPARE(setParams(actor, circleId,
                           QJsonObject{{QStringLiteral("minRadius"), 25.0},
                                       {QStringLiteral("maxRadius"), 100.0},
                                       {QStringLiteral("param1"), 50.0},
                                       {QStringLiteral("param2"), 30.0}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        QCOMPARE(actor
                     .executeTool(QStringLiteral("connect_modules"),
                                  QJsonObject{{QStringLiteral("fromId"), grabId},
                                              {QStringLiteral("toId"), circleId},
                                              {QStringLiteral("fromPort"), QStringLiteral("image")},
                                              {QStringLiteral("toPort"), QStringLiteral("image")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("connected"));

        QJsonObject result = runToTerminal(actor);
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = result.value(QStringLiteral("runId")).toString();
        const auto output = [&](const QString& key) {
            return actor.executeTool(QStringLiteral("get_module_output"),
                                     QJsonObject{{QStringLiteral("runId"), runId},
                                                 {QStringLiteral("instanceId"), circleId},
                                                 {QStringLiteral("key"), key}});
        };
        const double centerX = output(QStringLiteral("circle_center_x")).value(QStringLiteral("value")).toDouble();
        const double centerY = output(QStringLiteral("circle_center_y")).value(QStringLiteral("value")).toDouble();
        const double radius = output(QStringLiteral("circle_radius")).value(QStringLiteral("value")).toDouble();
        QVERIFY(qAbs(centerX - 100.0) < 20.0);
        QVERIFY(qAbs(centerY - 100.0) < 20.0);
        QVERIFY(qAbs(radius - 50.0) < 20.0);
        QVERIFY(output(QStringLiteral("circle")).value(QStringLiteral("status")).toString() ==
                QLatin1String("available"));

        QCOMPARE(configureGrab(blankPath).value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
        result = runToTerminal(actor);
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QJsonObject error = result.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(error.value(QStringLiteral("moduleId")).toString(), circleId);
        QCOMPARE(error.value(QStringLiteral("message")).toString(), QStringLiteral("未检测到圆"));

        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }

    void testAgentMeasuresConfiguredCircleAndReportsInsufficientEdges() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
        const QString sourcePath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-measure-circle-source.png"));
        const QString blankPath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-measure-circle-blank.png"));
        cv::Mat source(200, 200, CV_8UC1, cv::Scalar(0));
        cv::circle(source, cv::Point(100, 100), 50, cv::Scalar(255), cv::FILLED);
        QVERIFY(cv::imwrite(sourcePath.toStdString(), source));
        QVERIFY(cv::imwrite(blankPath.toStdString(), cv::Mat(200, 200, CV_8UC1, cv::Scalar(0))));

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 MeasureCircle")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        for (const QString& plugin : {QStringLiteral("GrabImage"), QStringLiteral("MeasureCircle")}) {
            const QString status =
                actor.executeTool(QStringLiteral("load_module_plugin"), QJsonObject{{QStringLiteral("plugin"), plugin}})
                    .value(QStringLiteral("status"))
                    .toString();
            QVERIFY(status == QLatin1String("loaded") || status == QLatin1String("already_loaded"));
        }
        const QJsonObject grab = actor.executeTool(
            QStringLiteral("add_module"),
            QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.grabimage")},
                        {QStringLiteral("instanceName"), QStringLiteral("g8_measure_circle_source")}});
        const QJsonObject measure = actor.executeTool(
            QStringLiteral("add_module"),
            QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.measurecircle")},
                        {QStringLiteral("instanceName"), QStringLiteral("g8_measure_circle")}});
        QCOMPARE(grab.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        QCOMPARE(measure.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString grabId = grab.value(QStringLiteral("instanceId")).toString();
        const QString measureId = measure.value(QStringLiteral("instanceId")).toString();
        const auto configureGrab = [&](const QString& path) {
            return setParams(actor, grabId,
                             QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Path")},
                                         {QStringLiteral("filePath"), path}});
        };
        QCOMPARE(configureGrab(sourcePath).value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
        QCOMPARE(setParams(actor, measureId,
                           QJsonObject{{QStringLiteral("initialCenterX"), 100.0},
                                       {QStringLiteral("initialCenterY"), 100.0},
                                       {QStringLiteral("initialRadius"), 50.0},
                                       {QStringLiteral("threshold"), 20.0},
                                       {QStringLiteral("measureCount"), 36},
                                       {QStringLiteral("searchLength"), 10.0},
                                       {QStringLiteral("exclusionRadius"), 0.0}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        QCOMPARE(actor
                     .executeTool(QStringLiteral("connect_modules"),
                                  QJsonObject{{QStringLiteral("fromId"), grabId},
                                              {QStringLiteral("toId"), measureId},
                                              {QStringLiteral("fromPort"), QStringLiteral("image")},
                                              {QStringLiteral("toPort"), QStringLiteral("image")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("connected"));

        QJsonObject result = runToTerminal(actor);
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = result.value(QStringLiteral("runId")).toString();
        const auto output = [&](const QString& key) {
            return actor.executeTool(QStringLiteral("get_module_output"),
                                     QJsonObject{{QStringLiteral("runId"), runId},
                                                 {QStringLiteral("instanceId"), measureId},
                                                 {QStringLiteral("key"), key}});
        };
        QCOMPARE(output(QStringLiteral("edge_point_count")).value(QStringLiteral("value")).toInt(), 36);
        QVERIFY(qAbs(output(QStringLiteral("circle_center_x")).value(QStringLiteral("value")).toDouble() - 100.0) <
                2.0);
        QVERIFY(qAbs(output(QStringLiteral("circle_center_y")).value(QStringLiteral("value")).toDouble() - 100.0) <
                2.0);
        QVERIFY(qAbs(output(QStringLiteral("circle_radius")).value(QStringLiteral("value")).toDouble() - 50.0) < 2.0);

        QCOMPARE(configureGrab(blankPath).value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
        result = runToTerminal(actor);
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QJsonObject error = result.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(error.value(QStringLiteral("moduleId")).toString(), measureId);
        QCOMPARE(error.value(QStringLiteral("message")).toString(), QStringLiteral("卡钳提取边缘点不足，无法拟合圆"));

        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }

    void testAgentSuppliesPointPairAndReportsDimensionMismatch() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 MeasurementInput")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        const QString status =
            actor
                .executeTool(QStringLiteral("load_module_plugin"),
                             QJsonObject{{QStringLiteral("plugin"), QStringLiteral("MeasurementInput")}})
                .value(QStringLiteral("status"))
                .toString();
        QVERIFY(status == QLatin1String("loaded") || status == QLatin1String("already_loaded"));
        const QJsonObject measurement = actor.executeTool(
            QStringLiteral("add_module"),
            QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.measurementinput")},
                        {QStringLiteral("instanceName"), QStringLiteral("g8_measurement_points")}});
        QCOMPARE(measurement.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString measurementId = measurement.value(QStringLiteral("instanceId")).toString();
        const QJsonArray point1{12.5, 20.0};
        const QJsonArray point2{3.25, 4.0};
        QCOMPARE(setParams(actor, measurementId,
                           QJsonObject{{QStringLiteral("mode"), QStringLiteral("point_pair")},
                                       {QStringLiteral("point1"), point1},
                                       {QStringLiteral("point2"), point2}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        QJsonObject result = runToTerminal(actor);
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = result.value(QStringLiteral("runId")).toString();
        const auto output = [&](const QString& key) {
            return actor.executeTool(QStringLiteral("get_module_output"),
                                     QJsonObject{{QStringLiteral("runId"), runId},
                                                 {QStringLiteral("instanceId"), measurementId},
                                                 {QStringLiteral("key"), key}});
        };
        QCOMPARE(output(QStringLiteral("measurement_input_mode")).value(QStringLiteral("value")).toString(),
                 QStringLiteral("point_pair"));
        QCOMPARE(output(QStringLiteral("point1")).value(QStringLiteral("value")).toArray(), point1);
        QCOMPARE(output(QStringLiteral("point2")).value(QStringLiteral("value")).toArray(), point2);

        const QJsonArray customPoint1{1.0, 2.0, 3.0};
        const QJsonArray customPoint2{4.0, 5.0, 6.0};
        const QJsonArray customPoint{7.0, 8.0, 9.0};
        const QJsonArray customLine{10.0, 11.0, 12.0, 13.0};
        const QJsonArray customLine1{14.0, 15.0, 16.0, 17.0};
        const QJsonArray customLine2{18.0, 19.0, 20.0, 21.0};
        const QJsonArray customPlane{0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
        QCOMPARE(setParams(actor, measurementId,
                           QJsonObject{{QStringLiteral("mode"), QStringLiteral("custom")},
                                       {QStringLiteral("point1"), customPoint1},
                                       {QStringLiteral("point2"), customPoint2},
                                       {QStringLiteral("point"), customPoint},
                                       {QStringLiteral("line"), customLine},
                                       {QStringLiteral("line1"), customLine1},
                                       {QStringLiteral("line2"), customLine2},
                                       {QStringLiteral("plane"), customPlane}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject customResult = runToTerminal(actor);
        QCOMPARE(customResult.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString customRunId = customResult.value(QStringLiteral("runId")).toString();
        QVERIFY(!customRunId.isEmpty());
        const auto customOutput = [&](const QString& key) {
            return actor.executeTool(QStringLiteral("get_module_output"),
                                     QJsonObject{{QStringLiteral("runId"), customRunId},
                                                 {QStringLiteral("instanceId"), measurementId},
                                                 {QStringLiteral("key"), key}});
        };
        for (const auto& expected :
             {qMakePair(QStringLiteral("point1"), customPoint1), qMakePair(QStringLiteral("point2"), customPoint2),
              qMakePair(QStringLiteral("point"), customPoint), qMakePair(QStringLiteral("line"), customLine),
              qMakePair(QStringLiteral("line1"), customLine1), qMakePair(QStringLiteral("line2"), customLine2),
              qMakePair(QStringLiteral("plane"), customPlane)}) {
            const QJsonObject byKey = customOutput(expected.first);
            const QJsonObject byPort = actor.executeTool(QStringLiteral("get_module_output"),
                                                         QJsonObject{{QStringLiteral("runId"), customRunId},
                                                                     {QStringLiteral("instanceId"), measurementId},
                                                                     {QStringLiteral("port"), expected.first}});
            QCOMPARE(byKey.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(byPort.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(byKey.value(QStringLiteral("value")).toArray(), expected.second);
            QCOMPARE(byPort.value(QStringLiteral("value")).toArray(), expected.second);
        }
        QCOMPARE(customOutput(QStringLiteral("measurement_input_mode")).value(QStringLiteral("value")).toString(),
                 QStringLiteral("custom"));

        QCOMPARE(setParams(actor, measurementId,
                           QJsonObject{{QStringLiteral("mode"), QStringLiteral("point_pair")},
                                       {QStringLiteral("point1"), point1},
                                       {QStringLiteral("point2"), QJsonArray{1.0, 2.0, 3.0}}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        result = runToTerminal(actor);
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QString failedRunId = result.value(QStringLiteral("runId")).toString();
        QVERIFY(!failedRunId.isEmpty());
        const QJsonObject error = result.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(error.value(QStringLiteral("moduleId")).toString(), measurementId);
        QCOMPARE(error.value(QStringLiteral("message")).toString(), QStringLiteral("两点坐标维数必须一致"));
        for (const QString& key : {QStringLiteral("point1"), QStringLiteral("point2"), QStringLiteral("point"),
                                   QStringLiteral("line"), QStringLiteral("line1"), QStringLiteral("line2"),
                                   QStringLiteral("plane"), QStringLiteral("measurement_input_mode")}) {
            const QJsonObject unavailable = actor.executeTool(QStringLiteral("get_module_output"),
                                                              QJsonObject{{QStringLiteral("runId"), failedRunId},
                                                                          {QStringLiteral("instanceId"), measurementId},
                                                                          {QStringLiteral("key"), key}});
            QCOMPARE(unavailable.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
            QVERIFY(!unavailable.contains(QStringLiteral("value")));
            if (key != QLatin1String("measurement_input_mode")) {
                const QJsonObject unavailablePort = actor.executeTool(
                    QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), failedRunId},
                                                                     {QStringLiteral("instanceId"), measurementId},
                                                                     {QStringLiteral("port"), key}});
                QCOMPARE(unavailablePort.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
                QVERIFY(!unavailablePort.contains(QStringLiteral("value")));
            }
        }

        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }

    void testAgentMeasuresPointPairAndValidatesRequiredDistanceInput() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 DistancePP")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        for (const QString& plugin : {QStringLiteral("MeasurementInput"), QStringLiteral("DistancePP")}) {
            const QString status =
                actor.executeTool(QStringLiteral("load_module_plugin"), QJsonObject{{QStringLiteral("plugin"), plugin}})
                    .value(QStringLiteral("status"))
                    .toString();
            QVERIFY(status == QLatin1String("loaded") || status == QLatin1String("already_loaded"));
        }

        const QJsonObject source = actor.executeTool(
            QStringLiteral("add_module"),
            QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.measurementinput")},
                        {QStringLiteral("instanceName"), QStringLiteral("g8_distance_points")}});
        QCOMPARE(source.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString sourceId = source.value(QStringLiteral("instanceId")).toString();
        const QJsonObject distance =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.distancepp")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_distance_pp")}});
        QCOMPARE(distance.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString distanceId = distance.value(QStringLiteral("instanceId")).toString();

        const QJsonArray point1{12.5, 20.0};
        const QJsonArray point2{3.25, 4.0};
        QCOMPARE(setParams(actor, sourceId,
                           QJsonObject{{QStringLiteral("mode"), QStringLiteral("point_pair")},
                                       {QStringLiteral("point1"), point1},
                                       {QStringLiteral("point2"), point2}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));

        for (const QString& port : {QStringLiteral("point1"), QStringLiteral("point2")}) {
            const QJsonObject connected =
                actor.executeTool(QStringLiteral("connect_modules"), QJsonObject{{QStringLiteral("fromId"), sourceId},
                                                                                 {QStringLiteral("toId"), distanceId},
                                                                                 {QStringLiteral("fromPort"), port},
                                                                                 {QStringLiteral("toPort"), port}});
            QCOMPARE(connected.value(QStringLiteral("status")).toString(), QStringLiteral("connected"));
        }

        const QJsonObject successRun = runToTerminal(actor);
        QCOMPARE(successRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = successRun.value(QStringLiteral("runId")).toString();
        const auto output = [&](const QString& key) {
            return actor.executeTool(QStringLiteral("get_module_output"),
                                     QJsonObject{{QStringLiteral("runId"), runId},
                                                 {QStringLiteral("instanceId"), distanceId},
                                                 {QStringLiteral("key"), key}});
        };
        const QJsonObject measuredDistance = output(QStringLiteral("distance"));
        QCOMPARE(measuredDistance.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QVERIFY(std::abs(measuredDistance.value(QStringLiteral("value")).toDouble() - std::sqrt(341.5625)) < 1e-9);
        const QJsonObject deltaX = output(QStringLiteral("delta_x"));
        QCOMPARE(deltaX.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(deltaX.value(QStringLiteral("value")).toDouble(), -9.25);
        const QJsonObject deltaY = output(QStringLiteral("delta_y"));
        QCOMPARE(deltaY.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(deltaY.value(QStringLiteral("value")).toDouble(), -16.0);

        const QJsonObject disconnected = actor.executeTool(
            QStringLiteral("disconnect_modules"), QJsonObject{{QStringLiteral("fromId"), sourceId},
                                                              {QStringLiteral("toId"), distanceId},
                                                              {QStringLiteral("fromPort"), QStringLiteral("point2")},
                                                              {QStringLiteral("toPort"), QStringLiteral("point2")}});
        QCOMPARE(disconnected.value(QStringLiteral("status")).toString(), QStringLiteral("disconnected"));
        const QJsonObject validation = actor.executeTool(QStringLiteral("validate_flow"), {});
        QCOMPARE(validation.value(QStringLiteral("status")).toString(), QStringLiteral("invalid"));
        QVERIFY(!validation.contains(QStringLiteral("runId")));
        const QJsonArray errors = validation.value(QStringLiteral("errors")).toArray();
        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.first().toObject().value(QStringLiteral("moduleId")).toString(), distanceId);
        QCOMPARE(errors.first().toObject().value(QStringLiteral("portId")).toString(), QStringLiteral("point2"));

        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }

    void testAgentMeasuresPointToLineAndValidatesRequiredLineInput() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 DistancePL")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        for (const QString& plugin : {QStringLiteral("MeasurementInput"), QStringLiteral("DistancePL")}) {
            const QString status =
                actor.executeTool(QStringLiteral("load_module_plugin"), QJsonObject{{QStringLiteral("plugin"), plugin}})
                    .value(QStringLiteral("status"))
                    .toString();
            QVERIFY(status == QLatin1String("loaded") || status == QLatin1String("already_loaded"));
        }

        const QJsonObject source = actor.executeTool(
            QStringLiteral("add_module"),
            QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.measurementinput")},
                        {QStringLiteral("instanceName"), QStringLiteral("g8_point_line")}});
        QCOMPARE(source.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString sourceId = source.value(QStringLiteral("instanceId")).toString();
        const QJsonObject distance =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.distancepl")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_distance_pl")}});
        QCOMPARE(distance.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString distanceId = distance.value(QStringLiteral("instanceId")).toString();

        QCOMPARE(setParams(actor, sourceId,
                           QJsonObject{{QStringLiteral("mode"), QStringLiteral("point_line")},
                                       {QStringLiteral("point"), QJsonArray{5.0, 12.0}},
                                       {QStringLiteral("line"), QJsonArray{0.0, 0.0, 10.0, 0.0}}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));

        for (const QString& port : {QStringLiteral("point"), QStringLiteral("line")}) {
            const QJsonObject connected =
                actor.executeTool(QStringLiteral("connect_modules"), QJsonObject{{QStringLiteral("fromId"), sourceId},
                                                                                 {QStringLiteral("toId"), distanceId},
                                                                                 {QStringLiteral("fromPort"), port},
                                                                                 {QStringLiteral("toPort"), port}});
            QCOMPARE(connected.value(QStringLiteral("status")).toString(), QStringLiteral("connected"));
        }

        const QJsonObject successRun = runToTerminal(actor);
        QCOMPARE(successRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = successRun.value(QStringLiteral("runId")).toString();
        const auto output = [&](const QString& key) {
            return actor.executeTool(QStringLiteral("get_module_output"),
                                     QJsonObject{{QStringLiteral("runId"), runId},
                                                 {QStringLiteral("instanceId"), distanceId},
                                                 {QStringLiteral("key"), key}});
        };
        for (const auto& expected :
             {qMakePair(QStringLiteral("distance"), 12.0), qMakePair(QStringLiteral("foot_x"), 5.0),
              qMakePair(QStringLiteral("foot_y"), 0.0)}) {
            const QJsonObject value = output(expected.first);
            QCOMPARE(value.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QVERIFY(std::abs(value.value(QStringLiteral("value")).toDouble() - expected.second) < 1e-9);
        }

        const QJsonObject disconnected = actor.executeTool(
            QStringLiteral("disconnect_modules"), QJsonObject{{QStringLiteral("fromId"), sourceId},
                                                              {QStringLiteral("toId"), distanceId},
                                                              {QStringLiteral("fromPort"), QStringLiteral("line")},
                                                              {QStringLiteral("toPort"), QStringLiteral("line")}});
        QCOMPARE(disconnected.value(QStringLiteral("status")).toString(), QStringLiteral("disconnected"));
        const QJsonObject validation = actor.executeTool(QStringLiteral("validate_flow"), {});
        QCOMPARE(validation.value(QStringLiteral("status")).toString(), QStringLiteral("invalid"));
        QVERIFY(!validation.contains(QStringLiteral("runId")));
        const QJsonArray errors = validation.value(QStringLiteral("errors")).toArray();
        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.first().toObject().value(QStringLiteral("moduleId")).toString(), distanceId);
        QCOMPARE(errors.first().toObject().value(QStringLiteral("portId")).toString(), QStringLiteral("line"));

        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }

    void testAgentFitsPointSetAndReportsRadiusConstraintFailure() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 FitCircle")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        for (const QString& plugin : {QStringLiteral("MeasurementInput"), QStringLiteral("FitCircle")}) {
            const QString status =
                actor.executeTool(QStringLiteral("load_module_plugin"), QJsonObject{{QStringLiteral("plugin"), plugin}})
                    .value(QStringLiteral("status"))
                    .toString();
            QVERIFY(status == QLatin1String("loaded") || status == QLatin1String("already_loaded"));
        }

        const QJsonObject source = actor.executeTool(
            QStringLiteral("add_module"),
            QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.measurementinput")},
                        {QStringLiteral("instanceName"), QStringLiteral("g8_fit_points")}});
        QCOMPARE(source.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString sourceId = source.value(QStringLiteral("instanceId")).toString();
        const QJsonObject fit =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.fitcircle")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_fit_circle")}});
        QCOMPARE(fit.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString fitId = fit.value(QStringLiteral("instanceId")).toString();

        const QJsonArray points{QJsonArray{42.0, 40.0}, QJsonArray{30.0, 52.0}, QJsonArray{18.0, 40.0},
                                QJsonArray{30.0, 28.0}};
        QCOMPARE(setParams(actor, sourceId,
                           QJsonObject{{QStringLiteral("mode"), QStringLiteral("point_set")},
                                       {QStringLiteral("points"), points}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject configured = setParams(actor, fitId,
                                                 QJsonObject{{QStringLiteral("threshold"), 0.1},
                                                             {QStringLiteral("iterations"), 100},
                                                             {QStringLiteral("minRadius"), 10.0},
                                                             {QStringLiteral("maxRadius"), 14.0}});
        QCOMPARE(configured.value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
        const QJsonObject connected = actor.executeTool(
            QStringLiteral("connect_modules"), QJsonObject{{QStringLiteral("fromId"), sourceId},
                                                           {QStringLiteral("toId"), fitId},
                                                           {QStringLiteral("fromPort"), QStringLiteral("fit_points")},
                                                           {QStringLiteral("toPort"), QStringLiteral("fit_points")}});
        QCOMPARE(connected.value(QStringLiteral("status")).toString(), QStringLiteral("connected"));

        const QJsonObject successRun = runToTerminal(actor);
        QCOMPARE(successRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = successRun.value(QStringLiteral("runId")).toString();
        const auto output = [&](const QString& key) {
            return actor.executeTool(QStringLiteral("get_module_output"),
                                     QJsonObject{{QStringLiteral("runId"), runId},
                                                 {QStringLiteral("instanceId"), fitId},
                                                 {QStringLiteral("key"), key}});
        };
        for (const auto& expected :
             {qMakePair(QStringLiteral("circle_center_x"), 30.0), qMakePair(QStringLiteral("circle_center_y"), 40.0),
              qMakePair(QStringLiteral("circle_radius"), 12.0), qMakePair(QStringLiteral("circle_error"), 0.0)}) {
            const QJsonObject value = output(expected.first);
            QCOMPARE(value.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QVERIFY(std::abs(value.value(QStringLiteral("value")).toDouble() - expected.second) < 1e-8);
        }

        const QJsonObject excludedRadius = setParams(actor, fitId,
                                                     QJsonObject{{QStringLiteral("threshold"), 0.1},
                                                                 {QStringLiteral("iterations"), 100},
                                                                 {QStringLiteral("minRadius"), 13.0},
                                                                 {QStringLiteral("maxRadius"), 20.0}});
        QCOMPARE(excludedRadius.value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
        const QJsonObject failureRun = runToTerminal(actor);
        QCOMPARE(failureRun.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QJsonObject error = failureRun.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(error.value(QStringLiteral("moduleId")).toString(), fitId);
        QCOMPARE(error.value(QStringLiteral("message")).toString(), QStringLiteral("圆拟合失败"));

        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }

    void testAgentFitsPointSetAndRejectsDegenerateLine() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 FitLine")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        for (const QString& plugin : {QStringLiteral("MeasurementInput"), QStringLiteral("FitLine")}) {
            const QString status =
                actor.executeTool(QStringLiteral("load_module_plugin"), QJsonObject{{QStringLiteral("plugin"), plugin}})
                    .value(QStringLiteral("status"))
                    .toString();
            QVERIFY(status == QLatin1String("loaded") || status == QLatin1String("already_loaded"));
        }

        const QJsonObject source = actor.executeTool(
            QStringLiteral("add_module"),
            QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.measurementinput")},
                        {QStringLiteral("instanceName"), QStringLiteral("g8_line_points")}});
        QCOMPARE(source.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString sourceId = source.value(QStringLiteral("instanceId")).toString();
        const QJsonObject fit =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.fitline")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_fit_line")}});
        QCOMPARE(fit.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString fitId = fit.value(QStringLiteral("instanceId")).toString();

        const QJsonArray points{QJsonArray{-10.0, 10.0}, QJsonArray{0.0, 10.0}, QJsonArray{10.0, 10.0},
                                QJsonArray{20.0, 10.0}};
        QCOMPARE(setParams(actor, sourceId,
                           QJsonObject{{QStringLiteral("mode"), QStringLiteral("point_set")},
                                       {QStringLiteral("points"), points}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        QCOMPARE(setParams(actor, fitId,
                           QJsonObject{{QStringLiteral("fitMethod"), QStringLiteral("RANSAC")},
                                       {QStringLiteral("threshold"), 0.1},
                                       {QStringLiteral("iterations"), 100}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject connected = actor.executeTool(
            QStringLiteral("connect_modules"), QJsonObject{{QStringLiteral("fromId"), sourceId},
                                                           {QStringLiteral("toId"), fitId},
                                                           {QStringLiteral("fromPort"), QStringLiteral("fit_points")},
                                                           {QStringLiteral("toPort"), QStringLiteral("fit_points")}});
        QCOMPARE(connected.value(QStringLiteral("status")).toString(), QStringLiteral("connected"));

        const QJsonObject successRun = runToTerminal(actor);
        QCOMPARE(successRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = successRun.value(QStringLiteral("runId")).toString();
        const auto outputForRun = [&](const QString& outputRunId, const QString& key) {
            return actor.executeTool(QStringLiteral("get_module_output"),
                                     QJsonObject{{QStringLiteral("runId"), outputRunId},
                                                 {QStringLiteral("instanceId"), fitId},
                                                 {QStringLiteral("key"), key}});
        };
        const auto output = [&](const QString& key) { return outputForRun(runId, key); };
        for (const auto& expected :
             {qMakePair(QStringLiteral("line_rho"), 10.0), qMakePair(QStringLiteral("line_phi"), 90.0),
              qMakePair(QStringLiteral("line_error"), 0.0)}) {
            const QJsonObject value = output(expected.first);
            QCOMPARE(value.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QVERIFY(std::abs(value.value(QStringLiteral("value")).toDouble() - expected.second) < 1e-6);
        }

        const QJsonArray pointsWithOutlier{QJsonArray{0.0, 10.0},  QJsonArray{10.0, 10.0}, QJsonArray{20.0, 10.0},
                                           QJsonArray{30.0, 10.0}, QJsonArray{40.0, 10.0}, QJsonArray{50.0, 10.0},
                                           QJsonArray{25.0, 30.0}};
        QCOMPARE(setParams(actor, sourceId,
                           QJsonObject{{QStringLiteral("mode"), QStringLiteral("point_set")},
                                       {QStringLiteral("points"), pointsWithOutlier}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        QCOMPARE(setParams(actor, fitId,
                           QJsonObject{{QStringLiteral("fitMethod"), QStringLiteral("RANSAC")},
                                       {QStringLiteral("threshold"), 0.5},
                                       {QStringLiteral("iterations"), 1000}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject ransacRun = runToTerminal(actor);
        QCOMPARE(ransacRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString ransacRunId = ransacRun.value(QStringLiteral("runId")).toString();
        for (const auto& expected :
             {qMakePair(QStringLiteral("line_rho"), 10.0), qMakePair(QStringLiteral("line_phi"), 90.0),
              qMakePair(QStringLiteral("line_error"), 20.0 / 7.0)}) {
            const QJsonObject value = outputForRun(ransacRunId, expected.first);
            QCOMPARE(value.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            double actual = value.value(QStringLiteral("value")).toDouble();
            if (expected.first == QLatin1String("line_phi")) {
                actual = std::remainder(actual - expected.second, 180.0);
            } else {
                actual -= expected.second;
            }
            QVERIFY(std::abs(actual) < 1e-3);
        }

        QCOMPARE(setParams(actor, fitId,
                           QJsonObject{{QStringLiteral("fitMethod"), QStringLiteral("LS")},
                                       {QStringLiteral("threshold"), 0.5},
                                       {QStringLiteral("iterations"), 1000}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject leastSquaresRun = runToTerminal(actor);
        QCOMPARE(leastSquaresRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString leastSquaresRunId = leastSquaresRun.value(QStringLiteral("runId")).toString();
        for (const auto& expected :
             {qMakePair(QStringLiteral("line_rho"), 90.0 / 7.0), qMakePair(QStringLiteral("line_phi"), 90.0),
              qMakePair(QStringLiteral("line_error"), 240.0 / 49.0)}) {
            const QJsonObject value = outputForRun(leastSquaresRunId, expected.first);
            QCOMPARE(value.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            double actual = value.value(QStringLiteral("value")).toDouble();
            if (expected.first == QLatin1String("line_phi")) {
                actual = std::remainder(actual - expected.second, 180.0);
            } else {
                actual -= expected.second;
            }
            QVERIFY(std::abs(actual) < 1e-3);
        }

        const QJsonArray repeatedPoints{QJsonArray{5.0, 5.0}, QJsonArray{5.0, 5.0}, QJsonArray{5.0, 5.0},
                                        QJsonArray{5.0, 5.0}};
        QCOMPARE(setParams(actor, sourceId,
                           QJsonObject{{QStringLiteral("mode"), QStringLiteral("point_set")},
                                       {QStringLiteral("points"), repeatedPoints}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        QCOMPARE(setParams(actor, fitId,
                           QJsonObject{{QStringLiteral("fitMethod"), QStringLiteral("RANSAC")},
                                       {QStringLiteral("threshold"), 0.1},
                                       {QStringLiteral("iterations"), 100}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject failureRun = runToTerminal(actor);
        QCOMPARE(failureRun.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QJsonObject error = failureRun.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(error.value(QStringLiteral("moduleId")).toString(), fitId);
        QCOMPARE(error.value(QStringLiteral("message")).toString(), QStringLiteral("直线拟合失败"));

        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }

    void testAgentFitsEllipseAndReportsAxisRangeFailure() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 FitEllipse")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        for (const QString& plugin : {QStringLiteral("MeasurementInput"), QStringLiteral("FitEllipse")}) {
            const QString status =
                actor.executeTool(QStringLiteral("load_module_plugin"), QJsonObject{{QStringLiteral("plugin"), plugin}})
                    .value(QStringLiteral("status"))
                    .toString();
            QVERIFY(status == QLatin1String("loaded") || status == QLatin1String("already_loaded"));
        }

        const QJsonObject source = actor.executeTool(
            QStringLiteral("add_module"),
            QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.measurementinput")},
                        {QStringLiteral("instanceName"), QStringLiteral("g8_fit_ellipse_points")}});
        QCOMPARE(source.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString sourceId = source.value(QStringLiteral("instanceId")).toString();
        const QJsonObject fit =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.fitellipse")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_fit_ellipse")}});
        QCOMPARE(fit.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString fitId = fit.value(QStringLiteral("instanceId")).toString();

        const QJsonArray points{QJsonArray{70.0, 40.0},
                                QJsonArray{50.0, 50.0},
                                QJsonArray{30.0, 40.0},
                                QJsonArray{50.0, 30.0},
                                QJsonArray{64.1421356237, 47.0710678119},
                                QJsonArray{35.8578643763, 47.0710678119},
                                QJsonArray{35.8578643763, 32.9289321881},
                                QJsonArray{64.1421356237, 32.9289321881}};
        QCOMPARE(setParams(actor, sourceId,
                           QJsonObject{{QStringLiteral("mode"), QStringLiteral("point_set")},
                                       {QStringLiteral("points"), points}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject configured = setParams(actor, fitId,
                                                 QJsonObject{{QStringLiteral("threshold"), 1.0},
                                                             {QStringLiteral("iterations"), 100},
                                                             {QStringLiteral("minAxis"), 5.0},
                                                             {QStringLiteral("maxAxis"), 25.0}});
        QCOMPARE(configured.value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
        const QJsonObject connected = actor.executeTool(
            QStringLiteral("connect_modules"), QJsonObject{{QStringLiteral("fromId"), sourceId},
                                                           {QStringLiteral("toId"), fitId},
                                                           {QStringLiteral("fromPort"), QStringLiteral("fit_points")},
                                                           {QStringLiteral("toPort"), QStringLiteral("fit_points")}});
        QCOMPARE(connected.value(QStringLiteral("status")).toString(), QStringLiteral("connected"));

        const QJsonObject successRun = runToTerminal(actor);
        QCOMPARE(successRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = successRun.value(QStringLiteral("runId")).toString();
        const auto output = [&](const QString& key) {
            return actor.executeTool(QStringLiteral("get_module_output"),
                                     QJsonObject{{QStringLiteral("runId"), runId},
                                                 {QStringLiteral("instanceId"), fitId},
                                                 {QStringLiteral("key"), key}});
        };
        for (const auto& expected :
             {qMakePair(QStringLiteral("ellipse_center_x"), 50.0), qMakePair(QStringLiteral("ellipse_center_y"), 40.0),
              qMakePair(QStringLiteral("ellipse_major_r"), 20.0), qMakePair(QStringLiteral("ellipse_minor_r"), 10.0),
              qMakePair(QStringLiteral("ellipse_ellipticity"), 0.5)}) {
            const QJsonObject value = output(expected.first);
            QCOMPARE(value.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            const double tolerance = expected.first == QLatin1String("ellipse_ellipticity") ? 0.02 : 0.25;
            QVERIFY(std::abs(value.value(QStringLiteral("value")).toDouble() - expected.second) < tolerance);
        }
        const QJsonObject fitError = output(QStringLiteral("ellipse_error"));
        QCOMPARE(fitError.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QVERIFY(fitError.value(QStringLiteral("value")).toDouble() < 0.5);

        const QJsonObject excludedAxes = setParams(actor, fitId,
                                                   QJsonObject{{QStringLiteral("threshold"), 1.0},
                                                               {QStringLiteral("iterations"), 100},
                                                               {QStringLiteral("minAxis"), 25.0},
                                                               {QStringLiteral("maxAxis"), 30.0}});
        QCOMPARE(excludedAxes.value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
        const QJsonObject failureRun = runToTerminal(actor);
        QCOMPARE(failureRun.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QJsonObject error = failureRun.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(error.value(QStringLiteral("moduleId")).toString(), fitId);
        QCOMPARE(error.value(QStringLiteral("message")).toString(), QStringLiteral("拟合半轴超出参数范围"));

        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }

    void testAgentCalculatesOffsetFromPointPortAndRejectsInvalidPoint3D() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 CalculateOffset")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        for (const QString& plugin : {QStringLiteral("MeasurementInput"), QStringLiteral("CalculateOffset")}) {
            const QString status =
                actor.executeTool(QStringLiteral("load_module_plugin"), QJsonObject{{QStringLiteral("plugin"), plugin}})
                    .value(QStringLiteral("status"))
                    .toString();
            QVERIFY(status == QLatin1String("loaded") || status == QLatin1String("already_loaded"));
        }

        const QJsonObject source = actor.executeTool(
            QStringLiteral("add_module"),
            QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.measurementinput")},
                        {QStringLiteral("instanceName"), QStringLiteral("g8_offset_point")}});
        QCOMPARE(source.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString sourceId = source.value(QStringLiteral("instanceId")).toString();
        const QJsonObject offset = actor.executeTool(
            QStringLiteral("add_module"),
            QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.calculateoffset")},
                        {QStringLiteral("instanceName"), QStringLiteral("g8_calculate_offset")}});
        QCOMPARE(offset.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString offsetId = offset.value(QStringLiteral("instanceId")).toString();

        const QJsonObject configuredSource =
            setParams(actor, sourceId,
                      QJsonObject{{QStringLiteral("mode"), QStringLiteral("point_line")},
                                  {QStringLiteral("point"), QJsonArray{10.0, 20.0, 0.0}},
                                  {QStringLiteral("line"), QJsonArray{0.0, 0.0, 100.0, 0.0}}});
        QCOMPARE(configuredSource.value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
        const QJsonObject configuredOffset = setParams(actor, offsetId,
                                                       QJsonObject{{QStringLiteral("currentX"), 999.0},
                                                                   {QStringLiteral("currentY"), 999.0},
                                                                   {QStringLiteral("currentAngle"), 30.0},
                                                                   {QStringLiteral("targetX"), 35.0},
                                                                   {QStringLiteral("targetY"), 55.0},
                                                                   {QStringLiteral("targetAngle"), 40.0}});
        QCOMPARE(configuredOffset.value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
        const QJsonObject connected =
            actor.executeTool(QStringLiteral("connect_modules"),
                              QJsonObject{{QStringLiteral("fromId"), sourceId},
                                          {QStringLiteral("toId"), offsetId},
                                          {QStringLiteral("fromPort"), QStringLiteral("point")},
                                          {QStringLiteral("toPort"), QStringLiteral("current_point")}});
        QCOMPARE(connected.value(QStringLiteral("status")).toString(), QStringLiteral("connected"));

        const QJsonObject successRun = runToTerminal(actor);
        QCOMPARE(successRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = successRun.value(QStringLiteral("runId")).toString();
        for (const auto& expected :
             {qMakePair(QStringLiteral("offset_x"), -25.0), qMakePair(QStringLiteral("offset_y"), -35.0),
              qMakePair(QStringLiteral("offset_a"), -10.0)}) {
            const QJsonObject value = actor.executeTool(QStringLiteral("get_module_output"),
                                                        QJsonObject{{QStringLiteral("runId"), runId},
                                                                    {QStringLiteral("instanceId"), offsetId},
                                                                    {QStringLiteral("key"), expected.first}});
            QCOMPARE(value.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(value.value(QStringLiteral("value")).toDouble(), expected.second);
        }

        const QJsonObject malformedPoint =
            setParams(actor, sourceId,
                      QJsonObject{{QStringLiteral("mode"), QStringLiteral("point_line")},
                                  {QStringLiteral("point"), QJsonArray{10.0, 20.0}},
                                  {QStringLiteral("line"), QJsonArray{0.0, 0.0, 100.0, 0.0}}});
        QCOMPARE(malformedPoint.value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
        const QJsonObject failureRun = runToTerminal(actor);
        QCOMPARE(failureRun.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QJsonObject error = failureRun.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(error.value(QStringLiteral("moduleId")).toString(), offsetId);
        QCOMPARE(error.value(QStringLiteral("message")).toString(), QStringLiteral("输入类型不匹配：当前坐标点"));

        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }

    void testAgentCropsMultipleRectanglesAndReportsNoIntersection() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
        const QString sourcePath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-crop-source.png"));
        cv::Mat source(100, 120, CV_8UC3);
        for (int y = 0; y < source.rows; ++y) {
            for (int x = 0; x < source.cols; ++x)
                source.at<cv::Vec3b>(y, x) =
                    cv::Vec3b(static_cast<uchar>(x), static_cast<uchar>(y), static_cast<uchar>((x + y) % 256));
        }
        QVERIFY(cv::imwrite(sourcePath.toStdString(), source));

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 CropImage")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        for (const QString& plugin : {QStringLiteral("GrabImage"), QStringLiteral("CropImage")}) {
            const QString status =
                actor.executeTool(QStringLiteral("load_module_plugin"), QJsonObject{{QStringLiteral("plugin"), plugin}})
                    .value(QStringLiteral("status"))
                    .toString();
            QVERIFY(status == QLatin1String("loaded") || status == QLatin1String("already_loaded"));
        }
        const QJsonObject grab =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.grabimage")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_crop_source")}});
        const QJsonObject crop =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.cropimage")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_crop_rectangles")}});
        QCOMPARE(grab.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        QCOMPARE(crop.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString grabId = grab.value(QStringLiteral("instanceId")).toString();
        const QString cropId = crop.value(QStringLiteral("instanceId")).toString();
        QCOMPARE(setParams(actor, grabId,
                           QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Path")},
                                       {QStringLiteral("filePath"), sourcePath}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonArray rectangles{QJsonArray{60, 50, 10, 15, 0}, QJsonArray{0, 0, 10, 10, 0},
                                    QJsonArray{200, 200, 5, 5, 0}};
        QCOMPARE(setParams(actor, cropId,
                           QJsonObject{{QStringLiteral("rectangles"), rectangles},
                                       {QStringLiteral("outputFirstAsImage"), true}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        QCOMPARE(actor
                     .executeTool(QStringLiteral("connect_modules"),
                                  QJsonObject{{QStringLiteral("fromId"), grabId},
                                              {QStringLiteral("toId"), cropId},
                                              {QStringLiteral("fromPort"), QStringLiteral("image")},
                                              {QStringLiteral("toPort"), QStringLiteral("image")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("connected"));

        QJsonObject result = runToTerminal(actor);
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = result.value(QStringLiteral("runId")).toString();
        const QJsonObject count = actor.executeTool(QStringLiteral("get_module_output"),
                                                    QJsonObject{{QStringLiteral("runId"), runId},
                                                                {QStringLiteral("instanceId"), cropId},
                                                                {QStringLiteral("key"), QStringLiteral("crop_count")}});
        QCOMPARE(count.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(count.value(QStringLiteral("value")).toInt(), 2);
        const QJsonObject image =
            actor.executeTool(QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                                               {QStringLiteral("instanceId"), cropId},
                                                                               {QStringLiteral("preview"), true}});
        QCOMPARE(image.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        const QJsonObject imageSummary =
            image.value(QStringLiteral("summary")).toObject().value(QStringLiteral("image")).toObject();
        QCOMPARE(imageSummary.value(QStringLiteral("width")).toInt(), 20);
        QCOMPARE(imageSummary.value(QStringLiteral("height")).toInt(), 30);
        const QJsonArray transform = imageSummary.value(QStringLiteral("sourceCoordinates"))
                                         .toObject()
                                         .value(QStringLiteral("localToSource"))
                                         .toArray();
        QCOMPARE(transform.size(), 9);
        QCOMPARE(transform.at(2).toDouble(), 50.0);
        QCOMPARE(transform.at(5).toDouble(), 35.0);
        const QByteArray previewBytes = QByteArray::fromBase64(image.value(QStringLiteral("summary"))
                                                                   .toObject()
                                                                   .value(QStringLiteral("preview"))
                                                                   .toObject()
                                                                   .value(QStringLiteral("base64"))
                                                                   .toString()
                                                                   .toLatin1());
        const QImage preview = QImage::fromData(previewBytes);
        QVERIFY(!preview.isNull());
        QCOMPARE(preview.pixelColor(0, 0), QColor(85, 35, 50));

        const QJsonArray outsideRectangles{QJsonValue(QJsonArray{200, 200, 5, 5, 0})};
        QCOMPARE(setParams(actor, cropId,
                           QJsonObject{{QStringLiteral("rectangles"), outsideRectangles},
                                       {QStringLiteral("outputFirstAsImage"), true}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        result = runToTerminal(actor);
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QJsonObject error = result.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(error.value(QStringLiteral("moduleId")).toString(), cropId);
        QCOMPARE(error.value(QStringLiteral("message")).toString(),
                 QStringLiteral("所有裁剪矩形与图像无交集（1 个矩形全部无效）"));

        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }

    void testAgentFitsHeightMapAndReportsNoValidPixels() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
        const QString planePath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-fit-plane.pfm"));
        const QString zeroPath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-fit-plane-zero.pfm"));
        cv::Mat plane(20, 30, CV_32FC1);
        cv::Mat zeros(20, 30, CV_32FC1, cv::Scalar(0.0F));
        for (int y = 0; y < plane.rows; ++y) {
            for (int x = 0; x < plane.cols; ++x)
                plane.at<float>(y, x) = static_cast<float>(5 + 2 * x);
        }
        QVERIFY(cv::imwrite(planePath.toStdString(), plane));
        QVERIFY(cv::imwrite(zeroPath.toStdString(), zeros));

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 FitPlane")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        for (const QString& plugin : {QStringLiteral("GrabImage"), QStringLiteral("FitPlane")}) {
            const QString status =
                actor.executeTool(QStringLiteral("load_module_plugin"), QJsonObject{{QStringLiteral("plugin"), plugin}})
                    .value(QStringLiteral("status"))
                    .toString();
            QVERIFY(status == QLatin1String("loaded") || status == QLatin1String("already_loaded"));
        }

        const QJsonObject grab =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.grabimage")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_height_map")}});
        QCOMPARE(grab.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString grabId = grab.value(QStringLiteral("instanceId")).toString();
        const QJsonObject fit =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.fitplane")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_fit_plane")}});
        QCOMPARE(fit.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString fitId = fit.value(QStringLiteral("instanceId")).toString();

        const QJsonObject configuredGrab = setParams(actor, grabId,
                                                     QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Path")},
                                                                 {QStringLiteral("filePath"), planePath}});
        QCOMPARE(configuredGrab.value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
        const QJsonObject configuredFit = setParams(actor, fitId,
                                                    QJsonObject{{QStringLiteral("roiLength1"), 0},
                                                                {QStringLiteral("roiLength2"), 0},
                                                                {QStringLiteral("pixelSizeX"), 1.0},
                                                                {QStringLiteral("pixelSizeY"), 1.0},
                                                                {QStringLiteral("zScale"), 1.0},
                                                                {QStringLiteral("invalidValue"), 0.0},
                                                                {QStringLiteral("autoNoData"), false}});
        QCOMPARE(configuredFit.value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
        const QJsonObject connected = actor.executeTool(
            QStringLiteral("connect_modules"), QJsonObject{{QStringLiteral("fromId"), grabId},
                                                           {QStringLiteral("toId"), fitId},
                                                           {QStringLiteral("fromPort"), QStringLiteral("image")},
                                                           {QStringLiteral("toPort"), QStringLiteral("image")}});
        QCOMPARE(connected.value(QStringLiteral("status")).toString(), QStringLiteral("connected"));

        const QJsonObject successRun = runToTerminal(actor);
        QCOMPARE(successRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = successRun.value(QStringLiteral("runId")).toString();
        const auto output = [&](const QString& key) {
            return actor.executeTool(QStringLiteral("get_module_output"),
                                     QJsonObject{{QStringLiteral("runId"), runId},
                                                 {QStringLiteral("instanceId"), fitId},
                                                 {QStringLiteral("key"), key}});
        };
        const double norm = std::sqrt(5.0);
        const QList<QPair<QString, double>> expected{
            {QStringLiteral("plane_nx"), -2.0 / norm},
            {QStringLiteral("plane_ny"), 0.0},
            {QStringLiteral("plane_nz"), 1.0 / norm},
            {QStringLiteral("plane_d"), -5.0 / norm},
            {QStringLiteral("flatness"), 0.0},
            {QStringLiteral("rms"), 0.0},
            {QStringLiteral("valid_pixel_count"), 600.0},
        };
        for (const auto& item : expected) {
            const QJsonObject value = output(item.first);
            QCOMPARE(value.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            const double tolerance = item.first == QLatin1String("valid_pixel_count") ? 0.0 : 1e-6;
            QVERIFY(std::abs(value.value(QStringLiteral("value")).toDouble() - item.second) <= tolerance);
        }

        const QJsonObject zeroGrab = setParams(actor, grabId, QJsonObject{{QStringLiteral("filePath"), zeroPath}});
        QCOMPARE(zeroGrab.value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
        const QJsonObject failureRun = runToTerminal(actor);
        QCOMPARE(failureRun.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QJsonObject error = failureRun.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(error.value(QStringLiteral("moduleId")).toString(), fitId);
        QCOMPARE(error.value(QStringLiteral("message")).toString(),
                 QStringLiteral("ROI 内有效像素不足（0<3），无法拟合平面"));

        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }

    void testAgentMeasuresConfiguredLineAndReportsNoLine() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
        const QString linePath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-measure-line.png"));
        const QString blankPath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-measure-line-blank.png"));
        cv::Mat lineImage(200, 200, CV_8UC3, cv::Scalar(0, 0, 0));
        cv::Mat blankImage(200, 200, CV_8UC3, cv::Scalar(0, 0, 0));
        cv::line(lineImage, cv::Point(20, 100), cv::Point(180, 100), cv::Scalar(255, 255, 255), 2, cv::LINE_8);
        QVERIFY(cv::imwrite(linePath.toStdString(), lineImage));
        QVERIFY(cv::imwrite(blankPath.toStdString(), blankImage));

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 MeasureLine")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        for (const QString& plugin : {QStringLiteral("GrabImage"), QStringLiteral("MeasureLine")}) {
            const QString status =
                actor.executeTool(QStringLiteral("load_module_plugin"), QJsonObject{{QStringLiteral("plugin"), plugin}})
                    .value(QStringLiteral("status"))
                    .toString();
            QVERIFY(status == QLatin1String("loaded") || status == QLatin1String("already_loaded"));
        }

        const QJsonObject grab =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.grabimage")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_line_image")}});
        QCOMPARE(grab.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString grabId = grab.value(QStringLiteral("instanceId")).toString();
        const QJsonObject measure =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.measureline")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_measure_line")}});
        QCOMPARE(measure.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString measureId = measure.value(QStringLiteral("instanceId")).toString();
        QCOMPARE(setParams(actor, grabId,
                           QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Path")},
                                       {QStringLiteral("filePath"), linePath}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        QCOMPARE(setParams(actor, measureId,
                           QJsonObject{{QStringLiteral("minLength"), 40.0},
                                       {QStringLiteral("maxLength"), 180.0},
                                       {QStringLiteral("threshold"), 50.0},
                                       {QStringLiteral("minAngle"), 0.0},
                                       {QStringLiteral("maxAngle"), 180.0}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject connected = actor.executeTool(
            QStringLiteral("connect_modules"), QJsonObject{{QStringLiteral("fromId"), grabId},
                                                           {QStringLiteral("toId"), measureId},
                                                           {QStringLiteral("fromPort"), QStringLiteral("image")},
                                                           {QStringLiteral("toPort"), QStringLiteral("image")}});
        QCOMPARE(connected.value(QStringLiteral("status")).toString(), QStringLiteral("connected"));

        const QJsonObject successRun = runToTerminal(actor);
        QCOMPARE(successRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = successRun.value(QStringLiteral("runId")).toString();
        const auto output = [&](const QString& key) {
            return actor.executeTool(QStringLiteral("get_module_output"),
                                     QJsonObject{{QStringLiteral("runId"), runId},
                                                 {QStringLiteral("instanceId"), measureId},
                                                 {QStringLiteral("key"), key}});
        };
        const QJsonObject length = output(QStringLiteral("line_length"));
        QCOMPARE(length.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QVERIFY(length.value(QStringLiteral("value")).toDouble() >= 150.0);
        QVERIFY(length.value(QStringLiteral("value")).toDouble() <= 180.0);
        const QJsonObject angle = output(QStringLiteral("line_angle"));
        QCOMPARE(angle.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        const double angleDegrees = angle.value(QStringLiteral("value")).toDouble();
        QVERIFY(std::min(angleDegrees, std::abs(180.0 - angleDegrees)) < 1.0);
        const QJsonObject line = output(QStringLiteral("line"));
        QCOMPARE(line.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(line.value(QStringLiteral("type")).toString(), QStringLiteral("Line2D"));
        const QJsonObject lineValue = line.value(QStringLiteral("value")).toObject();
        QCOMPARE(lineValue.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
        const QJsonArray start = lineValue.value(QStringLiteral("start")).toArray();
        const QJsonArray end = lineValue.value(QStringLiteral("end")).toArray();
        QCOMPARE(start.size(), 2);
        QCOMPARE(end.size(), 2);
        for (const QJsonValue& coordinate : {start[0], start[1], end[0], end[1]})
            QVERIFY(coordinate.isDouble() && std::isfinite(coordinate.toDouble()));

        QCOMPARE(setParams(actor, grabId, QJsonObject{{QStringLiteral("filePath"), blankPath}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject failureRun = runToTerminal(actor);
        QCOMPARE(failureRun.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QJsonObject error = failureRun.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(error.value(QStringLiteral("moduleId")).toString(), measureId);
        QCOMPARE(error.value(QStringLiteral("message")).toString(), QStringLiteral("未检测到线条"));

        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }

    void testAgentMeasuresRectangleAndReportsNoContour() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
        const QString rectanglePath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-measure-rect.png"));
        const QString blankPath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-measure-rect-blank.png"));
        cv::Mat rectangleImage(200, 200, CV_8UC3, cv::Scalar(0, 0, 0));
        cv::Mat blankImage(200, 200, CV_8UC3, cv::Scalar(0, 0, 0));
        cv::rectangle(rectangleImage, cv::Point(50, 60), cv::Point(150, 140), cv::Scalar(255, 255, 255), cv::FILLED);
        QVERIFY(cv::imwrite(rectanglePath.toStdString(), rectangleImage));
        QVERIFY(cv::imwrite(blankPath.toStdString(), blankImage));

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 MeasureRect")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        for (const QString& plugin : {QStringLiteral("GrabImage"), QStringLiteral("MeasureRect")}) {
            const QString status =
                actor.executeTool(QStringLiteral("load_module_plugin"), QJsonObject{{QStringLiteral("plugin"), plugin}})
                    .value(QStringLiteral("status"))
                    .toString();
            QVERIFY(status == QLatin1String("loaded") || status == QLatin1String("already_loaded"));
        }

        const QJsonObject grab =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.grabimage")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_rect_image")}});
        QCOMPARE(grab.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString grabId = grab.value(QStringLiteral("instanceId")).toString();
        const QJsonObject measure =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.measurerect")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_measure_rect")}});
        QCOMPARE(measure.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString measureId = measure.value(QStringLiteral("instanceId")).toString();
        QCOMPARE(setParams(actor, grabId,
                           QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Path")},
                                       {QStringLiteral("filePath"), rectanglePath}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        QCOMPARE(setParams(actor, measureId,
                           QJsonObject{{QStringLiteral("minArea"), 100.0},
                                       {QStringLiteral("maxArea"), 100000.0},
                                       {QStringLiteral("threshold1"), 50.0},
                                       {QStringLiteral("threshold2"), 150.0}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject connected = actor.executeTool(
            QStringLiteral("connect_modules"), QJsonObject{{QStringLiteral("fromId"), grabId},
                                                           {QStringLiteral("toId"), measureId},
                                                           {QStringLiteral("fromPort"), QStringLiteral("image")},
                                                           {QStringLiteral("toPort"), QStringLiteral("image")}});
        QCOMPARE(connected.value(QStringLiteral("status")).toString(), QStringLiteral("connected"));

        const QJsonObject successRun = runToTerminal(actor);
        QCOMPARE(successRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = successRun.value(QStringLiteral("runId")).toString();
        const auto output = [&](const QString& key) {
            return actor.executeTool(QStringLiteral("get_module_output"),
                                     QJsonObject{{QStringLiteral("runId"), runId},
                                                 {QStringLiteral("instanceId"), measureId},
                                                 {QStringLiteral("key"), key}});
        };
        for (const auto& expected : {qMakePair(QStringLiteral("rect_width"), qMakePair(95.0, 105.0)),
                                     qMakePair(QStringLiteral("rect_height"), qMakePair(75.0, 85.0)),
                                     qMakePair(QStringLiteral("rect_area"), qMakePair(7000.0, 8500.0))}) {
            const QJsonObject value = output(expected.first);
            QCOMPARE(value.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            const double measured = value.value(QStringLiteral("value")).toDouble();
            QVERIFY(measured >= expected.second.first);
            QVERIFY(measured <= expected.second.second);
        }

        QCOMPARE(setParams(actor, grabId, QJsonObject{{QStringLiteral("filePath"), blankPath}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject failureRun = runToTerminal(actor);
        QCOMPARE(failureRun.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QJsonObject error = failureRun.value(QStringLiteral("lastError")).toObject();
        QCOMPARE(error.value(QStringLiteral("moduleId")).toString(), measureId);
        QCOMPARE(error.value(QStringLiteral("message")).toString(), QStringLiteral("未检测到轮廓"));

        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }

    void testAgentMeasuresFiniteLineSegmentsAndRequiresBothInputs() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 LinesDistance")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        for (const QString& plugin : {QStringLiteral("MeasurementInput"), QStringLiteral("LinesDistance")}) {
            const QString status =
                actor.executeTool(QStringLiteral("load_module_plugin"), QJsonObject{{QStringLiteral("plugin"), plugin}})
                    .value(QStringLiteral("status"))
                    .toString();
            QVERIFY(status == QLatin1String("loaded") || status == QLatin1String("already_loaded"));
        }

        const QJsonObject source = actor.executeTool(
            QStringLiteral("add_module"),
            QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.measurementinput")},
                        {QStringLiteral("instanceName"), QStringLiteral("g8_line_pair")}});
        QCOMPARE(source.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString sourceId = source.value(QStringLiteral("instanceId")).toString();
        const QJsonObject distance = actor.executeTool(
            QStringLiteral("add_module"),
            QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.linesdistance")},
                        {QStringLiteral("instanceName"), QStringLiteral("g8_lines_distance")}});
        QCOMPARE(distance.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString distanceId = distance.value(QStringLiteral("instanceId")).toString();
        QCOMPARE(setParams(actor, sourceId,
                           QJsonObject{{QStringLiteral("mode"), QStringLiteral("line_pair")},
                                       {QStringLiteral("line1"), QJsonArray{0.0, 0.0, 1.0, 0.0}},
                                       {QStringLiteral("line2"), QJsonArray{3.0, -1.0, 3.0, 1.0}}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));

        for (const QString& port : {QStringLiteral("line1"), QStringLiteral("line2")}) {
            const QJsonObject connected =
                actor.executeTool(QStringLiteral("connect_modules"), QJsonObject{{QStringLiteral("fromId"), sourceId},
                                                                                 {QStringLiteral("toId"), distanceId},
                                                                                 {QStringLiteral("fromPort"), port},
                                                                                 {QStringLiteral("toPort"), port}});
            QCOMPARE(connected.value(QStringLiteral("status")).toString(), QStringLiteral("connected"));
        }

        const QJsonObject successRun = runToTerminal(actor);
        QCOMPARE(successRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = successRun.value(QStringLiteral("runId")).toString();
        const auto output = [&](const QString& key) {
            return actor.executeTool(QStringLiteral("get_module_output"),
                                     QJsonObject{{QStringLiteral("runId"), runId},
                                                 {QStringLiteral("instanceId"), distanceId},
                                                 {QStringLiteral("key"), key}});
        };
        const QList<QPair<QString, double>> expected{{QStringLiteral("distance"), 2.0},
                                                     {QStringLiteral("nearest_x1"), 1.0},
                                                     {QStringLiteral("nearest_y1"), 0.0},
                                                     {QStringLiteral("nearest_x2"), 3.0},
                                                     {QStringLiteral("nearest_y2"), 0.0}};
        for (const auto& item : expected) {
            const QJsonObject value = output(item.first);
            QCOMPARE(value.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(value.value(QStringLiteral("value")).toDouble(), item.second);
        }
        const QJsonObject segment = output(QStringLiteral("nearest_segment"));
        QCOMPARE(segment.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(segment.value(QStringLiteral("value")).toArray(), (QJsonArray{1.0, 0.0, 3.0, 0.0}));

        const QJsonObject disconnected = actor.executeTool(
            QStringLiteral("disconnect_modules"), QJsonObject{{QStringLiteral("fromId"), sourceId},
                                                              {QStringLiteral("toId"), distanceId},
                                                              {QStringLiteral("fromPort"), QStringLiteral("line2")},
                                                              {QStringLiteral("toPort"), QStringLiteral("line2")}});
        QCOMPARE(disconnected.value(QStringLiteral("status")).toString(), QStringLiteral("disconnected"));
        const QJsonObject validation = actor.executeTool(QStringLiteral("validate_flow"), {});
        QCOMPARE(validation.value(QStringLiteral("status")).toString(), QStringLiteral("invalid"));
        QVERIFY(!validation.contains(QStringLiteral("runId")));
        const QJsonArray errors = validation.value(QStringLiteral("errors")).toArray();
        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.first().toObject().value(QStringLiteral("moduleId")).toString(), distanceId);
        QCOMPARE(errors.first().toObject().value(QStringLiteral("portId")).toString(), QStringLiteral("line2"));

        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }

    void testAgentMeasures2DGapAndRequiresSecondPoint() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 MeasureGap")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        for (const QString& plugin : {QStringLiteral("MeasurementInput"), QStringLiteral("MeasureGap")}) {
            const QString status =
                actor.executeTool(QStringLiteral("load_module_plugin"), QJsonObject{{QStringLiteral("plugin"), plugin}})
                    .value(QStringLiteral("status"))
                    .toString();
            QVERIFY(status == QLatin1String("loaded") || status == QLatin1String("already_loaded"));
        }

        const QJsonObject source = actor.executeTool(
            QStringLiteral("add_module"),
            QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.measurementinput")},
                        {QStringLiteral("instanceName"), QStringLiteral("g8_point_pair")}});
        QCOMPARE(source.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString sourceId = source.value(QStringLiteral("instanceId")).toString();
        const QJsonObject gap =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.measuregap")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_measure_gap")}});
        QCOMPARE(gap.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString gapId = gap.value(QStringLiteral("instanceId")).toString();
        QCOMPARE(setParams(actor, sourceId,
                           QJsonObject{{QStringLiteral("mode"), QStringLiteral("point_pair")},
                                       {QStringLiteral("point1"), QJsonArray{0.0, 0.0}},
                                       {QStringLiteral("point2"), QJsonArray{3.0, 4.0}}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));

        for (const QString& port : {QStringLiteral("point1"), QStringLiteral("point2")}) {
            const QJsonObject connected =
                actor.executeTool(QStringLiteral("connect_modules"), QJsonObject{{QStringLiteral("fromId"), sourceId},
                                                                                 {QStringLiteral("toId"), gapId},
                                                                                 {QStringLiteral("fromPort"), port},
                                                                                 {QStringLiteral("toPort"), port}});
            QCOMPARE(connected.value(QStringLiteral("status")).toString(), QStringLiteral("connected"));
        }

        const QJsonObject successRun = runToTerminal(actor);
        QCOMPARE(successRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = successRun.value(QStringLiteral("runId")).toString();
        const auto output = [&](const QString& key) {
            return actor.executeTool(QStringLiteral("get_module_output"),
                                     QJsonObject{{QStringLiteral("runId"), runId},
                                                 {QStringLiteral("instanceId"), gapId},
                                                 {QStringLiteral("key"), key}});
        };
        for (const auto& expected :
             {qMakePair(QStringLiteral("gap"), 5.0), qMakePair(QStringLiteral("gap_distance"), 5.0),
              qMakePair(QStringLiteral("gap_delta_x"), 3.0), qMakePair(QStringLiteral("gap_delta_y"), 4.0)}) {
            const QJsonObject value = output(expected.first);
            QCOMPARE(value.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(value.value(QStringLiteral("value")).toDouble(), expected.second);
            QCOMPARE(value.value(QStringLiteral("unit")).toString(), QStringLiteral("px"));
        }
        const QJsonObject deltaZ = output(QStringLiteral("gap_delta_z"));
        QCOMPARE(deltaZ.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(deltaZ.value(QStringLiteral("value")).toDouble(), 0.0);
        QVERIFY(!deltaZ.contains(QStringLiteral("unit")));
        const QJsonObject dimension = output(QStringLiteral("measurement_dimension"));
        QCOMPARE(dimension.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(dimension.value(QStringLiteral("value")).toString(), QStringLiteral("2d"));
        QVERIFY(!dimension.contains(QStringLiteral("unit")));

        const QJsonObject disconnected = actor.executeTool(
            QStringLiteral("disconnect_modules"), QJsonObject{{QStringLiteral("fromId"), sourceId},
                                                              {QStringLiteral("toId"), gapId},
                                                              {QStringLiteral("fromPort"), QStringLiteral("point2")},
                                                              {QStringLiteral("toPort"), QStringLiteral("point2")}});
        QCOMPARE(disconnected.value(QStringLiteral("status")).toString(), QStringLiteral("disconnected"));
        const QJsonObject validation = actor.executeTool(QStringLiteral("validate_flow"), {});
        QCOMPARE(validation.value(QStringLiteral("status")).toString(), QStringLiteral("invalid"));
        QVERIFY(!validation.contains(QStringLiteral("runId")));
        const QJsonArray errors = validation.value(QStringLiteral("errors")).toArray();
        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.first().toObject().value(QStringLiteral("moduleId")).toString(), gapId);
        QCOMPARE(errors.first().toObject().value(QStringLiteral("portId")).toString(), QStringLiteral("point2"));

        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }

    void testAgentMeasuresWorldPointToPlaneAndRequiresPlane() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 PointSurfaceDistance")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        for (const QString& plugin : {QStringLiteral("MeasurementInput"), QStringLiteral("PointSurfaceDistance")}) {
            const QString status =
                actor.executeTool(QStringLiteral("load_module_plugin"), QJsonObject{{QStringLiteral("plugin"), plugin}})
                    .value(QStringLiteral("status"))
                    .toString();
            QVERIFY(status == QLatin1String("loaded") || status == QLatin1String("already_loaded"));
        }

        const QJsonObject source = actor.executeTool(
            QStringLiteral("add_module"),
            QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.measurementinput")},
                        {QStringLiteral("instanceName"), QStringLiteral("g8_point_plane")}});
        QCOMPARE(source.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString sourceId = source.value(QStringLiteral("instanceId")).toString();
        const QJsonObject distance = actor.executeTool(
            QStringLiteral("add_module"),
            QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.pointsurfacedistance")},
                        {QStringLiteral("instanceName"), QStringLiteral("g8_point_surface_distance")}});
        QCOMPARE(distance.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString distanceId = distance.value(QStringLiteral("instanceId")).toString();
        QCOMPARE(
            setParams(actor, sourceId,
                      QJsonObject{{QStringLiteral("mode"), QStringLiteral("point_plane")},
                                  {QStringLiteral("point"), QJsonArray{1.0, 2.0, 10.0}},
                                  {QStringLiteral("plane"), QJsonArray{0.0, 0.0, 3.0, 1.0, 0.0, 3.0, 0.0, 1.0, 3.0}}})
                .value(QStringLiteral("status"))
                .toString(),
            QStringLiteral("params_set"));

        for (const QString& port : {QStringLiteral("image"), QStringLiteral("point"), QStringLiteral("plane")}) {
            const QJsonObject connected =
                actor.executeTool(QStringLiteral("connect_modules"), QJsonObject{{QStringLiteral("fromId"), sourceId},
                                                                                 {QStringLiteral("toId"), distanceId},
                                                                                 {QStringLiteral("fromPort"), port},
                                                                                 {QStringLiteral("toPort"), port}});
            QCOMPARE(connected.value(QStringLiteral("status")).toString(), QStringLiteral("connected"));
        }

        const QJsonObject successRun = runToTerminal(actor);
        QCOMPARE(successRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = successRun.value(QStringLiteral("runId")).toString();
        const auto outputKey = [&](const QString& key) {
            return actor.executeTool(QStringLiteral("get_module_output"),
                                     QJsonObject{{QStringLiteral("runId"), runId},
                                                 {QStringLiteral("instanceId"), distanceId},
                                                 {QStringLiteral("key"), key}});
        };
        const QJsonObject measuredDistance = outputKey(QStringLiteral("distance"));
        QCOMPARE(measuredDistance.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(measuredDistance.value(QStringLiteral("value")).toDouble(), 7.0);
        for (const auto& expected : {qMakePair(QStringLiteral("foot_x"), 1.0), qMakePair(QStringLiteral("foot_y"), 2.0),
                                     qMakePair(QStringLiteral("foot_z"), 3.0)}) {
            const QJsonObject coordinate = outputKey(expected.first);
            QCOMPARE(coordinate.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(coordinate.value(QStringLiteral("value")).toDouble(), expected.second);
        }
        const QJsonObject footPoint = actor.executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                             {QStringLiteral("instanceId"), distanceId},
                                                             {QStringLiteral("port"), QStringLiteral("foot_point")}});
        QCOMPARE(footPoint.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(footPoint.value(QStringLiteral("type")).toString(), QStringLiteral("Point3D"));
        QCOMPARE(footPoint.value(QStringLiteral("coordinateFrame")).toString(), QStringLiteral("World3D"));
        QCOMPARE(footPoint.value(QStringLiteral("value")).toArray(), (QJsonArray{1.0, 2.0, 3.0}));

        const QJsonObject disconnected = actor.executeTool(
            QStringLiteral("disconnect_modules"), QJsonObject{{QStringLiteral("fromId"), sourceId},
                                                              {QStringLiteral("toId"), distanceId},
                                                              {QStringLiteral("fromPort"), QStringLiteral("plane")},
                                                              {QStringLiteral("toPort"), QStringLiteral("plane")}});
        QCOMPARE(disconnected.value(QStringLiteral("status")).toString(), QStringLiteral("disconnected"));
        const QJsonObject validation = actor.executeTool(QStringLiteral("validate_flow"), {});
        QCOMPARE(validation.value(QStringLiteral("status")).toString(), QStringLiteral("invalid"));
        QVERIFY(!validation.contains(QStringLiteral("runId")));
        const QJsonArray errors = validation.value(QStringLiteral("errors")).toArray();
        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.first().toObject().value(QStringLiteral("moduleId")).toString(), distanceId);
        QCOMPARE(errors.first().toObject().value(QStringLiteral("portId")).toString(), QStringLiteral("plane"));

        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }

    void testAgentQueueOutEmptyResultUsesConfiguredOutputAndReportsZeroSize() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();

        const QString queueName = QStringLiteral("g8_empty_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        const QString outputKey = QStringLiteral("g8_custom_queue_item");
        QCOMPARE(GlobalVarManager::instance().queueSize(queueName), 0);

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 QueueOut")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        const QJsonObject loaded = actor.executeTool(
            QStringLiteral("load_module_plugin"), QJsonObject{{QStringLiteral("plugin"), QStringLiteral("队列输出")}});
        QVERIFY(loaded.value(QStringLiteral("status")).toString() == QLatin1String("loaded") ||
                loaded.value(QStringLiteral("status")).toString() == QLatin1String("already_loaded"));
        const QJsonObject added =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.queueout")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_queue_out_empty")}});
        QCOMPARE(added.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString instanceId = added.value(QStringLiteral("instanceId")).toString();
        QCOMPARE(setParams(actor, instanceId,
                           QJsonObject{{QStringLiteral("queueName"), queueName},
                                       {QStringLiteral("outputVariable"), outputKey},
                                       {QStringLiteral("peekOnly"), false}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));

        const QJsonObject result = runToTerminal(actor);
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = result.value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());
        const auto expectOutput = [&actor, &runId, &instanceId](const QString& key, const QJsonValue& expected) {
            const QJsonObject output = actor.executeTool(QStringLiteral("get_module_output"),
                                                         QJsonObject{{QStringLiteral("runId"), runId},
                                                                     {QStringLiteral("instanceId"), instanceId},
                                                                     {QStringLiteral("key"), key}});
            if (output.value(QStringLiteral("status")).toString() != QLatin1String("available") ||
                output.value(QStringLiteral("value")) != expected) {
                return false;
            }
            return true;
        };
        QVERIFY(expectOutput(QStringLiteral("queue_empty"), QJsonValue(true)));
        QVERIFY(expectOutput(outputKey, QJsonValue(QStringLiteral(""))));
        QVERIFY(expectOutput(QStringLiteral("queue_%1_size").arg(queueName), QJsonValue(0)));
        QCOMPARE(GlobalVarManager::instance().queueSize(queueName), 0);
    }

    void testAgentQueueInRoundTripsConfiguredStringAndRejectsMissingData() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();

        const QString queueName =
            QStringLiteral("g8_roundtrip_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        QCOMPARE(GlobalVarManager::instance().queueSize(queueName), 0);

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 QueueIn")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        for (const QString& plugin :
             {QStringLiteral("创建字符串"), QStringLiteral("队列输入"), QStringLiteral("队列输出")}) {
            const QJsonObject loaded = actor.executeTool(QStringLiteral("load_module_plugin"),
                                                         QJsonObject{{QStringLiteral("plugin"), plugin}});
            QVERIFY(loaded.value(QStringLiteral("status")).toString() == QLatin1String("loaded") ||
                    loaded.value(QStringLiteral("status")).toString() == QLatin1String("already_loaded"));
        }

        const auto add = [&actor](const QString& pluginId, const QString& instanceName) {
            return actor.executeTool(
                QStringLiteral("add_module"),
                QJsonObject{{QStringLiteral("plugin"), pluginId}, {QStringLiteral("instanceName"), instanceName}});
        };
        const QJsonObject producer =
            add(QStringLiteral("com.deeplux.plugin.createstring"), QStringLiteral("g8_queue_string_source"));
        const QJsonObject queueIn = add(QStringLiteral("com.deeplux.plugin.queuein"), QStringLiteral("g8_queue_in"));
        const QJsonObject queueOut = add(QStringLiteral("com.deeplux.plugin.queueout"), QStringLiteral("g8_queue_out"));
        QCOMPARE(producer.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        QCOMPARE(queueIn.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        QCOMPARE(queueOut.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString producerId = producer.value(QStringLiteral("instanceId")).toString();
        const QString queueInId = queueIn.value(QStringLiteral("instanceId")).toString();
        const QString queueOutId = queueOut.value(QStringLiteral("instanceId")).toString();

        QCOMPARE(setParams(actor, producerId,
                           QJsonObject{{QStringLiteral("stringSource"), QStringLiteral("Fixed")},
                                       {QStringLiteral("fixedString"), QStringLiteral("lot-42-检测")},
                                       {QStringLiteral("outputVarName"), QStringLiteral("message")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        QCOMPARE(setParams(actor, queueInId,
                           QJsonObject{{QStringLiteral("queueName"), queueName},
                                       {QStringLiteral("dataVariable"), QStringLiteral("message")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        QCOMPARE(setParams(actor, queueOutId,
                           QJsonObject{{QStringLiteral("queueName"), queueName},
                                       {QStringLiteral("outputVariable"), QStringLiteral("roundtrip")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        for (const QPair<QString, QString>& edge :
             {qMakePair(producerId, queueInId), qMakePair(queueInId, queueOutId)}) {
            const QJsonObject connected = actor.executeTool(
                QStringLiteral("connect_modules"), QJsonObject{{QStringLiteral("fromId"), edge.first},
                                                               {QStringLiteral("toId"), edge.second},
                                                               {QStringLiteral("fromPort"), QStringLiteral("image")},
                                                               {QStringLiteral("toPort"), QStringLiteral("image")}});
            QCOMPARE(connected.value(QStringLiteral("status")).toString(), QStringLiteral("connected"));
        }

        const QJsonObject success = runToTerminal(actor);
        QCOMPARE(success.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = success.value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());
        const auto output = [&actor, &runId](const QString& instanceId, const QString& key) {
            return actor.executeTool(QStringLiteral("get_module_output"),
                                     QJsonObject{{QStringLiteral("runId"), runId},
                                                 {QStringLiteral("instanceId"), instanceId},
                                                 {QStringLiteral("key"), key}});
        };
        const QJsonObject queuedSize = output(queueInId, QStringLiteral("queue_%1_size").arg(queueName));
        QCOMPARE(queuedSize.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(queuedSize.value(QStringLiteral("value")).toInt(), 1);
        const QJsonObject roundtrip = output(queueOutId, QStringLiteral("roundtrip"));
        QCOMPARE(roundtrip.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(roundtrip.value(QStringLiteral("value")).toString(), QStringLiteral("lot-42-检测"));
        const QJsonObject queueEmpty = output(queueOutId, QStringLiteral("queue_empty"));
        QCOMPARE(queueEmpty.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(queueEmpty.value(QStringLiteral("value")).toBool(), false);
        QCOMPARE(GlobalVarManager::instance().queueSize(queueName), 0);

        QCOMPARE(setEnabled(actor, producerId, false).value(QStringLiteral("status")).toString(),
                 QStringLiteral("updated"));
        const QJsonObject failed = runToTerminal(actor);
        QCOMPARE(failed.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QString failedRunId = failed.value(QStringLiteral("runId")).toString();
        const RunObservation observation = RunEngine::instance().runObservation(failedRunId);
        QCOMPARE(observation.result.failedModule, queueInId);
        QCOMPARE(observation.result.errorMessage, QStringLiteral("队列数据为空"));
        const QJsonObject unavailable =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{QStringLiteral("runId"), failedRunId},
                                          {QStringLiteral("instanceId"), queueInId},
                                          {QStringLiteral("key"), QStringLiteral("queue_%1_size").arg(queueName)}});
        QCOMPARE(unavailable.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
        QCOMPARE(unavailable.value(QStringLiteral("reason")).toString(), QStringLiteral("run_failed"));
        QCOMPARE(GlobalVarManager::instance().queueSize(queueName), 0);
    }

    void testAgentMeasuresGapFromHeightImageAndReportsNotFoundAndInvalidRoi() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();

        const QString groovePath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-groove-height.tiff"));
        const QString flatPath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-flat-height.tiff"));
        cv::Mat groove(480, 640, CV_32F, cv::Scalar(100.0f));
        for (int y = 200; y <= 280; ++y) {
            for (int x = 300; x <= 339; ++x)
                groove.at<float>(y, x) = 90.0f;
        }
        cv::Mat flat(480, 640, CV_32F, cv::Scalar(100.0f));
        QVERIFY(cv::imwrite(groovePath.toStdString(), groove));
        QVERIFY(cv::imwrite(flatPath.toStdString(), flat));

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 GapMeasure3D")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        for (const QString& plugin : {QStringLiteral("GrabImage"), QStringLiteral("GapMeasure3D")}) {
            const QJsonObject loaded = actor.executeTool(QStringLiteral("load_module_plugin"),
                                                         QJsonObject{{QStringLiteral("plugin"), plugin}});
            QVERIFY(loaded.value(QStringLiteral("status")).toString() == QLatin1String("loaded") ||
                    loaded.value(QStringLiteral("status")).toString() == QLatin1String("already_loaded"));
        }
        const QJsonObject grab =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.grabimage")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_gap_height_map")}});
        const QJsonObject gap =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.gapmeasure3d")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_gap_measure")}});
        QCOMPARE(grab.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        QCOMPARE(gap.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString grabId = grab.value(QStringLiteral("instanceId")).toString();
        const QString gapId = gap.value(QStringLiteral("instanceId")).toString();
        QCOMPARE(setParams(actor, grabId,
                           QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Path")},
                                       {QStringLiteral("filePath"), groovePath}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        QCOMPARE(setParams(actor, gapId,
                           QJsonObject{{QStringLiteral("roiCenterX"), 320},
                                       {QStringLiteral("roiCenterY"), 240},
                                       {QStringLiteral("roiLength"), 200},
                                       {QStringLiteral("roiHeight"), 5},
                                       {QStringLiteral("pixelSizeX"), 0.5},
                                       {QStringLiteral("offsetMm"), 0.25},
                                       {QStringLiteral("specUpperLimit"), 50.0},
                                       {QStringLiteral("measureFailValue"), -3.5}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject connected = actor.executeTool(
            QStringLiteral("connect_modules"), QJsonObject{{QStringLiteral("fromId"), grabId},
                                                           {QStringLiteral("toId"), gapId},
                                                           {QStringLiteral("fromPort"), QStringLiteral("image")},
                                                           {QStringLiteral("toPort"), QStringLiteral("image")}});
        QCOMPARE(connected.value(QStringLiteral("status")).toString(), QStringLiteral("connected"));

        const QJsonObject measured = runToTerminal(actor);
        QCOMPARE(measured.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString measuredRunId = measured.value(QStringLiteral("runId")).toString();
        const auto output = [&actor, &gapId](const QString& runId, const QString& key) {
            return actor.executeTool(QStringLiteral("get_module_output"),
                                     QJsonObject{{QStringLiteral("runId"), runId},
                                                 {QStringLiteral("instanceId"), gapId},
                                                 {QStringLiteral("key"), key}});
        };
        const auto expectNumber = [&output](const QString& runId, const QString& key, double expected) {
            const QJsonObject result = output(runId, key);
            if (result.value(QStringLiteral("status")).toString() != QLatin1String("available") ||
                std::abs(result.value(QStringLiteral("value")).toDouble() - expected) >= 0.75) {
                return false;
            }
            return true;
        };
        QCOMPARE(output(measuredRunId, QStringLiteral("gap_found")).value(QStringLiteral("status")).toString(),
                 QStringLiteral("available"));
        QCOMPARE(output(measuredRunId, QStringLiteral("gap_found")).value(QStringLiteral("value")).toBool(), true);
        QVERIFY(expectNumber(measuredRunId, QStringLiteral("gap_width"), 20.0));
        QVERIFY(expectNumber(measuredRunId, QStringLiteral("gap_offset_width"), 20.5));
        const QJsonObject width = output(measuredRunId, QStringLiteral("gap_width"));
        QCOMPARE(width.value(QStringLiteral("unit")).toString(), QStringLiteral("mm"));
        const QJsonObject offsetWidth = output(measuredRunId, QStringLiteral("gap_offset_width"));
        QCOMPARE(offsetWidth.value(QStringLiteral("unit")).toString(), QStringLiteral("mm"));
        const QJsonObject horizontal = output(measuredRunId, QStringLiteral("corner_dx_mm"));
        QCOMPARE(horizontal.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(horizontal.value(QStringLiteral("value")).toDouble(), width.value(QStringLiteral("value")).toDouble());
        QCOMPARE(horizontal.value(QStringLiteral("unit")).toString(), QStringLiteral("mm"));
        const QJsonObject pass = output(measuredRunId, QStringLiteral("is_pass"));
        QCOMPARE(pass.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(pass.value(QStringLiteral("value")).toBool(), true);
        const QJsonObject vertical = output(measuredRunId, QStringLiteral("corner_dz_mm"));
        QCOMPARE(vertical.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QVERIFY(!vertical.contains(QStringLiteral("unit")));
        const QJsonObject cornerDistance = output(measuredRunId, QStringLiteral("corner_dist_mm"));
        QCOMPARE(cornerDistance.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QVERIFY(!cornerDistance.contains(QStringLiteral("unit")));

        QCOMPARE(setParams(actor, grabId, QJsonObject{{QStringLiteral("filePath"), flatPath}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject notDetected = runToTerminal(actor);
        QCOMPARE(notDetected.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString notDetectedRunId = notDetected.value(QStringLiteral("runId")).toString();
        QCOMPARE(output(notDetectedRunId, QStringLiteral("gap_found")).value(QStringLiteral("status")).toString(),
                 QStringLiteral("available"));
        QCOMPARE(output(notDetectedRunId, QStringLiteral("gap_found")).value(QStringLiteral("value")).toBool(), false);
        const QJsonObject sentinel = output(notDetectedRunId, QStringLiteral("gap_width"));
        QCOMPARE(sentinel.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(sentinel.value(QStringLiteral("value")).toDouble(), -3.5);
        const QJsonObject notDetectedPass = output(notDetectedRunId, QStringLiteral("is_pass"));
        QCOMPARE(notDetectedPass.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(notDetectedPass.value(QStringLiteral("value")).toBool(), false);

        QCOMPARE(setParams(actor, gapId, QJsonObject{{QStringLiteral("roiCenterX"), 2000}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject failed = runToTerminal(actor);
        QCOMPARE(failed.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QString failedRunId = failed.value(QStringLiteral("runId")).toString();
        const RunObservation observation = RunEngine::instance().runObservation(failedRunId);
        QCOMPARE(observation.result.failedModule, gapId);
        QCOMPARE(observation.result.errorMessage, QStringLiteral("截面 ROI 与图像无交集"));
        const QJsonObject unavailable = output(failedRunId, QStringLiteral("gap_width"));
        QCOMPARE(unavailable.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
        QCOMPARE(unavailable.value(QStringLiteral("reason")).toString(), QStringLiteral("run_failed"));
    }

    void testAgentPerProcessingGaussianBlurAndRejectsUnsupportedTypeAndMissingImage() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();

        const QString sourcePath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-perprocessing-source.png"));
        cv::Mat source(9, 9, CV_8UC3);
        for (int y = 0; y < source.rows; ++y) {
            for (int x = 0; x < source.cols; ++x) {
                const uchar value = static_cast<uchar>(std::min(255, 10 + 17 * x + 11 * y + ((x * y * 13) % 73)));
                source.at<cv::Vec3b>(y, x) = cv::Vec3b(value, value, value);
            }
        }
        QVERIFY(cv::imwrite(sourcePath.toStdString(), source));

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 PerProcessing")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        for (const QString& plugin : {QStringLiteral("GrabImage"), QStringLiteral("PerProcessing")}) {
            const QJsonObject loaded = actor.executeTool(QStringLiteral("load_module_plugin"),
                                                         QJsonObject{{QStringLiteral("plugin"), plugin}});
            QVERIFY(loaded.value(QStringLiteral("status")).toString() == QLatin1String("loaded") ||
                    loaded.value(QStringLiteral("status")).toString() == QLatin1String("already_loaded"));
        }
        const QJsonObject grab =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.grabimage")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_filter_source")}});
        const QJsonObject filter = actor.executeTool(
            QStringLiteral("add_module"),
            QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.perprocessing")},
                        {QStringLiteral("instanceName"), QStringLiteral("g8_filter")}});
        QCOMPARE(grab.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        QCOMPARE(filter.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString grabId = grab.value(QStringLiteral("instanceId")).toString();
        const QString filterId = filter.value(QStringLiteral("instanceId")).toString();
        QCOMPARE(setParams(actor, grabId,
                           QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Path")},
                                       {QStringLiteral("filePath"), sourcePath}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const auto setFilterParams = [&actor, &filterId](const QJsonObject& values) {
            return setParams(actor, filterId, values);
        };
        QCOMPARE(setFilterParams(QJsonObject{{QStringLiteral("processType"), QStringLiteral("GaussianBlur")},
                                             {QStringLiteral("kernelSize"), 3},
                                             {QStringLiteral("sigmaX"), 1.2},
                                             {QStringLiteral("iterations"), 1}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject connected = actor.executeTool(
            QStringLiteral("connect_modules"), QJsonObject{{QStringLiteral("fromId"), grabId},
                                                           {QStringLiteral("toId"), filterId},
                                                           {QStringLiteral("fromPort"), QStringLiteral("image")},
                                                           {QStringLiteral("toPort"), QStringLiteral("image")}});
        QCOMPARE(connected.value(QStringLiteral("status")).toString(), QStringLiteral("connected"));

        const QJsonObject successfulRun = runToTerminal(actor);
        QCOMPARE(successfulRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = successfulRun.value(QStringLiteral("runId")).toString();
        const auto output = [&actor, &filterId](const QString& id, const QString& key) {
            return actor.executeTool(QStringLiteral("get_module_output"),
                                     QJsonObject{{QStringLiteral("runId"), id},
                                                 {QStringLiteral("instanceId"), filterId},
                                                 {QStringLiteral("key"), key}});
        };
        QCOMPARE(output(runId, QStringLiteral("processed_type")).value(QStringLiteral("value")).toString(),
                 QStringLiteral("GaussianBlur"));
        QCOMPARE(output(runId, QStringLiteral("processed_width")).value(QStringLiteral("value")).toInt(), 9);
        QCOMPARE(output(runId, QStringLiteral("processed_height")).value(QStringLiteral("value")).toInt(), 9);
        ImageData grabOutput;
        ImageData filterOutput;
        QMap<QString, QVariant> grabValues;
        QMap<QString, QVariant> filterValues;
        PortValueMap grabPorts;
        PortValueMap filterPorts;
        QVERIFY(RunEngine::instance().runOutputSnapshot(runId, grabId, grabOutput, grabValues, grabPorts));
        QVERIFY(RunEngine::instance().runOutputSnapshot(runId, filterId, filterOutput, filterValues, filterPorts));
        const ImageCoordinateProvenance inputProvenance = grabOutput.coordinateProvenance();
        const ImageCoordinateProvenance outputProvenance = filterOutput.coordinateProvenance();
        QVERIFY(inputProvenance.isValid());
        QVERIFY(!inputProvenance.sourceImageId.isEmpty());
        QCOMPARE(inputProvenance.sourceSize, QSize(9, 9));
        QCOMPARE(outputProvenance.sourceImageId, inputProvenance.sourceImageId);
        QCOMPARE(outputProvenance.sourceSize, inputProvenance.sourceSize);
        QCOMPARE(outputProvenance.localToSourceMatrix(), inputProvenance.localToSourceMatrix());
        cv::Mat expected;
        cv::GaussianBlur(source, expected, cv::Size(3, 3), 1.2, 0, cv::BORDER_DEFAULT);
        const cv::Mat actual = filterOutput.toMat();
        QCOMPARE(actual.size(), expected.size());
        cv::Mat difference;
        cv::absdiff(actual, expected, difference);
        QCOMPARE(cv::countNonZero(difference.reshape(1)), 0);
        cv::Mat changed;
        cv::absdiff(actual, source, changed);
        QVERIFY(cv::countNonZero(changed.reshape(1)) > 0);

        const auto expectedForMode = [&source](const QString& mode) {
            cv::Mat expected;
            cv::Mat gray;
            if (mode == QLatin1String("GaussianBlur")) {
                cv::GaussianBlur(source, expected, cv::Size(3, 3), 1.2);
            } else if (mode == QLatin1String("MedianBlur")) {
                cv::medianBlur(source, expected, 3);
            } else if (mode == QLatin1String("BilateralFilter")) {
                cv::bilateralFilter(source, expected, 3, 20.0, 5.0);
            } else if (mode == QLatin1String("Sobel")) {
                cv::cvtColor(source, gray, cv::COLOR_BGR2GRAY);
                cv::Sobel(gray, expected, CV_16S, 1, 1);
                cv::convertScaleAbs(expected, expected);
            } else if (mode == QLatin1String("Laplacian")) {
                cv::cvtColor(source, gray, cv::COLOR_BGR2GRAY);
                cv::Laplacian(gray, expected, CV_16S, 3);
                cv::convertScaleAbs(expected, expected);
            } else if (mode == QLatin1String("Canny")) {
                cv::cvtColor(source, gray, cv::COLOR_BGR2GRAY);
                cv::Canny(gray, expected, 50, 150, 3);
            } else {
                const QMap<QString, int> morphologyOperations = {{QStringLiteral("Dilate"), cv::MORPH_DILATE},
                                                                 {QStringLiteral("Erode"), cv::MORPH_ERODE},
                                                                 {QStringLiteral("Open"), cv::MORPH_OPEN},
                                                                 {QStringLiteral("Close"), cv::MORPH_CLOSE},
                                                                 {QStringLiteral("Gradient"), cv::MORPH_GRADIENT}};
                const auto operation = morphologyOperations.constFind(mode);
                if (operation == morphologyOperations.cend())
                    return cv::Mat();
                const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
                cv::morphologyEx(source, expected, operation.value(), kernel, cv::Point(-1, -1), 1);
            }
            return expected;
        };
        const auto pixelSignature = [](const cv::Mat& image) {
            QByteArray signature;
            signature.reserve(static_cast<int>(image.total() * image.elemSize()));
            const int rowBytes = image.cols * static_cast<int>(image.elemSize());
            for (int row = 0; row < image.rows; ++row)
                signature.append(reinterpret_cast<const char*>(image.ptr(row)), rowBytes);
            return signature;
        };
        QList<QByteArray> modeSignatures{pixelSignature(actual)};
        const QStringList remainingModes = {QStringLiteral("MedianBlur"), QStringLiteral("BilateralFilter"),
                                            QStringLiteral("Sobel"),      QStringLiteral("Laplacian"),
                                            QStringLiteral("Canny"),      QStringLiteral("Dilate"),
                                            QStringLiteral("Erode"),      QStringLiteral("Open"),
                                            QStringLiteral("Close"),      QStringLiteral("Gradient")};
        for (const QString& mode : remainingModes) {
            const double sigmaX = mode == QLatin1String("BilateralFilter") ? 10.0 : 1.2;
            QCOMPARE(setFilterParams(QJsonObject{{QStringLiteral("processType"), mode},
                                                 {QStringLiteral("kernelSize"), 3},
                                                 {QStringLiteral("sigmaX"), sigmaX},
                                                 {QStringLiteral("iterations"), 1}})
                         .value(QStringLiteral("status"))
                         .toString(),
                     QStringLiteral("params_set"));
            const QJsonObject modeRun = runToTerminal(actor);
            QCOMPARE(modeRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
            const QString modeRunId = modeRun.value(QStringLiteral("runId")).toString();
            QVERIFY(!modeRunId.isEmpty());
            const QJsonObject processedType = output(modeRunId, QStringLiteral("processed_type"));
            QCOMPARE(processedType.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(processedType.value(QStringLiteral("value")).toString(), mode);
            QCOMPARE(output(modeRunId, QStringLiteral("processed_width")).value(QStringLiteral("value")).toInt(), 9);
            QCOMPARE(output(modeRunId, QStringLiteral("processed_height")).value(QStringLiteral("value")).toInt(), 9);

            ImageData modeGrabOutput;
            ImageData modeFilterOutput;
            QMap<QString, QVariant> modeGrabValues;
            QMap<QString, QVariant> modeFilterValues;
            PortValueMap modeGrabPorts;
            PortValueMap modeFilterPorts;
            QVERIFY(RunEngine::instance().runOutputSnapshot(modeRunId, grabId, modeGrabOutput, modeGrabValues,
                                                            modeGrabPorts));
            QVERIFY(RunEngine::instance().runOutputSnapshot(modeRunId, filterId, modeFilterOutput, modeFilterValues,
                                                            modeFilterPorts));
            const cv::Mat modeActual = modeFilterOutput.toMat();
            const cv::Mat modeExpected = expectedForMode(mode);
            QVERIFY(!modeExpected.empty());
            QCOMPARE(modeActual.size(), modeExpected.size());
            QCOMPARE(modeActual.type(), modeExpected.type());
            QCOMPARE(modeActual.channels(), mode == QLatin1String("Sobel") || mode == QLatin1String("Laplacian") ||
                                                    mode == QLatin1String("Canny")
                                                ? 1
                                                : 3);
            cv::Mat modeDifference;
            cv::absdiff(modeActual, modeExpected, modeDifference);
            QCOMPARE(cv::countNonZero(modeDifference.reshape(1)), 0);
            QVERIFY(!modeSignatures.contains(pixelSignature(modeActual)));
            modeSignatures.append(pixelSignature(modeActual));

            cv::Mat comparableInput;
            if (modeActual.channels() == 1)
                cv::cvtColor(source, comparableInput, cv::COLOR_BGR2GRAY);
            else
                comparableInput = source;
            cv::Mat sourceDifference;
            cv::absdiff(modeActual, comparableInput, sourceDifference);
            QVERIFY(cv::countNonZero(sourceDifference.reshape(1)) > 0);

            const ImageCoordinateProvenance modeInputProvenance = modeGrabOutput.coordinateProvenance();
            const ImageCoordinateProvenance modeOutputProvenance = modeFilterOutput.coordinateProvenance();
            QCOMPARE(modeOutputProvenance.sourceImageId, modeInputProvenance.sourceImageId);
            QCOMPARE(modeOutputProvenance.sourceSize, modeInputProvenance.sourceSize);
            QCOMPARE(modeOutputProvenance.localToSourceMatrix(), modeInputProvenance.localToSourceMatrix());
        }
        QCOMPARE(modeSignatures.size(), 11);
        QCOMPARE(setFilterParams(QJsonObject{{QStringLiteral("processType"), QStringLiteral("GaussianBlur")},
                                             {QStringLiteral("kernelSize"), 3},
                                             {QStringLiteral("sigmaX"), 1.2},
                                             {QStringLiteral("iterations"), 1}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));

        const auto moduleParams = [&filterId]() {
            return ProjectManager::instance().currentProject()->moduleById(filterId);
        };
        const auto expectAccepted = [&setFilterParams, &moduleParams](const QJsonObject& values) {
            if (setFilterParams(values).value(QStringLiteral("status")).toString() != QLatin1String("params_set"))
                return false;
            const std::optional<ModuleInstance> current = moduleParams();
            if (!current.has_value())
                return false;
            for (auto it = values.begin(); it != values.end(); ++it) {
                const QJsonValue actualValue = current->params.value(it.key());
                if (!current->params.contains(it.key()))
                    return false;
                if (it.value().isDouble()) {
                    if (actualValue.toDouble() != it.value().toDouble())
                        return false;
                } else if (actualValue != it.value()) {
                    return false;
                }
            }
            return true;
        };
        QVERIFY(expectAccepted(QJsonObject{
            {QStringLiteral("kernelSize"), 1}, {QStringLiteral("sigmaX"), 0.0}, {QStringLiteral("iterations"), 1}}));
        QVERIFY(expectAccepted(QJsonObject{{QStringLiteral("kernelSize"), 31},
                                           {QStringLiteral("sigmaX"), 100.0},
                                           {QStringLiteral("iterations"), 10}}));
        QVERIFY(expectAccepted(QJsonObject{
            {QStringLiteral("kernelSize"), 3}, {QStringLiteral("sigmaX"), 1.2}, {QStringLiteral("iterations"), 1}}));
        const QList<QPair<QJsonObject, QString>> rejectedParams = {
            {QJsonObject{{QStringLiteral("kernelSize"), 0}}, QStringLiteral("kernelSize 必须为 1–31 的整数")},
            {QJsonObject{{QStringLiteral("kernelSize"), 32}}, QStringLiteral("kernelSize 必须为 1–31 的整数")},
            {QJsonObject{{QStringLiteral("kernelSize"), 1.5}}, QStringLiteral("kernelSize 必须为 1–31 的整数")},
            {QJsonObject{{QStringLiteral("sigmaX"), -0.1}}, QStringLiteral("sigmaX 必须为 [0,100] 的有限数")},
            {QJsonObject{{QStringLiteral("sigmaX"), 100.1}}, QStringLiteral("sigmaX 必须为 [0,100] 的有限数")},
            {QJsonObject{{QStringLiteral("iterations"), 0}}, QStringLiteral("iterations 必须为 1–10 的整数")},
            {QJsonObject{{QStringLiteral("iterations"), 11}}, QStringLiteral("iterations 必须为 1–10 的整数")},
            {QJsonObject{{QStringLiteral("iterations"), 1.5}}, QStringLiteral("iterations 必须为 1–10 的整数")}};
        for (const auto& rejectedParam : rejectedParams) {
            const QJsonObject result = setFilterParams(rejectedParam.first);
            QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("VALIDATION_FAILED"));
            QCOMPARE(result.value(QStringLiteral("error")).toString(), rejectedParam.second);
            const std::optional<ModuleInstance> current = moduleParams();
            QVERIFY(current.has_value());
            const QString changedKey = rejectedParam.first.keys().first();
            QCOMPARE(current->params.value(changedKey), QJsonValue(changedKey == QLatin1String("kernelSize")   ? 3
                                                                   : changedKey == QLatin1String("iterations") ? 1
                                                                                                               : 1.2));
        }

        QSignalSpy moduleStarted(&RunEngine::instance(), &RunEngine::moduleStarted);
        const QJsonObject unsupported =
            setFilterParams(QJsonObject{{QStringLiteral("processType"), QStringLiteral("UnsupportedFilter")}});
        QCOMPARE(unsupported.value(QStringLiteral("errorCode")).toString(), QStringLiteral("VALIDATION_FAILED"));
        QCOMPARE(unsupported.value(QStringLiteral("error")).toString(),
                 QStringLiteral("不支持的处理类型: UnsupportedFilter"));
        const std::optional<ModuleInstance> unchanged = moduleParams();
        QVERIFY(unchanged.has_value());
        QCOMPARE(unchanged->params.value(QStringLiteral("processType")).toString(), QStringLiteral("GaussianBlur"));
        QVERIFY(!RunEngine::instance().isBusy());
        QCOMPARE(moduleStarted.count(), 0);

        const QJsonObject disconnected = actor.executeTool(
            QStringLiteral("disconnect_modules"), QJsonObject{{QStringLiteral("fromId"), grabId},
                                                              {QStringLiteral("toId"), filterId},
                                                              {QStringLiteral("fromPort"), QStringLiteral("image")},
                                                              {QStringLiteral("toPort"), QStringLiteral("image")}});
        QCOMPARE(disconnected.value(QStringLiteral("status")).toString(), QStringLiteral("disconnected"));
        QCOMPARE(setEnabled(actor, grabId, false).value(QStringLiteral("status")).toString(),
                 QStringLiteral("updated"));
        const QJsonObject failed = runToTerminal(actor);
        QCOMPARE(failed.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QString failedRunId = failed.value(QStringLiteral("runId")).toString();
        const RunObservation observation = RunEngine::instance().runObservation(failedRunId);
        QCOMPARE(observation.result.failedModule, filterId);
        QCOMPARE(observation.result.errorMessage, QStringLiteral("输入图像无效"));
        const QJsonObject unavailable = output(failedRunId, QStringLiteral("processed_type"));
        QCOMPARE(unavailable.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
        QCOMPARE(unavailable.value(QStringLiteral("reason")).toString(), QStringLiteral("run_failed"));
    }

    void testAgentPreProcessing3DReportsHeightMapAndRejectsColorInput() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();

        const QString heightPath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-height-map.tiff"));
        const QString colorPath = QDir(m_appDataDir.path()).filePath(QStringLiteral("g8-color-not-height.png"));
        cv::Mat height(7, 9, CV_32F);
        for (int y = 0; y < height.rows; ++y) {
            for (int x = 0; x < height.cols; ++x)
                height.at<float>(y, x) = static_cast<float>(10 + 2 * x + 3 * y);
        }
        cv::Mat color(7, 9, CV_8UC3, cv::Scalar(20, 100, 220));
        QVERIFY(cv::imwrite(heightPath.toStdString(), height));
        QVERIFY(cv::imwrite(colorPath.toStdString(), color));

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 3DPreProcessing")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        for (const QString& plugin : {QStringLiteral("GrabImage"), QStringLiteral("3DPreProcessing")}) {
            const QJsonObject loaded = actor.executeTool(QStringLiteral("load_module_plugin"),
                                                         QJsonObject{{QStringLiteral("plugin"), plugin}});
            QVERIFY(loaded.value(QStringLiteral("status")).toString() == QLatin1String("loaded") ||
                    loaded.value(QStringLiteral("status")).toString() == QLatin1String("already_loaded"));
        }
        const QJsonObject grab =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.grabimage")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_height_source")}});
        const QJsonObject preprocessing = actor.executeTool(
            QStringLiteral("add_module"),
            QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.3dpreprocessing")},
                        {QStringLiteral("instanceName"), QStringLiteral("g8_height_preprocess")}});
        QCOMPARE(grab.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        QCOMPARE(preprocessing.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString grabId = grab.value(QStringLiteral("instanceId")).toString();
        const QString preprocessingId = preprocessing.value(QStringLiteral("instanceId")).toString();
        QCOMPARE(setParams(actor, grabId,
                           QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Path")},
                                       {QStringLiteral("filePath"), heightPath}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        QCOMPARE(setParams(actor, preprocessingId,
                           QJsonObject{{QStringLiteral("heightFilterEnabled"), false},
                                       {QStringLiteral("autoNoData"), true},
                                       {QStringLiteral("roiWidth"), 0},
                                       {QStringLiteral("roiHeight"), 0}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject connected = actor.executeTool(
            QStringLiteral("connect_modules"), QJsonObject{{QStringLiteral("fromId"), grabId},
                                                           {QStringLiteral("toId"), preprocessingId},
                                                           {QStringLiteral("fromPort"), QStringLiteral("image")},
                                                           {QStringLiteral("toPort"), QStringLiteral("image")}});
        QCOMPARE(connected.value(QStringLiteral("status")).toString(), QStringLiteral("connected"));

        const QJsonArray outputPorts = actor
                                           .executeTool(QStringLiteral("get_module_params_schema"),
                                                        QJsonObject{{QStringLiteral("instanceId"), preprocessingId}})
                                           .value(QStringLiteral("ports"))
                                           .toObject()
                                           .value(QStringLiteral("outputs"))
                                           .toArray();
        const auto imagePort = std::find_if(outputPorts.cbegin(), outputPorts.cend(), [](const QJsonValue& value) {
            return value.toObject().value(QStringLiteral("id")).toString() == QLatin1String("image");
        });
        QVERIFY(imagePort != outputPorts.cend());
        QCOMPARE(imagePort->toObject().value(QStringLiteral("type")).toString(), QStringLiteral("HeightMap2D"));

        const QJsonObject successfulRun = runToTerminal(actor);
        QCOMPARE(successfulRun.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = successfulRun.value(QStringLiteral("runId")).toString();
        const auto output = [&actor, &preprocessingId](const QString& id, const QString& key) {
            return actor.executeTool(QStringLiteral("get_module_output"),
                                     QJsonObject{{QStringLiteral("runId"), id},
                                                 {QStringLiteral("instanceId"), preprocessingId},
                                                 {QStringLiteral("key"), key}});
        };
        for (const auto& expected :
             {qMakePair(QStringLiteral("valid_pixel_count"), 63.0),
              qMakePair(QStringLiteral("filtered_pixel_count"), 0.0), qMakePair(QStringLiteral("min_height"), 10.0),
              qMakePair(QStringLiteral("max_height"), 44.0)}) {
            const QJsonObject result = output(runId, expected.first);
            QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(result.value(QStringLiteral("value")).toDouble(), expected.second);
        }
        const QJsonObject imagePortOutput = actor.executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                             {QStringLiteral("instanceId"), preprocessingId},
                                                             {QStringLiteral("port"), QStringLiteral("image")}});
        QCOMPARE(imagePortOutput.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(imagePortOutput.value(QStringLiteral("source")).toString(), QStringLiteral("port"));
        const QJsonObject imageSummary = imagePortOutput.value(QStringLiteral("value")).toObject();
        QCOMPARE(imageSummary.value(QStringLiteral("width")).toInt(), 9);
        QCOMPARE(imageSummary.value(QStringLiteral("height")).toInt(), 7);
        ImageData preprocessed;
        QMap<QString, QVariant> values;
        PortValueMap ports;
        QVERIFY(RunEngine::instance().runOutputSnapshot(runId, preprocessingId, preprocessed, values, ports));
        QCOMPARE(preprocessed.width(), 9);
        QCOMPARE(preprocessed.height(), 7);
        const cv::Mat actual = preprocessed.toMat();
        cv::Mat difference;
        cv::absdiff(actual, height, difference);
        QCOMPARE(cv::countNonZero(difference.reshape(1)), 0);

        QCOMPARE(setParams(actor, grabId, QJsonObject{{QStringLiteral("filePath"), colorPath}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const QJsonObject failed = runToTerminal(actor);
        QCOMPARE(failed.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        const QString failedRunId = failed.value(QStringLiteral("runId")).toString();
        const RunObservation observation = RunEngine::instance().runObservation(failedRunId);
        QCOMPARE(observation.result.failedModule, preprocessingId);
        QCOMPARE(observation.result.errorMessage, QStringLiteral("高度图须为 1 或 2 通道，收到 3 通道"));
        const QJsonObject unavailable = output(failedRunId, QStringLiteral("valid_pixel_count"));
        QCOMPARE(unavailable.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
        QCOMPARE(unavailable.value(QStringLiteral("reason")).toString(), QStringLiteral("run_failed"));
    }

    void testAgentWhileFalseConditionAndRejectsInvalidComparisonAndLimit() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 While")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        for (const QString& plugin : {QStringLiteral("创建字符串"), QStringLiteral("条件循环")}) {
            const QString status =
                actor.executeTool(QStringLiteral("load_module_plugin"), QJsonObject{{QStringLiteral("plugin"), plugin}})
                    .value(QStringLiteral("status"))
                    .toString();
            QVERIFY(status == QLatin1String("loaded") || status == QLatin1String("already_loaded"));
        }

        const QJsonObject created =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.createstring")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_empty_condition")}});
        QCOMPARE(created.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString createId = created.value(QStringLiteral("instanceId")).toString();
        const QJsonObject whileModule =
            actor.executeTool(QStringLiteral("add_module"),
                              QJsonObject{{QStringLiteral("plugin"), QStringLiteral("com.deeplux.plugin.while")},
                                          {QStringLiteral("instanceName"), QStringLiteral("g8_while")}});
        QCOMPARE(whileModule.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString whileId = whileModule.value(QStringLiteral("instanceId")).toString();
        QCOMPARE(setParams(actor, createId,
                           QJsonObject{{QStringLiteral("fixedString"), QString()},
                                       {QStringLiteral("outputVarName"), QStringLiteral("loopGate")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        QCOMPARE(setParams(actor, whileId,
                           QJsonObject{{QStringLiteral("conditionVariable"), QStringLiteral("loopGate")},
                                       {QStringLiteral("comparison"), QStringLiteral("NotEmpty")},
                                       {QStringLiteral("compareValue"), QString()},
                                       {QStringLiteral("maxIterations"), 1}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        const std::optional<ModuleInstance> minimumIteration =
            ProjectManager::instance().currentProject()->moduleById(whileId);
        QVERIFY(minimumIteration.has_value());
        QCOMPARE(minimumIteration->params.value(QStringLiteral("maxIterations")).toInt(), 1);
        QCOMPARE(actor
                     .executeTool(QStringLiteral("connect_modules"),
                                  QJsonObject{{QStringLiteral("fromId"), createId},
                                              {QStringLiteral("toId"), whileId},
                                              {QStringLiteral("fromPort"), QStringLiteral("image")},
                                              {QStringLiteral("toPort"), QStringLiteral("image")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("connected"));

        const QJsonObject completed = runToTerminal(actor);
        QCOMPARE(completed.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QString runId = completed.value(QStringLiteral("runId")).toString();
        const auto output = [&actor](const QString& id, const QString& instanceId, const QString& key) {
            return actor.executeTool(QStringLiteral("get_module_output"),
                                     QJsonObject{{QStringLiteral("runId"), id},
                                                 {QStringLiteral("instanceId"), instanceId},
                                                 {QStringLiteral("key"), key}});
        };
        const QJsonObject loopGate = output(runId, createId, QStringLiteral("loopGate"));
        QCOMPARE(loopGate.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(loopGate.value(QStringLiteral("value")).toString(), QString());
        const QJsonObject whileResult = output(runId, whileId, QStringLiteral("while_result"));
        QCOMPARE(whileResult.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(whileResult.value(QStringLiteral("value")).toBool(), false);

        const auto currentInstance = [&]() { return ProjectManager::instance().currentProject()->moduleById(whileId); };
        for (const int accepted : {2, 100000}) {
            QCOMPARE(setParams(actor, whileId, QJsonObject{{QStringLiteral("maxIterations"), accepted}})
                         .value(QStringLiteral("status"))
                         .toString(),
                     QStringLiteral("params_set"));
            const std::optional<ModuleInstance> current = currentInstance();
            QVERIFY(current.has_value());
            QCOMPARE(current->params.value(QStringLiteral("maxIterations")).toInt(), accepted);
        }
        for (const double rejected : {0.0, 100001.0, 1.5}) {
            const QJsonObject result =
                setParams(actor, whileId, QJsonObject{{QStringLiteral("maxIterations"), rejected}});
            QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("VALIDATION_FAILED"));
            QCOMPARE(result.value(QStringLiteral("error")).toString(),
                     QStringLiteral("maxIterations 必须为 1–100000 的整数"));
            const std::optional<ModuleInstance> current = currentInstance();
            QVERIFY(current.has_value());
            QCOMPARE(current->params.value(QStringLiteral("maxIterations")).toInt(), 100000);
        }

        QSignalSpy moduleStarted(&RunEngine::instance(), &RunEngine::moduleStarted);
        const QJsonObject unsupported = setParams(
            actor, whileId, QJsonObject{{QStringLiteral("comparison"), QStringLiteral("UnsupportedComparison")}});
        QCOMPARE(unsupported.value(QStringLiteral("errorCode")).toString(), QStringLiteral("VALIDATION_FAILED"));
        QCOMPARE(unsupported.value(QStringLiteral("error")).toString(),
                 QStringLiteral("comparison 不支持: UnsupportedComparison"));
        const std::optional<ModuleInstance> current = currentInstance();
        QVERIFY(current.has_value());
        QCOMPARE(current->params.value(QStringLiteral("comparison")).toString(), QStringLiteral("NotEmpty"));
        QVERIFY(!RunEngine::instance().isBusy());
        QCOMPARE(moduleStarted.count(), 0);
    }

    void testAgentStopWhileExitsWhileBodyAndContinuesAfterFlow() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();

        AgentActor actor;
        QCOMPARE(actor
                     .executeTool(QStringLiteral("create_project"),
                                  QJsonObject{{QStringLiteral("name"), QStringLiteral("G8 StopWhile")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("created"));
        for (const QString& plugin : {QStringLiteral("创建字符串"), QStringLiteral("条件循环"),
                                      QStringLiteral("停止循环"), QStringLiteral("延时")}) {
            const QString status =
                actor.executeTool(QStringLiteral("load_module_plugin"), QJsonObject{{QStringLiteral("plugin"), plugin}})
                    .value(QStringLiteral("status"))
                    .toString();
            QVERIFY(status == QLatin1String("loaded") || status == QLatin1String("already_loaded"));
        }

        const auto add = [&actor](const QString& plugin, const QString& instanceName) {
            return actor.executeTool(
                QStringLiteral("add_module"),
                QJsonObject{{QStringLiteral("plugin"), plugin}, {QStringLiteral("instanceName"), instanceName}});
        };
        const QJsonObject source =
            add(QStringLiteral("com.deeplux.plugin.createstring"), QStringLiteral("g8_while_source"));
        const QJsonObject whileModule = add(QStringLiteral("com.deeplux.plugin.while"), QStringLiteral("g8_while"));
        const QJsonObject body = add(QStringLiteral("com.deeplux.plugin.delay"), QStringLiteral("g8_body"));
        const QJsonObject stop = add(QStringLiteral("com.deeplux.plugin.stopwhile"), QStringLiteral("g8_stop"));
        const QJsonObject after = add(QStringLiteral("com.deeplux.plugin.delay"), QStringLiteral("g8_after"));
        for (const QJsonObject& result : {source, whileModule, body, stop, after})
            QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("added"));
        const QString sourceId = source.value(QStringLiteral("instanceId")).toString();
        const QString whileId = whileModule.value(QStringLiteral("instanceId")).toString();
        const QString bodyId = body.value(QStringLiteral("instanceId")).toString();
        const QString stopId = stop.value(QStringLiteral("instanceId")).toString();
        const QString afterId = after.value(QStringLiteral("instanceId")).toString();
        QCOMPARE(setParams(actor, sourceId,
                           QJsonObject{{QStringLiteral("fixedString"), QString()},
                                       {QStringLiteral("outputVarName"), QStringLiteral("whileGate")}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        QCOMPARE(setParams(actor, whileId,
                           QJsonObject{{QStringLiteral("conditionVariable"), QStringLiteral("whileGate")},
                                       {QStringLiteral("comparison"), QStringLiteral("NotEmpty")},
                                       {QStringLiteral("compareValue"), QString()},
                                       {QStringLiteral("maxIterations"), 1000}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("params_set"));
        for (const QString& instanceId : {bodyId, afterId}) {
            QCOMPARE(setParams(actor, instanceId, QJsonObject{{QStringLiteral("delayMs"), 1}})
                         .value(QStringLiteral("status"))
                         .toString(),
                     QStringLiteral("params_set"));
        }

        const auto connect = [&actor](const QString& fromId, const QString& fromPort, const QString& toId) {
            return actor.executeTool(QStringLiteral("connect_modules"),
                                     QJsonObject{{QStringLiteral("fromId"), fromId},
                                                 {QStringLiteral("fromPort"), fromPort},
                                                 {QStringLiteral("toId"), toId},
                                                 {QStringLiteral("toPort"), QStringLiteral("control")}});
        };
        for (const QJsonObject& connected :
             {connect(whileId, QStringLiteral("body"), bodyId), connect(bodyId, QStringLiteral("next"), stopId),
              connect(stopId, QStringLiteral("stop"), afterId), connect(whileId, QStringLiteral("done"), afterId)}) {
            QCOMPARE(connected.value(QStringLiteral("status")).toString(), QStringLiteral("connected"));
            QCOMPARE(connected.value(QStringLiteral("edgeType")).toString(), QStringLiteral("control"));
        }
        const QJsonObject imageConnected = actor.executeTool(
            QStringLiteral("connect_modules"), QJsonObject{{QStringLiteral("fromId"), sourceId},
                                                           {QStringLiteral("fromPort"), QStringLiteral("image")},
                                                           {QStringLiteral("toId"), whileId},
                                                           {QStringLiteral("toPort"), QStringLiteral("image")}});
        QCOMPARE(imageConnected.value(QStringLiteral("status")).toString(), QStringLiteral("connected"));
        QCOMPARE(imageConnected.value(QStringLiteral("edgeType")).toString(), QStringLiteral("data"));
        const QJsonObject wrongEdge = connect(afterId, QStringLiteral("image"), bodyId);
        QCOMPARE(wrongEdge.value(QStringLiteral("errorCode")).toString(), QStringLiteral("VALIDATION_FAILED"));
        const QJsonObject state = actor.executeTool(QStringLiteral("get_flow_state"), {});
        const QJsonArray connections = state.value(QStringLiteral("connections")).toArray();
        QCOMPARE(connections.size(), 5);
        const auto hasConnection = [&connections](const QString& from, const QString& fromPort, const QString& to,
                                                  const QString& toPort, const QString& expectedEdgeType) {
            return std::any_of(connections.cbegin(), connections.cend(), [&](const QJsonValue& value) {
                const QJsonObject edge = value.toObject();
                return edge.value(QStringLiteral("from")).toString() == from &&
                       edge.value(QStringLiteral("fromPort")).toString() == fromPort &&
                       edge.value(QStringLiteral("to")).toString() == to &&
                       edge.value(QStringLiteral("toPort")).toString() == toPort &&
                       edge.value(QStringLiteral("edgeType")).toString() == expectedEdgeType;
            });
        };
        QVERIFY(
            hasConnection(sourceId, QStringLiteral("image"), whileId, QStringLiteral("image"), QStringLiteral("data")));
        QVERIFY(hasConnection(whileId, QStringLiteral("body"), bodyId, QStringLiteral("control"),
                              QStringLiteral("control")));
        QVERIFY(hasConnection(bodyId, QStringLiteral("next"), stopId, QStringLiteral("control"),
                              QStringLiteral("control")));
        QVERIFY(hasConnection(stopId, QStringLiteral("stop"), afterId, QStringLiteral("control"),
                              QStringLiteral("control")));
        QVERIFY(hasConnection(whileId, QStringLiteral("done"), afterId, QStringLiteral("control"),
                              QStringLiteral("control")));
        QSignalSpy moduleStarted(&RunEngine::instance(), &RunEngine::moduleStarted);
        const auto startedCount = [&moduleStarted](const QString& instanceId) {
            return std::count_if(moduleStarted.cbegin(), moduleStarted.cend(), [&](const QList<QVariant>& arguments) {
                return !arguments.isEmpty() && arguments.first().toString() == instanceId;
            });
        };
        const auto output = [&actor](const QString& runId, const QString& instanceId, const QString& key) {
            return actor.executeTool(QStringLiteral("get_module_output"),
                                     QJsonObject{{QStringLiteral("runId"), runId},
                                                 {QStringLiteral("instanceId"), instanceId},
                                                 {QStringLiteral("key"), key}});
        };
        const auto parameterUpdateAccepted = [](const QString& status) {
            return status == QLatin1String("params_set") || status == QLatin1String("no_op");
        };
        struct ComparatorCase {
            const char* comparison;
            const char* inputValue;
            const char* compareValue;
        };
        const ComparatorCase comparatorCases[] = {
            {"NotEmpty", "gate", ""},      {"Empty", "", ""},          {"Equal", "match", "match"},
            {"NotEqual", "left", "right"}, {"GreaterThan", "42", "9"}, {"LessThan", "2", "10"},
        };
        for (const ComparatorCase& comparatorCase : comparatorCases) {
            const QString inputValue = QString::fromLatin1(comparatorCase.inputValue);
            QVERIFY(parameterUpdateAccepted(
                setParams(actor, sourceId, QJsonObject{{QStringLiteral("fixedString"), inputValue}})
                    .value(QStringLiteral("status"))
                    .toString()));
            QVERIFY(parameterUpdateAccepted(
                setParams(
                    actor, whileId,
                    QJsonObject{{QStringLiteral("conditionVariable"), QStringLiteral("whileGate")},
                                {QStringLiteral("comparison"), QString::fromLatin1(comparatorCase.comparison)},
                                {QStringLiteral("compareValue"), QString::fromLatin1(comparatorCase.compareValue)},
                                {QStringLiteral("maxIterations"), 1000}})
                    .value(QStringLiteral("status"))
                    .toString()));

            moduleStarted.clear();
            const QJsonObject completed = runToTerminal(actor);
            QCOMPARE(completed.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
            QCOMPARE(actor.executeTool(QStringLiteral("validate_flow"), {}).value(QStringLiteral("status")).toString(),
                     QStringLiteral("valid"));
            const QString runId = completed.value(QStringLiteral("runId")).toString();
            const QJsonObject sourceOutput = output(runId, sourceId, QStringLiteral("whileGate"));
            QCOMPARE(sourceOutput.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(sourceOutput.value(QStringLiteral("value")).toString(), inputValue);
            if (inputValue.isEmpty()) {
                const QJsonObject stringLength = output(runId, sourceId, QStringLiteral("string_length"));
                QCOMPARE(stringLength.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
                QCOMPARE(stringLength.value(QStringLiteral("value")).toInt(), 0);
            }

            const QJsonObject whileResult = output(runId, whileId, QStringLiteral("while_result"));
            QCOMPARE(whileResult.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(whileResult.value(QStringLiteral("value")).toBool(), true);
            const QJsonObject stopRequested = output(runId, stopId, QStringLiteral("stop_while_requested"));
            QCOMPARE(stopRequested.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(stopRequested.value(QStringLiteral("value")).toBool(), true);
            QCOMPARE(startedCount(bodyId), 1);
            QCOMPARE(startedCount(stopId), 1);
            QCOMPARE(startedCount(afterId), 1);
        }
    }
};

QTEST_MAIN(TestAgentVisualPluginsG8)
#include "test_agent_visual_plugins_g8.moc"
