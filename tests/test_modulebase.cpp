#include <QtTest/QtTest>
#include <core/base/ModuleBase.h>
#include <core/common/CancellationToken.h>
#include <core/deeplux/DataContract.h>

using namespace DeepLux;

// 测试用的具体模块实现
class TestModule : public ModuleBase {
    Q_OBJECT

public:
    explicit TestModule(QObject* parent = nullptr) : ModuleBase(parent) {
        m_moduleId = "com.deeplux.test.module";
        m_name = "Test Module";
        m_category = "test";
        m_author = "Test Author";
        m_description = "A test module";
    }

    bool m_processCalled = false;
    bool m_processResult = true;
    QString m_lastError;
    // T2.8：process() 执行期间是否能看到借用中的 token——用于证明"装得上"，
    // 与"退得掉"配对断言，避免恒为空的实现骗过契约。
    bool m_sawTokenDuringProcess = false;

    // isCancellationRequested() 是 protected（只给插件自己在执行期用）；
    // 测试需要从外部观察，用一个公开转发，不改生产访问权限。
    bool cancellationRequestedForTest() const {
        return isCancellationRequested();
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        Q_UNUSED(input)
        Q_UNUSED(output)
        m_processCalled = true;
        m_sawTokenDuringProcess = (cancellationToken() != nullptr);
        if (!m_processResult) {
            emit errorOccurred(m_lastError);
        }

        return m_processResult;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class DefaultParamModule : public ModuleBase {
    Q_OBJECT

public:
    DefaultParamModule() {
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
    bool process(const ImageData& input, ImageData& output) override {
        Q_UNUSED(input)
        Q_UNUSED(output)
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class TestModuleBase : public QObject {
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
    // T2.8（2026-11-10 评审裁定）：token 借用的生命周期契约。
    void testCancellationTokenDetachedAfterExecute();
    void testCancellationTokenDtorLeavesNoDanglingBorrow();

private:
    TestModule* m_module;
};

void TestModuleBase::initTestCase() {
    qDebug() << "=== TestModuleBase Start ===";
    m_module = new TestModule();
}

void TestModuleBase::cleanupTestCase() {
    delete m_module;
    qDebug() << "=== TestModuleBase End ===";
}

void TestModuleBase::testModuleInfo() {
    QCOMPARE(m_module->moduleId(), QString("com.deeplux.test.module"));
    QCOMPARE(m_module->name(), QString("Test Module"));
    QCOMPARE(m_module->category(), QString("test"));
    QCOMPARE(m_module->version(), QString("1.0.0"));
    QCOMPARE(m_module->author(), QString("Test Author"));
    QCOMPARE(m_module->description(), QString("A test module"));
}

void TestModuleBase::testInitialize() {
    QVERIFY(!m_module->isInitialized());

    bool result = m_module->initialize();
    QVERIFY(result);
    QVERIFY(m_module->isInitialized());

    // 重复初始化应该返回 true
    result = m_module->initialize();
    QVERIFY(result);
}

void TestModuleBase::testShutdown() {
    m_module->initialize();
    QVERIFY(m_module->isInitialized());

    m_module->shutdown();
    QVERIFY(!m_module->isInitialized());

    // 重复关闭应该安全
    m_module->shutdown();
    QVERIFY(!m_module->isInitialized());
}

void TestModuleBase::testExecute() {
    m_module->initialize();
    m_module->m_processCalled = false;

    ImageData input, output;
    bool result = m_module->execute(input, output);

    QVERIFY(result);
    QVERIFY(m_module->m_processCalled);
}

void TestModuleBase::testExecuteWithoutInit() {
    m_module->shutdown();

    ImageData input, output;
    bool result = m_module->execute(input, output);

    QVERIFY(!result);
}

void TestModuleBase::testExecuteTwice() {
    m_module->initialize();

    // 这个测试需要异步执行来测试并发问题
    // 简化版本：确保单线程下正常工作
    ImageData input, output;
    m_module->m_processCalled = false;

    bool result = m_module->execute(input, output);
    QVERIFY(result);
    QVERIFY(m_module->m_processCalled);
}

void TestModuleBase::testParams() {
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

void TestModuleBase::testSetParamsMergesDefaults() {
    DefaultParamModule module;

    QJsonObject partial;
    partial["threshold"] = 7.25;
    module.setParams(partial);

    const QJsonObject params = module.currentParams();
    QCOMPARE(params["threshold"].toDouble(), 7.25);
    QCOMPARE(params["iterations"].toInt(), 100);
    QCOMPARE(params["method"].toString(), QString("RANSAC"));
}

void TestModuleBase::testFromJsonMergesDefaults() {
    DefaultParamModule module;
    QJsonObject json = module.toJson();
    json["params"] = QJsonObject{{"threshold", 8.5}};

    QVERIFY(module.fromJson(json));

    const QJsonObject params = module.currentParams();
    QCOMPARE(params["threshold"].toDouble(), 8.5);
    QCOMPARE(params["iterations"].toInt(), 100);
    QCOMPARE(params["method"].toString(), QString("RANSAC"));
}

void TestModuleBase::testSerialization() {
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

// ==== T2.8（2026-11-10 评审裁定）：CancellationToken 借用语义 ====
//
// 缺陷前提：ModuleBase::execute() 在入口把调用方传入的 token 写进全局借用表
// （g_cancellationTokens），但**只设不清**——本次执行结束不摘除，CancellationToken
// 自身也没有析构清理。于是表里长期留存调用方的裸指针：一旦 token 先于模块销毁
// （RunEngine 析构后模块仍存活，或调用方传入短生命周期 token），
// isCancellationRequested() 就会解引用野指针。
//
// 性质声明：本用例是**行为契约／预防性加固**的守门测试，不是 NF 红灯证据。
// 悬垂解引用无法在单元测试里安全地"复现到崩"，故用两条可观察断言替代：
//   ① 执行结束后借用表必须为空（作用域守卫）；
//   ② token 先于模块销毁后，借用表必须为空（析构兜底）。
// 两者共同保证表里永不会留下指向已销毁 token 的条目。

void TestModuleBase::testCancellationTokenDetachedAfterExecute() {
    TestModule module;
    QVERIFY(module.initialize());

    CancellationToken token;
    PortValueMap inputs;
    PortValueMap outputs;
    ExecutionContext ctx;
    ctx.cancellationToken = &token;

    const ExecutionResult result = module.execute(inputs, outputs, ctx);
    QVERIFY2(result.success, "the module must execute successfully");

    // 执行期内借用成立：这正是插件能读到 token 的前提，不能因为整改而失效。
    // 注意断言顺序——先证明"装得上"，再证明"退得掉"，否则恒为空的实现也能骗过用例。
    QVERIFY2(module.m_sawTokenDuringProcess, "the module must see the token while it is executing");

    // ① 执行结束后必须摘除：token 与"本次执行"同生命周期。
    QVERIFY2(module.cancellationToken() == nullptr,
             "the borrowed token must be detached once execute() returns; a leftover entry is a "
             "dangling pointer waiting for the token to die");
    QVERIFY2(!module.cancellationRequestedForTest(), "an unattached module must not report a cancellation request");
}

void TestModuleBase::testCancellationTokenDtorLeavesNoDanglingBorrow() {
    TestModule module;
    QVERIFY(module.initialize());

    PortValueMap inputs;
    PortValueMap outputs;

    // token 短于模块生命周期：手工建立借用后销毁 token，模拟"token 先于模块消失"
    // （例如 RunEngine 析构后模块仍被持有）。
    {
        CancellationToken token;
        module.setCancellationToken(&token);
        QVERIFY2(module.cancellationToken() == &token, "the borrow must be observable before the token dies");
    } // token 在此销毁

    // ② 析构兜底：token 消失后，模块的借用必须已被清空。
    QVERIFY2(module.cancellationToken() == nullptr,
             "a destroyed token must be detached from every borrowing module; otherwise the table "
             "keeps a dangling pointer");
    QVERIFY2(!module.cancellationRequestedForTest(),
             "querying cancellation after the token died must be safe and report false");
}

QTEST_MAIN(TestModuleBase)
#include "test_modulebase.moc"
