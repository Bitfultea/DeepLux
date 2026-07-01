#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <core/engine/RunEngine.h>
#include <core/manager/ProjectManager.h>
#include <core/model/Project.h>
#include <ui/viewmodels/MainViewModel.h>

using namespace DeepLux;

class TestMainViewModel : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testInstance();
    void testNewProject();
    void testProjectName();
    void testModified();
    void testSaveProjectWithoutProjectEmitsExplicitError();
    void testSaveNewProjectToPath();
    void testRunProjectWithoutProjectEmitsError();
    void testRunProjectDelegatesToRunEngine();
    void testStopProjectDelegatesToRunEngine();
    void testCurrentTime();

private:
    MainViewModel* m_viewModel;
};

void TestMainViewModel::initTestCase() {
    qDebug() << "=== TestMainViewModel Start ===";
    m_viewModel = &MainViewModel::instance();
}

void TestMainViewModel::cleanupTestCase() {
    qDebug() << "=== TestMainViewModel End ===";
}

void TestMainViewModel::testInstance() {
    // 单例测试
    MainViewModel& instance1 = MainViewModel::instance();
    MainViewModel& instance2 = MainViewModel::instance();

    QVERIFY(&instance1 == &instance2);
}

void TestMainViewModel::testNewProject() {
    m_viewModel->newProject();

    QCOMPARE(m_viewModel->projectName(), QString("未命名项目"));
    QCOMPARE(m_viewModel->isModified(), false);
}

void TestMainViewModel::testProjectName() {
    m_viewModel->newProject();

    QString name = m_viewModel->projectName();
    QVERIFY(!name.isEmpty());
}

void TestMainViewModel::testModified() {
    m_viewModel->newProject();
    QVERIFY(!m_viewModel->isModified());
}

void TestMainViewModel::testSaveProjectWithoutProjectEmitsExplicitError() {
    ProjectManager::instance().closeProject();
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QSignalSpy errorSpy(m_viewModel, &MainViewModel::errorOccurred);

    const QString path = dir.filePath("missing-project.dproj");
    QVERIFY(!m_viewModel->saveProject(path));

    QCOMPARE(errorSpy.count(), 1);
    QVERIFY(errorSpy.takeFirst().at(0).toString().contains("No project"));
    QVERIFY(!QFileInfo::exists(path));
}

void TestMainViewModel::testSaveNewProjectToPath() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("new-project.dproj");

    m_viewModel->newProject();
    QVERIFY(m_viewModel->saveProject(path));

    QVERIFY(QFileInfo::exists(path));
    QCOMPARE(ProjectManager::instance().currentProject()->filePath(), path);
    QVERIFY(!m_viewModel->isModified());
}

void TestMainViewModel::testRunProjectWithoutProjectEmitsError() {
    ProjectManager::instance().closeProject();
    QSignalSpy errorSpy(m_viewModel, &MainViewModel::errorOccurred);

    m_viewModel->runProject();

    QCOMPARE(errorSpy.count(), 1);
    QVERIFY(errorSpy.takeFirst().at(0).toString().contains("No project"));
}

void TestMainViewModel::testRunProjectDelegatesToRunEngine() {
    m_viewModel->newProject();
    RunEngine::instance().stop();

    QSignalSpy errorSpy(m_viewModel, &MainViewModel::errorOccurred);

    m_viewModel->runProject();

    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(RunEngine::instance().runMode(), RunMode::RunOnce);
}

void TestMainViewModel::testStopProjectDelegatesToRunEngine() {
    m_viewModel->stopProject();
    QVERIFY(RunEngine::instance().isStopped());
}

void TestMainViewModel::testCurrentTime() {
    QString time = m_viewModel->currentTime();

    qDebug() << "Current time:" << time;

    QVERIFY(!time.isEmpty());
    // 检查格式 yyyy-MM-dd hh:mm:ss
    QVERIFY(time.contains("-"));
    QVERIFY(time.contains(":"));
}

QTEST_MAIN(TestMainViewModel)
#include "test_mainviewmodel.moc"
