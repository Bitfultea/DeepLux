#include "core/engine/RunEngine.h"
#include "core/manager/PluginManager.h"
#include "core/manager/ProjectManager.h"
#include "core/model/ImageData.h"
#include "core/model/Project.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>

using namespace DeepLux;

class TestFitEllipseFlow : public QObject {
    Q_OBJECT

private:
    bool installPlugin(const QString& pluginRoot, const QString& name, const QString& metaSrc,
                       const QString& libSrc) const {
        QDir root(pluginRoot);
        if (!root.mkpath(name))
            return false;
        QDir dir(root.filePath(name));
        if (!QFileInfo::exists(metaSrc) || !QFileInfo::exists(libSrc))
            return false;
        const QString destLib = dir.filePath(QFileInfo(libSrc).fileName());
        QFile::remove(dir.filePath("metadata.json"));
        QFile::remove(destLib);
        return QFile::copy(metaSrc, dir.filePath("metadata.json")) && QFile::copy(libSrc, destLib);
    }

private slots:
    void cleanup() {
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
        PluginManager::instance().shutdown();
        qunsetenv("DEEPLUX_APP_DATA_DIR");
    }

    void testMeasurementInputToFitEllipse() {
        RunEngine& engine = RunEngine::instance();
        engine.clearModules();
        ProjectManager::instance().closeProject();

        QTemporaryDir appDir;
        QVERIFY(appDir.isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", appDir.path().toLocal8Bit());
        const QString pluginRoot = QDir(appDir.path()).filePath("plugins");
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("MeasurementInput"),
                              QStringLiteral(TEST_FITELLIPSE_META_MeasurementInput),
                              QStringLiteral(TEST_FITELLIPSE_LIB_MeasurementInput)));
        QVERIFY(installPlugin(pluginRoot, QStringLiteral("FitEllipse"), QStringLiteral(TEST_FITELLIPSE_META_FitEllipse),
                              QStringLiteral(TEST_FITELLIPSE_LIB_FitEllipse)));
        PluginManager::instance().addPluginPath(pluginRoot);
        QVERIFY(PluginManager::instance().initialize());
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("MeasurementInput")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("FitEllipse")));

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);

        QJsonArray points;
        for (int i = 0; i < 24; ++i) {
            const double t = i * M_PI / 12.0;
            points.append(QJsonArray{100 + 50 * std::cos(t), 100 + 25 * std::sin(t)});
        }
        ModuleInstance input;
        input.id = QStringLiteral("mi");
        input.moduleId = QStringLiteral("MeasurementInput");
        input.params["mode"] = QStringLiteral("point_set");
        input.params["points"] = points;
        project->addModule(input);

        ModuleInstance ellipse;
        ellipse.id = QStringLiteral("ell");
        ellipse.moduleId = QStringLiteral("FitEllipse");
        project->addModule(ellipse);

        ModuleConnection connection;
        connection.fromModuleId = QStringLiteral("mi");
        connection.toModuleId = QStringLiteral("ell");
        connection.fromPort = QStringLiteral("fit_points");
        connection.toPort = QStringLiteral("fit_points");
        connection.edgeType = QStringLiteral("data");
        project->addConnection(connection);

        QVERIFY(engine.loadProject(project));
        engine.runOnce();
        const ImageData output = engine.moduleOutput(QStringLiteral("ell"));
        QVERIFY2(output.hasData("ellipse_major_r"), "flow must produce ellipse_major_r");
        QVERIFY2(std::abs(output.data("ellipse_major_r").toDouble() - 50) < 3.0, "major axis ~50");
        QVERIFY2(std::abs(output.data("ellipse_minor_r").toDouble() - 25) < 3.0, "minor axis ~25");
    }
};

QTEST_MAIN(TestFitEllipseFlow)
#include "test_fitellipse_flow.moc"
