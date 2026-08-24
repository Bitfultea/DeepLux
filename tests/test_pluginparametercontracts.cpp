#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>
#include <QTemporaryDir>
#include <QVariant>
#include <QWidget>
#include <QtTest/QtTest>
#include <core/base/ModuleBase.h>
#include <core/interface/IModule.h>
#include <core/manager/PluginManager.h>

using namespace DeepLux;

/// 参数契约测试 — 阶段 0 基线
///
/// 目标：以当前代码为基线，检出以下问题（预期失败）：
///   1. 默认参数无法通过 validateParams()
///   2. clone() 丢失参数或实例不独立
///   3. ui.parameters 中的键不存在于 defaultParams()
///   4. 非内部参数缺少 UI Schema
///   5. 默认枚举值不在 options 中
///   6. 默认数值超出 min/max 或 decimals 不足以表达 step
///
/// 后续阶段修复后，此测试应全部通过。

class TestPluginParameterContracts : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testDefaultParamsValidate();
    void testClonePreservesParams();
    void testSchemaKeysExistInDefaultParams();
    void testNonInternalParamsHaveSchema();
    void testEnumDefaultsValid();
    void testNumericDefaultsInRange();

    // 阶段 1.3：端口与 ABI 契约
    void testAllPluginsAbiV2();
    void testPortsWellFormed();
    void testMissingRequiredInputReturnsStructuredError();
    void testWrongPortTypeReturnsStructuredError();
    void testDeclaredOutputsAreExported();

    // 阶段 G：execution 标记注入
    void testBlockingInjectedIntoCreatedModule();

private:
    struct PluginEntry {
        QString name;         // metadata "name" field
        QString dirName;      // source directory name
        QString metadataPath; // absolute path to metadata.json
        QString soPath;       // absolute path to .so
    };

    QList<PluginEntry> m_entries;
    QTemporaryDir m_tempDir;

    bool installAllPlugins();
    QVariant replacementValue(const QString& key, const QJsonValue& value) const;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void TestPluginParameterContracts::initTestCase() {
    QVERIFY2(installAllPlugins(), "Failed to install plugins for testing");

    // Redirect default app data dir to temp to avoid loading stale ~/.deeplux/plugins
    qputenv("DEEPLUX_APP_DATA_DIR", m_tempDir.filePath("appdata").toLocal8Bit());

    PluginManager::instance().shutdown();
    PluginManager::instance().addPluginPath(m_tempDir.filePath("plugins"));
    QVERIFY2(PluginManager::instance().initialize(), "PluginManager initialize failed");

    // Load all plugins
    for (const auto& entry : m_entries) {
        bool ok = PluginManager::instance().loadPlugin(entry.name);
        QVERIFY2(ok, qPrintable(QString("Failed to load plugin: %1").arg(entry.name)));
    }
}

void TestPluginParameterContracts::cleanupTestCase() {
    PluginManager::instance().shutdown();
}

bool TestPluginParameterContracts::installAllPlugins() {
    const QString srcRoot = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../../src/plugins");
    const QString libDir = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../lib");
    const QString pluginRoot = m_tempDir.filePath("plugins");

    // Scan for all metadata.json under src/plugins
    QDirIterator it(srcRoot, QStringList() << "metadata.json", QDir::Files, QDirIterator::Subdirectories);
    QList<QFileInfo> metas;
    while (it.hasNext()) {
        it.next();
        metas << it.fileInfo();
    }

    if (metas.isEmpty()) {
        qWarning() << "No metadata.json files found under" << srcRoot;
        return false;
    }

    for (const QFileInfo& meta : metas) {
        QFile f(meta.filePath());
        if (!f.open(QIODevice::ReadOnly))
            continue;
        QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        f.close();
        if (!doc.isObject())
            continue;
        QJsonObject json = doc.object();
        QString name = json.value("name").toString();
        if (name.isEmpty())
            continue;

        // Try to find matching .so: lib<name>Plugin.so or lib<dirName>Plugin.so
        const QString dirName = meta.dir().dirName();
        const QString shortDir = dirName.endsWith("Plugin") ? dirName.left(dirName.length() - 6) : dirName;

        QString soName;
        // Try name match first, then dirName match, then shortDir match
        for (const QString& candidate : {name, dirName, shortDir}) {
            QString p = QDir(libDir).filePath(QString("lib%1Plugin.so").arg(candidate));
            if (QFileInfo::exists(p)) {
                soName = p;
                break;
            }
        }
        if (soName.isEmpty()) {
            // Skip camera plugins and other platform-specific plugins that may not build
            continue;
        }

        // Install: copy metadata.json and .so to temp/plugins/<dirName>/
        const QString targetDir = QDir(pluginRoot).filePath(dirName);
        QDir::root().mkpath(targetDir);
        QFile::copy(meta.filePath(), QDir(targetDir).filePath("metadata.json"));
        QFile::copy(soName, QDir(targetDir).filePath(QFileInfo(soName).fileName()));

        m_entries.append({name, dirName, meta.filePath(), soName});
    }

    return !m_entries.isEmpty();
}

QVariant TestPluginParameterContracts::replacementValue(const QString& key, const QJsonValue& value) const {
    // Pick a replacement value different from default to test clone independence
    if (key == "fitMethod")
        return QString("LS");
    if (key == "operation")
        return QString("Subtract");
    if (key == "stringSource")
        return QString("Input");
    if (key == "maxAngle")
        return 120.0;
    if (key == "matchThreshold")
        return 0.65;
    if (key == "grabSource")
        return QString("Camera");
    if (key == "grabTimeout")
        return 3000;

    if (value.isString())
        return value.toString() + "_x";
    if (value.isBool())
        return !value.toBool();
    if (value.isDouble())
        return value.toDouble() + 17.0;
    if (value.isObject())
        return value.toObject(); // keep as-is for nested
    return QVariant("test");
}

// ---------------------------------------------------------------------------
// Test 1: Default params pass validateParams()
// ---------------------------------------------------------------------------

void TestPluginParameterContracts::testDefaultParamsValidate() {
    QStringList failures;
    for (const auto& entry : m_entries) {
        IModule* module = PluginManager::instance().createModule(entry.name);
        if (!module) {
            // Some plugins may fail to create (e.g. camera plugins without hardware)
            continue;
        }

        const QJsonObject params = module->currentParams();
        QString error;
        if (!module->validateParams(params, error)) {
            failures << QString("%1: validateParams failed for defaults: %2").arg(entry.name, error);
        }
        delete module;
    }

    if (!failures.isEmpty()) {
        QFAIL(qPrintable(QString("Default params validation failures:\n  - %1").arg(failures.join("\n  - "))));
    }
}

// ---------------------------------------------------------------------------
// Test 2: clone() preserves all params and is independent
// ---------------------------------------------------------------------------

void TestPluginParameterContracts::testClonePreservesParams() {
    QStringList failures;
    for (const auto& entry : m_entries) {
        IModule* original = PluginManager::instance().createModule(entry.name);
        if (!original)
            continue;

        const QJsonObject defaults = original->currentParams();
        if (defaults.isEmpty()) {
            delete original;
            continue;
        }

        // Clone without modifying — clone should have same params
        IModule* clone = original->clone();
        if (!clone) {
            // Some plugins may not support cloning yet
            delete original;
            continue;
        }
        if (clone == original) {
            failures << QString("%1: clone returned same instance").arg(entry.name);
            delete original;
            continue;
        }

        // Check all params preserved
        const QJsonObject cloneParams = clone->currentParams();
        for (auto it = defaults.begin(); it != defaults.end(); ++it) {
            if (cloneParams.value(it.key()) != it.value()) {
                failures << QString("%1: clone lost param '%2' (orig=%3, clone=%4)")
                                .arg(entry.name, it.key(), it.value().toVariant().toString(),
                                     cloneParams.value(it.key()).toVariant().toString());
            }
        }

        // Modify original, verify clone is unaffected
        const QString firstKey = defaults.keys().first();
        const QVariant repl = replacementValue(firstKey, defaults.value(firstKey));
        original->setParam(firstKey, repl);
        const QJsonObject cloneParamsAfter = clone->currentParams();
        if (cloneParamsAfter.value(firstKey).toVariant() == repl) {
            failures << QString("%1: clone is not independent (change propagated)").arg(entry.name);
        }

        delete clone;
        delete original;
    }

    if (!failures.isEmpty()) {
        QFAIL(qPrintable(QString("Clone parameter preservation failures:\n  - %1").arg(failures.join("\n  - "))));
    }
}

// ---------------------------------------------------------------------------
// Test 3: ui.parameters keys must exist in defaultParams()
// ---------------------------------------------------------------------------

void TestPluginParameterContracts::testSchemaKeysExistInDefaultParams() {
    QStringList failures;
    for (const auto& entry : m_entries) {
        PluginInfo info = PluginManager::instance().pluginInfo(entry.name);
        QJsonObject ui = info.ui;
        QJsonObject paramsSchema = ui.value("parameters").toObject();
        if (paramsSchema.isEmpty())
            continue; // No schema = uses createConfigWidget()

        IModule* module = PluginManager::instance().createModule(entry.name);
        if (!module)
            continue;
        const QJsonObject defaults = module->defaultParams();

        for (auto it = paramsSchema.begin(); it != paramsSchema.end(); ++it) {
            const QString& schemaKey = it.key();
            if (!defaults.contains(schemaKey)) {
                failures << QString("%1: schema key '%2' not in defaultParams()").arg(entry.name, schemaKey);
            }
        }
        delete module;
    }

    if (!failures.isEmpty()) {
        QFAIL(qPrintable(QString("Schema key mismatches:\n  - %1").arg(failures.join("\n  - "))));
    }
}

// ---------------------------------------------------------------------------
// Test 4: All non-internal params must have UI Schema entries
//         (internal params start with '_')
// ---------------------------------------------------------------------------

void TestPluginParameterContracts::testNonInternalParamsHaveSchema() {
    QStringList failures;
    for (const auto& entry : m_entries) {
        PluginInfo info = PluginManager::instance().pluginInfo(entry.name);
        QJsonObject ui = info.ui;
        QJsonObject paramsSchema = ui.value("parameters").toObject();
        if (paramsSchema.isEmpty()) {
            // Check if the plugin actually has any non-internal params
            IModule* mod = PluginManager::instance().createModule(entry.name);
            if (mod) {
                QJsonObject defaults = mod->defaultParams();
                bool hasVisibleParams = false;
                for (auto it = defaults.begin(); it != defaults.end(); ++it) {
                    if (!it.key().startsWith('_') && !it.key().endsWith("_options")) {
                        hasVisibleParams = true;
                        break;
                    }
                }
                delete mod;
                if (!hasVisibleParams) {
                    continue; // No params = no schema needed
                }
            }
            failures << QString("%1: no ui.parameters schema (uses createConfigWidget exclusively)").arg(entry.name);
            continue;
        }

        IModule* module = PluginManager::instance().createModule(entry.name);
        if (!module)
            continue;
        const QJsonObject defaults = module->defaultParams();

        for (auto it = defaults.begin(); it != defaults.end(); ++it) {
            const QString& key = it.key();
            if (key.startsWith('_'))
                continue; // internal param

            if (!paramsSchema.contains(key)) {
                failures << QString("%1: param '%2' missing from ui.parameters schema").arg(entry.name, key);
            }
        }
        delete module;
    }

    if (!failures.isEmpty()) {
        QFAIL(qPrintable(QString("Missing schema entries:\n  - %1").arg(failures.join("\n  - "))));
    }
}

// ---------------------------------------------------------------------------
// Test 5: Default enum values must exist in _options
// ---------------------------------------------------------------------------

void TestPluginParameterContracts::testEnumDefaultsValid() {
    QStringList failures;
    for (const auto& entry : m_entries) {
        PluginInfo info = PluginManager::instance().pluginInfo(entry.name);
        QJsonObject paramsSchema = info.ui.value("parameters").toObject();
        if (paramsSchema.isEmpty())
            continue;

        IModule* module = PluginManager::instance().createModule(entry.name);
        if (!module)
            continue;
        const QJsonObject defaults = module->defaultParams();

        for (auto it = paramsSchema.begin(); it != paramsSchema.end(); ++it) {
            const QString& key = it.key();
            QJsonObject schema = it.value().toObject();
            QJsonArray options = schema.value("_options").toArray();
            if (options.isEmpty())
                continue; // Not an enum

            QJsonValue defaultVal = defaults.value(key);
            bool found = false;
            for (const QJsonValue& opt : options) {
                if (opt.isObject()) {
                    if (opt.toObject().value("value") == defaultVal) {
                        found = true;
                        break;
                    }
                } else if (opt == defaultVal) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                failures << QString("%1: default value '%2' for '%3' not in _options")
                                .arg(entry.name, defaultVal.toVariant().toString(), key);
            }
        }
        delete module;
    }

    if (!failures.isEmpty()) {
        QFAIL(qPrintable(QString("Invalid enum defaults:\n  - %1").arg(failures.join("\n  - "))));
    }
}

// ---------------------------------------------------------------------------
// Test 6: Default numeric values within min/max, decimals sufficient for step
// ---------------------------------------------------------------------------

void TestPluginParameterContracts::testNumericDefaultsInRange() {
    QStringList failures;
    for (const auto& entry : m_entries) {
        PluginInfo info = PluginManager::instance().pluginInfo(entry.name);
        QJsonObject paramsSchema = info.ui.value("parameters").toObject();
        if (paramsSchema.isEmpty())
            continue;

        IModule* module = PluginManager::instance().createModule(entry.name);
        if (!module)
            continue;
        const QJsonObject defaults = module->defaultParams();

        for (auto it = paramsSchema.begin(); it != paramsSchema.end(); ++it) {
            const QString& key = it.key();
            QJsonObject schema = it.value().toObject();

            QJsonValue defaultVal = defaults.value(key);
            if (!defaultVal.isDouble())
                continue; // Only check numeric params

            double dv = defaultVal.toDouble();

            // Check min
            if (schema.contains("min")) {
                double mn = schema.value("min").toDouble();
                if (dv < mn) {
                    failures << QString("%1: default %2=%3 below min %4").arg(entry.name, key).arg(dv).arg(mn);
                }
            }

            // Check max
            if (schema.contains("max")) {
                double mx = schema.value("max").toDouble();
                if (dv > mx) {
                    failures << QString("%1: default %2=%3 above max %4").arg(entry.name, key).arg(dv).arg(mx);
                }
            }

            // Check decimals can express step
            if (schema.contains("step") && schema.contains("decimals")) {
                double step = schema.value("step").toDouble();
                int decimals = schema.value("decimals").toInt();
                // step should be representable with the given decimals
                double pow10 = std::pow(10.0, decimals);
                double stepScaled = step * pow10;
                if (std::abs(stepScaled - std::round(stepScaled)) > 1e-9) {
                    failures << QString("%1: step %2 not expressible with decimals %3 for '%4'")
                                    .arg(entry.name)
                                    .arg(step)
                                    .arg(decimals)
                                    .arg(key);
                }
            }
        }
        delete module;
    }

    if (!failures.isEmpty()) {
        QFAIL(qPrintable(QString("Numeric range failures:\n  - %1").arg(failures.join("\n  - "))));
    }
}

// ---------------------------------------------------------------------------
// 阶段 1.3：端口与 ABI 契约
// ---------------------------------------------------------------------------

void TestPluginParameterContracts::testAllPluginsAbiV2() {
    QStringList failures;
    for (const auto& entry : m_entries) {
        IModule* module = PluginManager::instance().createModule(entry.name);
        if (!module) {
            failures << QString("%1: createModule returned null").arg(entry.name);
            continue;
        }
        if (module->interfaceVersion() != DEEPLUX_MODULE_INTERFACE_VERSION) {
            failures << QString("%1: interfaceVersion=%2 expected=%3")
                            .arg(entry.name)
                            .arg(module->interfaceVersion())
                            .arg(DEEPLUX_MODULE_INTERFACE_VERSION);
        }
        delete module;
    }
    if (!failures.isEmpty()) {
        QFAIL(qPrintable(QString("ABI v2 failures:\n  - %1").arg(failures.join("\n  - "))));
    }
}

void TestPluginParameterContracts::testPortsWellFormed() {
    QStringList failures;
    for (const auto& entry : m_entries) {
        const PluginInfo info = PluginManager::instance().pluginInfo(entry.name);
        // 端口 ID 唯一、显示名非空（PluginManager 加载时已校验，这里复核）
        auto checkList = [&](const QList<PortSpec>& ports, const char* kind) {
            QSet<QString> ids;
            for (const PortSpec& p : ports) {
                if (p.id.isEmpty())
                    failures << QString("%1[%2]: empty port id").arg(entry.name, kind);
                if (p.displayName.isEmpty())
                    failures << QString("%1[%2]: port %3 empty displayName").arg(entry.name, kind, p.id);
                if (ids.contains(p.id))
                    failures << QString("%1[%2]: duplicate port %3").arg(entry.name, kind, p.id);
                ids.insert(p.id);
            }
        };
        checkList(info.inputPorts, "in");
        checkList(info.outputPorts, "out");

        // 非源模块应声明 image 输出载体
        bool hasImageOut = false;
        for (const PortSpec& p : info.outputPorts)
            if (p.id == QLatin1String("image"))
                hasImageOut = true;
        if (!hasImageOut && entry.name != QLatin1String("MeasurementInput")) {
            failures << QString("%1: no 'image' output port declared").arg(entry.name);
        }
    }
    if (!failures.isEmpty()) {
        QFAIL(qPrintable(QString("Port well-formedness failures:\n  - %1").arg(failures.join("\n  - "))));
    }
}

void TestPluginParameterContracts::testMissingRequiredInputReturnsStructuredError() {
    // FitLine 声明必需输入 fit_points；空输入应返回结构化错误而非崩溃
    IModule* module = PluginManager::instance().createModule(QStringLiteral("FitLine"));
    QVERIFY(module != nullptr);

    PortValueMap inputs; // 空：缺少必需 fit_points
    PortValueMap outputs;
    ExecutionContext ctx;
    const ExecutionResult result = module->execute(inputs, outputs, ctx);

    QVERIFY2(!result.success, "expected failure for missing required input");
    QCOMPARE(result.errorCode, ExecError::MissingRequiredInput);
    QVERIFY(!result.userMessage.isEmpty());
    delete module;
}

void TestPluginParameterContracts::testWrongPortTypeReturnsStructuredError() {
    IModule* module = PluginManager::instance().createModule(QStringLiteral("FitLine"));
    QVERIFY(module != nullptr);

    PortValueMap inputs;
    inputs.insert(QStringLiteral("fit_points"), QStringLiteral("not a point set"));
    PortValueMap outputs;
    ExecutionContext ctx;
    const ExecutionResult result = module->execute(inputs, outputs, ctx);

    QVERIFY(!result.success);
    QCOMPARE(result.errorCode, ExecError::TypeMismatch);
    QVERIFY(result.diagnostics.contains(QStringLiteral("fit_points")));
    delete module;
}

void TestPluginParameterContracts::testDeclaredOutputsAreExported() {
    IModule* module = PluginManager::instance().createModule(QStringLiteral("DistancePP"));
    QVERIFY(module != nullptr);
    QVERIFY(module->initialize());

    PortValueMap inputs;
    inputs.insert(QStringLiteral("point1"), QVariantList{200.0, 200.0});
    inputs.insert(QStringLiteral("point2"), QVariantList{440.0, 320.0});
    PortValueMap outputs;
    ExecutionContext ctx;
    const ExecutionResult result = module->execute(inputs, outputs, ctx);

    QVERIFY2(result.success, qPrintable(result.userMessage));
    QVERIFY(outputs.contains(QStringLiteral("image")));
    QVERIFY(outputs.contains(QStringLiteral("distance")));
    QVERIFY(qAbs(outputs.value(QStringLiteral("distance")).toDouble() - 268.3281573) < 0.001);
    delete module;
}

void TestPluginParameterContracts::testBlockingInjectedIntoCreatedModule() {
    // G3-fix2: createModule 必须把 metadata execution.blocking 注入模块实例。
    // 强制断言：找一个声明 blocking=true 的插件，验证 isBlocking() 为 true；
    // 找一个 blocking=false 的，验证 isBlocking() 为 false。
    // 若删除 PluginManager::createModule 的 setBlocking 注入，本测试失败。
    QString blockingPlugin, nonBlockingPlugin;
    for (const PluginEntry& entry : m_entries) {
        const PluginInfo info = PluginManager::instance().pluginInfo(entry.name);
        if (info.blocking && blockingPlugin.isEmpty())
            blockingPlugin = entry.name;
        if (!info.blocking && !info.inputPorts.isEmpty() && nonBlockingPlugin.isEmpty())
            nonBlockingPlugin = entry.name;
    }

    QVERIFY2(!blockingPlugin.isEmpty(), "at least one plugin must declare execution.blocking=true (e.g. SaveData)");
    IModule* bmod = PluginManager::instance().createModule(blockingPlugin);
    QVERIFY2(bmod != nullptr, qPrintable(QString("createModule(%1) must succeed").arg(blockingPlugin)));
    auto* bbase = qobject_cast<ModuleBase*>(bmod);
    QVERIFY(bbase != nullptr);
    QVERIFY2(bbase->isBlocking(),
             qPrintable(QString("createModule(%1) must inject blocking=true").arg(blockingPlugin)));
    delete bmod;

    // G4-fix1: blocking=false 反向验证也必须是强制断言，防止全部插件误标 blocking 时静默跳过
    QVERIFY2(!nonBlockingPlugin.isEmpty(),
             "at least one plugin must declare execution.blocking=false for reverse verification");
    IModule* nmod = PluginManager::instance().createModule(nonBlockingPlugin);
    QVERIFY2(nmod != nullptr, qPrintable(QString("createModule(%1) must succeed").arg(nonBlockingPlugin)));
    auto* nbase = qobject_cast<ModuleBase*>(nmod);
    QVERIFY(nbase != nullptr);
    QVERIFY2(!nbase->isBlocking(),
             qPrintable(QString("createModule(%1) must inject blocking=false").arg(nonBlockingPlugin)));
    delete nmod;
}

QTEST_MAIN(TestPluginParameterContracts)
#include "test_pluginparametercontracts.moc"
