#include <QtTest/QtTest>
#include <core/base/ModuleBase.h>

using namespace DeepLux;

// 测试用的具体模块实现
class TestModule : public ModuleBase
{
    Q_OBJECT

public:
    explicit TestModule(QObject* parent = nullptr)
        : ModuleBase(parent)
    {
        m_moduleId = "com.deeplux.test.module";
        m_name = "Test Module";
        m_category = "test";
        m_author = "Test Author";
        m_description = "A test module";
    }
    
    bool m_processCalled = false;
    bool m_processResult = true;
    QString m_lastError;

protected:
    bool process(const ImageData& input, ImageData& output) override
    {
        Q_UNUSED(input)
        Q_UNUSED(output)
        m_processCalled = true;
        
        if (!m_processResult) {
            emit errorOccurred(m_lastError);
        }
        
        return m_processResult;
    }
    
    QWidget* createConfigWidget() override
    {
        return nullptr;
    }
};

class DefaultParamModule : public ModuleBase
{
    Q_OBJECT

public:
    DefaultParamModule()
    {
        m_moduleId = "com.deeplux.test.default-param-module";
        m_name = "Default Param Module";
        m_category = "test";
        m_defaultParams = QJsonObject{
            {"threshold", 3.0},
            {"iterations", 100},
            {"method", "RANSAC"},
        };
        m_params = m_defaultParams;
    }

protected:
    bool process(const ImageData& input, ImageData& output) override
    {
        Q_UNUSED(input)
        Q_UNUSED(output)
        return true;
    }

    QWidget* createConfigWidget() override
    {
        return nullptr;
    }
};

class TestModuleBase : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    
    void testModuleInfo();
    void testInitialize();
    void testShutdown();
    void testExecute();
    void testExecuteWithoutInit();
    void testExecuteTwice();
    void testParams();
    void testSetParamsMergesDefaults();
    void testFromJsonMergesDefaults();
    void testSerialization();

private:
    TestModule* m_module;
};

void TestModuleBase::initTestCase()
{
    qDebug() << "=== TestModuleBase Start ===";
    m_module = new TestModule();
}

void TestModuleBase::cleanupTestCase()
{
    delete m_module;
    qDebug() << "=== TestModuleBase End ===";
}

void TestModuleBase::testModuleInfo()
{
    QCOMPARE(m_module->moduleId(), QString("com.deeplux.test.module"));
    QCOMPARE(m_module->name(), QString("Test Module"));
    QCOMPARE(m_module->category(), QString("test"));
    QCOMPARE(m_module->version(), QString("1.0.0"));
    QCOMPARE(m_module->author(), QString("Test Author"));
    QCOMPARE(m_module->description(), QString("A test module"));
}

void TestModuleBase::testInitialize()
{
    QVERIFY(!m_module->isInitialized());
    
    bool result = m_module->initialize();
    QVERIFY(result);
    QVERIFY(m_module->isInitialized());
    
    // 重复初始化应该返回 true
    result = m_module->initialize();
    QVERIFY(result);
}

void TestModuleBase::testShutdown()
{
    m_module->initialize();
    QVERIFY(m_module->isInitialized());
    
    m_module->shutdown();
    QVERIFY(!m_module->isInitialized());
    
    // 重复关闭应该安全
    m_module->shutdown();
    QVERIFY(!m_module->isInitialized());
}

void TestModuleBase::testExecute()
{
    m_module->initialize();
    m_module->m_processCalled = false;
    
    ImageData input, output;
    bool result = m_module->execute(input, output);
    
    QVERIFY(result);
    QVERIFY(m_module->m_processCalled);
}

void TestModuleBase::testExecuteWithoutInit()
{
    m_module->shutdown();
    
    ImageData input, output;
    bool result = m_module->execute(input, output);
    
    QVERIFY(!result);
}

void TestModuleBase::testExecuteTwice()
{
    m_module->initialize();
    
    // 这个测试需要异步执行来测试并发问题
    // 简化版本：确保单线程下正常工作
    ImageData input, output;
    m_module->m_processCalled = false;
    
    bool result = m_module->execute(input, output);
    QVERIFY(result);
    QVERIFY(m_module->m_processCalled);
}

void TestModuleBase::testParams()
{
    QJsonObject defaultParams = m_module->defaultParams();
    QVERIFY(defaultParams.isEmpty()); // 默认为空
    
    QJsonObject params;
    params["testKey"] = "testValue";
    params["number"] = 123;
    
    m_module->setParams(params);
    
    QJsonObject currentParams = m_module->currentParams();
    QCOMPARE(currentParams["testKey"].toString(), QString("testValue"));
    QCOMPARE(currentParams["number"].toInt(), 123);
}

void TestModuleBase::testSetParamsMergesDefaults()
{
    DefaultParamModule module;

    QJsonObject partial;
    partial["threshold"] = 7.25;
    module.setParams(partial);

    const QJsonObject params = module.currentParams();
    QCOMPARE(params["threshold"].toDouble(), 7.25);
    QCOMPARE(params["iterations"].toInt(), 100);
    QCOMPARE(params["method"].toString(), QString("RANSAC"));
}

void TestModuleBase::testFromJsonMergesDefaults()
{
    DefaultParamModule module;
    QJsonObject json = module.toJson();
    json["params"] = QJsonObject{{"threshold", 8.5}};

    QVERIFY(module.fromJson(json));

    const QJsonObject params = module.currentParams();
    QCOMPARE(params["threshold"].toDouble(), 8.5);
    QCOMPARE(params["iterations"].toInt(), 100);
    QCOMPARE(params["method"].toString(), QString("RANSAC"));
}

void TestModuleBase::testSerialization()
{
    m_module->initialize();
    
    QJsonObject params;
    params["param1"] = "value1";
    m_module->setParams(params);
    
    // 序列化
    QJsonObject json = m_module->toJson();
    
    QCOMPARE(json["moduleId"].toString(), QString("com.deeplux.test.module"));
    QCOMPARE(json["name"].toString(), QString("Test Module"));
    QVERIFY(json.contains("params"));
    
    // 反序列化到新对象
    TestModule newModule;
    bool result = newModule.fromJson(json);
    
    QVERIFY(result);
    QCOMPARE(newModule.moduleId(), QString("com.deeplux.test.module"));
    QCOMPARE(newModule.name(), QString("Test Module"));
}

QTEST_MAIN(TestModuleBase)
#include "test_modulebase.moc"
