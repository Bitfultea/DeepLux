#include "TestPluginRegistry.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QPointF>
#include <QSet>
#include <QTemporaryDir>
#include <QVariant>
#include <QVector>
#include <QWidget>
#include <QtTest/QtTest>
#include <core/agent/AgentActor.h>
#include <core/agent/ToolSchema.h>
#include <core/base/ModuleBase.h>
#include <core/common/UserManager.h>
#include <core/deeplux/DataContract.h>
#include <core/display/DisplayData.h>
#include <core/interface/IModule.h>
#include <core/manager/PluginManager.h>
#include <core/manager/ProjectManager.h>
#include <core/model/ImageData.h>
#include <core/model/Project.h>
#include <limits>

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
    // 步4/P2-6：强类型载荷校验
    void testPayloadTypeTightening();
    void testMissingRequiredInputReturnsStructuredError();
    void testWrongPortTypeReturnsStructuredError();
    void testDeclaredOutputsAreExported();

    // 阶段 G：execution 标记注入
    void testBlockingInjectedIntoCreatedModule();

    // 阶段 3：数据与构建契约收口
    void testUnsupportedPortTypesClassified();
    void testMetadataTypeSupportGate();
    void testPortsFieldsMustBeJsonArrays();
    void testWrongPayloadRejected();
    void testDetectionListBoundary();
    void testG2MeasurementModesThroughAgent();
    void testG2ComplexConfigRoundTrip();
    void testGapMeasure3DRoiCentersDescribeInputPixelGrid();
    void testGapMeasure3DZOutputsDoNotClaimMillimeters();
    void testHeightMapRoiMetadataMatchesAgentSchema();
    void testMeasureCirclePixelParametersDescribeInputGrid();
    void testCropImageRoiMetadataSchemaAndGuiDescribePixels();

private:
    struct PluginEntry {
        QString name;         // metadata "name" field
        QString dirName;      // source directory name (install directory)
        QString metadataPath; // absolute path to metadata.json
        QString libraryPath;  // CMake-injected plugin library (dll/so/dylib)
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
    QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));

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
    QVERIFY(UserManager::instance().login(QStringLiteral("operator"), QString()));
}

bool TestPluginParameterContracts::installAllPlugins() {
    const QString pluginRoot = m_tempDir.filePath("plugins");

    for (const DeepLuxTest::PluginEntry& plugin : DeepLuxTest::pluginRegistry()) {
        QFile metaFile(plugin.metadataPath);
        if (!metaFile.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(metaFile.readAll());
        metaFile.close();
        if (!doc.isObject()) {
            continue;
        }
        const QJsonObject json = doc.object();
        const QString name = json.value("name").toString();
        if (name.isEmpty()) {
            continue;
        }
        // 相机插件实现 ICameraPlugin 而非 IModule，不参与参数契约校验。
        if (json.value("category").toString().compare(QLatin1String("camera"), Qt::CaseInsensitive) == 0) {
            continue;
        }

        const QString dirName = DeepLuxTest::pluginInstallDir(plugin);
        const QString targetDir = QDir(pluginRoot).filePath(dirName);
        if (!QDir().mkpath(targetDir)) {
            continue;
        }
        const QString libName = QFileInfo(plugin.libraryPath).fileName();
        QFile::remove(QDir(targetDir).filePath("metadata.json"));
        QFile::remove(QDir(targetDir).filePath(libName));
        if (!QFile::copy(plugin.metadataPath, QDir(targetDir).filePath("metadata.json")) ||
            !QFile::copy(plugin.libraryPath, QDir(targetDir).filePath(libName))) {
            continue;
        }

        m_entries.append({name, dirName, plugin.metadataPath, plugin.libraryPath});
    }

    if (m_entries.isEmpty()) {
        qWarning() << "No plugin libraries/metadata found via CMake-injected plugin manifest";
        return false;
    }
    return true;
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
        auto module = PluginManager::instance().createManagedModule(entry.name);
        if (!module) {
            // Some plugins may fail to create (e.g. camera plugins without hardware)
            continue;
        }

        const QJsonObject params = module->currentParams();
        QString error;
        if (!module->validateParams(params, error)) {
            failures << QString("%1: validateParams failed for defaults: %2").arg(entry.name, error);
        }
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
        auto original = PluginManager::instance().createManagedModule(entry.name);
        if (!original)
            continue;

        const QJsonObject defaults = original->currentParams();
        if (defaults.isEmpty()) {
            continue;
        }

        // Clone without modifying — clone should have same params
        IModule* clone = original->clone();
        if (!clone) {
            // Some plugins may not support cloning yet
            continue;
        }
        if (clone == original.get()) {
            failures << QString("%1: clone returned same instance").arg(entry.name);
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

        auto module = PluginManager::instance().createManagedModule(entry.name);
        if (!module)
            continue;
        const QJsonObject defaults = module->defaultParams();

        for (auto it = paramsSchema.begin(); it != paramsSchema.end(); ++it) {
            const QString& schemaKey = it.key();
            if (!defaults.contains(schemaKey)) {
                failures << QString("%1: schema key '%2' not in defaultParams()").arg(entry.name, schemaKey);
            }
        }
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
            auto mod = PluginManager::instance().createManagedModule(entry.name);
            if (mod) {
                QJsonObject defaults = mod->defaultParams();
                bool hasVisibleParams = false;
                for (auto it = defaults.begin(); it != defaults.end(); ++it) {
                    if (!it.key().startsWith('_') && !it.key().endsWith("_options")) {
                        hasVisibleParams = true;
                        break;
                    }
                }
                if (!hasVisibleParams) {
                    continue; // No params = no schema needed
                }
            }
            failures << QString("%1: no ui.parameters schema (uses createConfigWidget exclusively)").arg(entry.name);
            continue;
        }

        auto module = PluginManager::instance().createManagedModule(entry.name);
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

        auto module = PluginManager::instance().createManagedModule(entry.name);
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

        auto module = PluginManager::instance().createManagedModule(entry.name);
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
        auto module = PluginManager::instance().createManagedModule(entry.name);
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
    auto module = PluginManager::instance().createManagedModule(QStringLiteral("FitLine"));
    QVERIFY(module != nullptr);

    PortValueMap inputs; // 空：缺少必需 fit_points
    PortValueMap outputs;
    ExecutionContext ctx;
    const ExecutionResult result = module->execute(inputs, outputs, ctx);

    QVERIFY2(!result.success, "expected failure for missing required input");
    QCOMPARE(result.errorCode, ExecError::MissingRequiredInput);
    QVERIFY(!result.userMessage.isEmpty());
}

void TestPluginParameterContracts::testWrongPortTypeReturnsStructuredError() {
    auto module = PluginManager::instance().createManagedModule(QStringLiteral("FitLine"));
    QVERIFY(module != nullptr);

    PortValueMap inputs;
    inputs.insert(QStringLiteral("fit_points"), QStringLiteral("not a point set"));
    PortValueMap outputs;
    ExecutionContext ctx;
    const ExecutionResult result = module->execute(inputs, outputs, ctx);

    QVERIFY(!result.success);
    QCOMPARE(result.errorCode, ExecError::TypeMismatch);
    QVERIFY(result.diagnostics.contains(QStringLiteral("fit_points")));
}

void TestPluginParameterContracts::testDeclaredOutputsAreExported() {
    auto module = PluginManager::instance().createManagedModule(QStringLiteral("DistancePP"));
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
    auto bmod = PluginManager::instance().createManagedModule(blockingPlugin);
    QVERIFY2(bmod != nullptr, qPrintable(QString("createModule(%1) must succeed").arg(blockingPlugin)));
    auto* bbase = qobject_cast<ModuleBase*>(bmod.get());
    QVERIFY(bbase != nullptr);
    QVERIFY2(bbase->isBlocking(),
             qPrintable(QString("createModule(%1) must inject blocking=true").arg(blockingPlugin)));

    // G4-fix1: blocking=false 反向验证也必须是强制断言，防止全部插件误标 blocking 时静默跳过
    QVERIFY2(!nonBlockingPlugin.isEmpty(),
             "at least one plugin must declare execution.blocking=false for reverse verification");
    auto nmod = PluginManager::instance().createManagedModule(nonBlockingPlugin);
    QVERIFY2(nmod != nullptr, qPrintable(QString("createModule(%1) must succeed").arg(nonBlockingPlugin)));
    auto* nbase = qobject_cast<ModuleBase*>(nmod.get());
    QVERIFY(nbase != nullptr);
    QVERIFY2(!nbase->isBlocking(),
             qPrintable(QString("createModule(%1) must inject blocking=false").arg(nonBlockingPlugin)));
}

void TestPluginParameterContracts::testPayloadTypeTightening() {
    using DeepLux::DataType;
    using DeepLux::portValueMatchesType;

    // PointSet2D: 接受 QPointF 列表 / 2 数值列表
    QVERIFY(portValueMatchesType(QVariant::fromValue(QVector<QPointF>{QPointF(0, 0), QPointF(1, 1)}),
                                 DataType::PointSet2D));
    QVERIFY(portValueMatchesType(QVariant(QVariantList{QVariant(QVariantList{0.0, 0.0})}), DataType::PointSet2D));
    QVERIFY(portValueMatchesType(QVariant(QVariantList{}), DataType::PointSet2D));
    // PointSet2D: 拒绝任意标量列表
    QVERIFY(!portValueMatchesType(QVariant(QVariantList{1.0, 2.0, 3.0}), DataType::PointSet2D));

    // DetectionList: 接受强类型
    DeepLux::DetectionList dl;
    DeepLux::Detection d;
    d.x = 1;
    d.y = 2;
    d.width = 3;
    d.height = 4;
    d.score = 0.8;
    dl.items.append(d);
    QVERIFY(portValueMatchesType(QVariant::fromValue(dl), DataType::DetectionList));
    QVERIFY(portValueMatchesType(QVariant::fromValue(DeepLux::DetectionList{}), DataType::DetectionList));
    // DetectionList: 接受完整且有效的逐元素 map
    QVERIFY(portValueMatchesType(
        QVariant(QVariantList{QVariant(
            QVariantMap{{"x", 1.0}, {"y", 2.0}, {"width", 3.0}, {"height", 4.0}, {"score", 0.8}, {"label", "ok"}})}),
        DataType::DetectionList));
    QVERIFY(portValueMatchesType(QVariant(QVariantList{}), DataType::DetectionList));
    QVERIFY(!portValueMatchesType(QVariant(QVariantList{QVariant(QVariantMap{{"x", 1.0}, {"y", 2.0}})}),
                                  DataType::DetectionList));
    d.score = 1.1;
    dl.items[0] = d;
    QVERIFY(!portValueMatchesType(QVariant::fromValue(dl), DataType::DetectionList));
    // DetectionList: 拒绝任意标量列表
    QVERIFY(!portValueMatchesType(QVariant(QVariantList{1.0, 2.0}), DataType::DetectionList));

    // Circle2D: 接受强类型与 3 数值列表
    DeepLux::Circle2D c;
    c.radius = 5;
    QVERIFY(portValueMatchesType(QVariant::fromValue(c), DataType::Circle2D));
    QVERIFY(portValueMatchesType(QVariant(QVariantList{1.0, 2.0, 3.0}), DataType::Circle2D));

    // Region2D（SAM 批1）: 接受强类型（有效）与 polygon+bbox 完整 map，拒绝任意载荷
    DeepLux::Region2D r;
    r.polygon = {QPointF(0, 0), QPointF(4, 0), QPointF(0, 3)};
    QVERIFY(portValueMatchesType(QVariant::fromValue(r), DataType::Region2D));
    QVERIFY(!portValueMatchesType(QVariant::fromValue(DeepLux::Region2D{}), DataType::Region2D));
    QVERIFY(portValueMatchesType(QVariant(QVariantMap{{"polygon", QVariant(QVariantList{})},
                                                      {"bbox", QVariant(QVariantList{1.0, 2.0, 3.0, 4.0})}}),
                                 DataType::Region2D));
    QVERIFY(!portValueMatchesType(QVariant(1), DataType::Region2D));

    // 尚无专用载荷和生产者的复杂类型必须失败关闭，不能接受任意 QVariant。
    for (DataType type : {DataType::Mask2D, DataType::Transform2D, DataType::ClassScores}) {
        QVERIFY(!portValueMatchesType(QVariant(1), type));
    }
}

// ---------------------------------------------------------------------------
// 阶段 3：未实现端口类型不得声明为可运行端口（加载期拒绝）
// ---------------------------------------------------------------------------

void TestPluginParameterContracts::testUnsupportedPortTypesClassified() {
    using DeepLux::DataType;
    using DeepLux::isSupportedPortType;

    // 保留枚举 ABI，但以下未实现类型尚无载荷契约与生产者插件，不得作为可运行端口类型
    // （SAM 批1：Region2D 已有载荷契约与 SamPredict 生产者，升级为受支持）
    for (DataType type : {DataType::Mask2D, DataType::Transform2D, DataType::ClassScores}) {
        QVERIFY2(!isSupportedPortType(type), qPrintable(dataTypeName(type) + " must be unsupported"));
    }
    // 已实现的类型不受影响
    for (DataType type :
         {DataType::Image2D, DataType::HeightMap2D, DataType::PointCloud3D, DataType::Point2D, DataType::Point3D,
          DataType::PointSet2D, DataType::Line2D, DataType::Circle2D, DataType::Ellipse2D, DataType::Plane3D,
          DataType::DetectionList, DataType::Region2D, DataType::Number, DataType::Integer, DataType::Boolean,
          DataType::String, DataType::Binary, DataType::Table, DataType::Any}) {
        QVERIFY2(isSupportedPortType(type), qPrintable(dataTypeName(type) + " must be supported"));
    }
    // 失败关闭：非法枚举值不得被判定为支持（防止未来新增未实现类型自动放行）
    QVERIFY(!isSupportedPortType(static_cast<DataType>(-1)));
    QVERIFY(!isSupportedPortType(static_cast<DataType>(9999)));
}

// 写临时 metadata 并走 PluginManager::loadPluginMetadata 加载门禁
static void writeMetadataAndLoad(const QTemporaryDir& dir, const QByteArray& content, const QString& fileName,
                                 bool& loaded, QString& loadError) {
    const QString path = dir.filePath(fileName);
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(content);
    file.close();
    PluginInfo info;
    loaded = PluginManager::instance().loadPluginMetadata(path, info);
    loadError = info.error;
}

void TestPluginParameterContracts::testMetadataTypeSupportGate() {
    // metadata 声明未实现类型必须在加载阶段明确拒绝，
    // 不能放行后再让运行期"连接合法但永远拒绝"。
    const QByteArray unsupportedInput = R"({
        "id": "com.test.unsupportedin",
        "name": "UnsupportedTypeProbeIn",
        "category": "test",
        "version": "1.0.0",
        "ports": {
            "inputs": [{"id": "mask", "displayName": "掩膜", "type": "Mask2D"}],
            "outputs": [{"id": "image", "displayName": "输出", "type": "Image2D"}]
        }
    })";
    const QByteArray ellipseOutput = R"({
        "id": "com.test.ellipseprobe",
        "name": "Ellipse2DProbe",
        "category": "test",
        "version": "1.0.0",
        "ports": {
            "inputs": [{"id": "image", "displayName": "输入", "type": "Image2D"}],
            "outputs": [{"id": "ellipse", "displayName": "椭圆", "type": "Ellipse2D"}]
        }
    })";
    // SAM 批1：Region2D 已有载荷契约与生产者，加载门禁必须放行（与上方拒绝相对照）
    const QByteArray regionOutput = R"({
        "id": "com.test.regionprobe",
        "name": "Region2DProbe",
        "category": "test",
        "version": "1.0.0",
        "ports": {
            "inputs": [{"id": "image", "displayName": "输入", "type": "Image2D"}],
            "outputs": [{"id": "region", "displayName": "区域", "type": "Region2D"}]
        }
    })";

    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    bool loaded = true;
    QString loadError;
    writeMetadataAndLoad(dir, unsupportedInput, "unsupported_in.json", loaded, loadError);
    QVERIFY2(!loaded, "metadata declaring Mask2D input must be rejected at load");
    QVERIFY2(loadError.contains(QStringLiteral("Mask2D")),
             qPrintable("rejection must name the unsupported type, got: " + loadError));

    writeMetadataAndLoad(dir, ellipseOutput, "ellipse_probe.json", loaded, loadError);
    QVERIFY2(loaded, qPrintable("metadata declaring Ellipse2D output must load, got: " + loadError));

    writeMetadataAndLoad(dir, regionOutput, "region_probe.json", loaded, loadError);
    QVERIFY2(loaded, qPrintable("metadata declaring Region2D output must load, got: " + loadError));
}

void TestPluginParameterContracts::testPortsFieldsMustBeJsonArrays() {
    // ports.inputs/outputs 不是 JSON 数组时必须拒绝，
    // 不能被 toArray() 静默转成空数组后按"零端口"合法加载。
    const QByteArray inputsNotArray = R"({
        "id": "com.test.portsnotarray1",
        "name": "PortsNotArrayProbeIn",
        "category": "test",
        "version": "1.0.0",
        "ports": {
            "inputs": "image",
            "outputs": [{"id": "image", "displayName": "输出", "type": "Image2D"}]
        }
    })";
    const QByteArray outputsNotArray = R"({
        "id": "com.test.portsnotarray2",
        "name": "PortsNotArrayProbeOut",
        "category": "test",
        "version": "1.0.0",
        "ports": {
            "inputs": [{"id": "image", "displayName": "输入", "type": "Image2D"}],
            "outputs": {"id": "image", "displayName": "输出", "type": "Image2D"}
        }
    })";

    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    bool loaded = true;
    QString loadError;
    writeMetadataAndLoad(dir, inputsNotArray, "ports_inputs_not_array.json", loaded, loadError);
    QVERIFY2(!loaded, "metadata with non-array ports.inputs must be rejected at load");
    QVERIFY2(loadError.contains(QStringLiteral("inputs")),
             qPrintable("rejection must name ports.inputs, got: " + loadError));

    writeMetadataAndLoad(dir, outputsNotArray, "ports_outputs_not_array.json", loaded, loadError);
    QVERIFY2(!loaded, "metadata with non-array ports.outputs must be rejected at load");
    QVERIFY2(loadError.contains(QStringLiteral("outputs")),
             qPrintable("rejection must name ports.outputs, got: " + loadError));
}

// ---------------------------------------------------------------------------
// 阶段 3：错误载荷与 DetectionList 边界
// ---------------------------------------------------------------------------

void TestPluginParameterContracts::testWrongPayloadRejected() {
    using DeepLux::DataType;
    using DeepLux::portValueMatchesType;

    // Image2D/HeightMap2D 拒绝非 ImageData 载荷
    QVERIFY(!portValueMatchesType(QVariant(42), DataType::Image2D));
    QVERIFY(!portValueMatchesType(QVariant(QStringLiteral("image")), DataType::HeightMap2D));

    // PointCloud3D 拒绝不携带 point_cloud 键的 ImageData
    ImageData bareImage;
    QVERIFY(!portValueMatchesType(QVariant::fromValue(bareImage), DataType::PointCloud3D));
    ImageData withCloud;
    withCloud.setData(QStringLiteral("point_cloud"), QVariant::fromValue(PointCloudData()));
    QVERIFY(portValueMatchesType(QVariant::fromValue(withCloud), DataType::PointCloud3D));

    // PointCloud3D 还必须校验键值本身可转换为 PointCloudData，
    // 携带错误键值（如字符串）的 ImageData 不得被当作合法点云载荷
    ImageData badCloud;
    badCloud.setData(QStringLiteral("point_cloud"), QStringLiteral("invalid"));
    QVERIFY(!portValueMatchesType(QVariant::fromValue(badCloud), DataType::PointCloud3D));
    ImageData badCloudList;
    badCloudList.setData(QStringLiteral("point_cloud"), QVariant(QVariantList{1.0, 2.0}));
    QVERIFY(!portValueMatchesType(QVariant::fromValue(badCloudList), DataType::PointCloud3D));

    // 几何类型拒绝错误维度/错误元素
    QVERIFY(!portValueMatchesType(QVariant(QVariantList{1.0, 2.0, 3.0}), DataType::Line2D)); // 需 4 数值
    QVERIFY(!portValueMatchesType(QVariant(QVariantList{1.0, 2.0}), DataType::Plane3D));     // 需 9 数值
    QVERIFY(!portValueMatchesType(QVariant(QVariantList{QStringLiteral("a"), QStringLiteral("b")}), DataType::Point2D));

    // 标量/容器类型拒绝串型载荷
    QVERIFY(!portValueMatchesType(QVariant(QStringLiteral("1")), DataType::Number));
    QVERIFY(!portValueMatchesType(QVariant(1), DataType::Boolean));
    QVERIFY(!portValueMatchesType(QVariant(1.5), DataType::Integer));
    QVERIFY(!portValueMatchesType(QVariant(QStringLiteral("table")), DataType::Table));
    QVERIFY(!portValueMatchesType(QVariant(1), DataType::Binary));
}

void TestPluginParameterContracts::testDetectionListBoundary() {
    using DeepLux::DataType;
    using DeepLux::portValueMatchesType;

    auto mapDetection = [](double x, double y, double w, double h, double score, const QString& label = QString()) {
        QVariantMap m{{"x", x}, {"y", y}, {"width", w}, {"height", h}, {"score", score}};
        if (!label.isNull())
            m.insert("label", label);
        return QVariant(m);
    };

    // 空列表合法
    QVERIFY(portValueMatchesType(QVariant(QVariantList{}), DataType::DetectionList));
    QVERIFY(portValueMatchesType(QVariant::fromValue(DeepLux::DetectionList{}), DataType::DetectionList));

    // 边界值合法：宽高为 0、score 取 0/1
    QVERIFY(portValueMatchesType(QVariant(QVariantList{mapDetection(1, 2, 0, 0, 0)}), DataType::DetectionList));
    QVERIFY(portValueMatchesType(QVariant(QVariantList{mapDetection(1, 2, 3, 4, 1.0)}), DataType::DetectionList));

    // 非法数值拒绝：负宽高、score 越界、NaN
    QVERIFY(!portValueMatchesType(QVariant(QVariantList{mapDetection(1, 2, -3, 4, 0.5)}), DataType::DetectionList));
    QVERIFY(!portValueMatchesType(QVariant(QVariantList{mapDetection(1, 2, 3, -4, 0.5)}), DataType::DetectionList));
    QVERIFY(!portValueMatchesType(QVariant(QVariantList{mapDetection(1, 2, 3, 4, 1.5)}), DataType::DetectionList));
    QVERIFY(!portValueMatchesType(
        QVariant(QVariantList{mapDetection(1, 2, 3, 4, std::numeric_limits<double>::quiet_NaN())}),
        DataType::DetectionList));

    // 字段缺失拒绝；label 必须为字符串
    QVariantMap missingScore{{"x", 1.0}, {"y", 2.0}, {"width", 3.0}, {"height", 4.0}};
    QVERIFY(!portValueMatchesType(QVariant(QVariantList{QVariant(missingScore)}), DataType::DetectionList));
    QVariantMap badLabel{{"x", 1.0}, {"y", 2.0}, {"width", 3.0}, {"height", 4.0}, {"score", 0.5}, {"label", 7}};
    QVERIFY(!portValueMatchesType(QVariant(QVariantList{QVariant(badLabel)}), DataType::DetectionList));

    // 强类型载荷同样执行边界校验
    DeepLux::DetectionList dl;
    DeepLux::Detection d;
    d.x = 1;
    d.y = 2;
    d.width = 3;
    d.height = 4;
    d.score = 0.8;
    dl.items.append(d);
    QVERIFY(portValueMatchesType(QVariant::fromValue(dl), DataType::DetectionList));
    d.width = -1;
    dl.items[0] = d;
    QVERIFY(!portValueMatchesType(QVariant::fromValue(dl), DataType::DetectionList));
}

void TestPluginParameterContracts::testG2MeasurementModesThroughAgent() {
    ToolSchema::instance().registerDefaultTools();
    Project* project = ProjectManager::instance().newProject();
    QVERIFY(project);
    ModuleInstance instance;
    instance.id = QStringLiteral("measurement_modes");
    instance.moduleId = QStringLiteral("MeasurementInput");
    project->addModule(instance);
    AgentActor actor;
    const QList<QJsonObject> modes{
        QJsonObject{{"mode", "point_pair"}, {"point1", QJsonArray{1, 2}}, {"point2", QJsonArray{3, 4}}},
        QJsonObject{{"mode", "point_line"}, {"point", QJsonArray{1, 2, 3}}, {"line", QJsonArray{0, 0, 4, 5}}},
        QJsonObject{{"mode", "line_pair"}, {"line1", QJsonArray{0, 0, 1, 1}}, {"line2", QJsonArray{2, 2, 3, 3}}},
        QJsonObject{
            {"mode", "point_plane"}, {"point", QJsonArray{1, 2, 3}}, {"plane", QJsonArray{0, 0, 0, 1, 0, 0, 0, 1, 0}}},
        QJsonObject{{"mode", "point_set"}, {"points", QJsonArray{QJsonArray{1, 2}, QJsonArray{3, 4}}}}};
    for (const QJsonObject& values : modes) {
        const QJsonObject result =
            actor.executeTool("set_module_params", QJsonObject{{"instanceId", instance.id}, {"values", values}});
        QVERIFY2(!result.contains("error"), qPrintable(result.value("error").toString()));
        const QJsonObject current =
            actor.executeTool("get_module_params_schema", QJsonObject{{"instanceId", instance.id}})
                .value("currentParams")
                .toObject();
        for (auto it = values.begin(); it != values.end(); ++it)
            QCOMPARE(current.value(it.key()), it.value());
    }
    ProjectManager::instance().closeProject();
}

void TestPluginParameterContracts::testG2ComplexConfigRoundTrip() {
    ToolSchema::instance().registerDefaultTools();
    const QList<QPair<QString, QJsonObject>> cases{
        {QStringLiteral("MeasurementInput"),
         QJsonObject{{"mode", "point_set"}, {"points", QJsonArray{QJsonArray{12.0, 34.0}, QJsonArray{56.0, 78.0}}}}},
        {QStringLiteral("CropImage"), QJsonObject{{"rectangles", QJsonArray{QJsonArray{10.0, 20.0, 3.0, 4.0, 15.0},
                                                                            QJsonArray{50.0, 60.0, 5.0, 6.0, -30.0}}}}},
        {QStringLiteral("StrFormatPlugin"), QJsonObject{{"format", "%s:%d"},
                                                        {"inputVariables", QJsonArray{"${batch}", "001"}},
                                                        {"outputVariable", "formatted_report"}}},
        {QStringLiteral("PLCWritePlugin"), QJsonObject{{"startAddress", 42}, {"values", QJsonArray{0, 65535}}}},
        {QStringLiteral("SamPredict"), QJsonObject{{"points", QJsonArray{QJsonValue(QJsonArray{1.0, 2.0})}},
                                                   {"negativePoints", QJsonArray{QJsonValue(QJsonArray{3.0, 4.0})}},
                                                   {"box", QJsonArray{0.0, 0.0, 20.0, 30.0}}}}};

    Project* project = ProjectManager::instance().newProject();
    QVERIFY(project);
    AgentActor actor;
    for (const auto& item : cases) {
        QString pluginName;
        QString metadataPath;
        for (const PluginEntry& entry : m_entries) {
            if (entry.dirName == item.first) {
                pluginName = entry.name;
                metadataPath = entry.metadataPath;
                break;
            }
        }
        QVERIFY2(!pluginName.isEmpty(), qPrintable(item.first + " plugin not installed"));
        QFile metadataFile(metadataPath);
        QVERIFY(metadataFile.open(QIODevice::ReadOnly));
        const QJsonObject metadata = QJsonDocument::fromJson(metadataFile.readAll()).object();
        const QJsonObject schema = actor.executeTool("get_module_params_schema", QJsonObject{{"plugin", pluginName}});
        QCOMPARE(schema.value("pluginId"), metadata.value("id"));
        if (item.first == QStringLiteral("SamPredict")) {
            const QJsonObject box = schema.value("parameters").toObject().value("box").toObject();
            QCOMPARE(box.value("type").toString(), QStringLiteral("array"));
            QVERIFY(box.value("nullable").toBool());
            QCOMPARE(box.value("items").toObject().value("type").toString(), QStringLiteral("number"));
        }
        if (item.first == QStringLiteral("PLCWritePlugin")) {
            const QJsonObject parameters = schema.value("parameters").toObject();
            for (const QString& key : {QStringLiteral("port"), QStringLiteral("slaveId"), QStringLiteral("timeout")})
                QCOMPARE(parameters.value(key).toObject().value("min"), QJsonValue(1));
        }
        const QJsonObject declaredResults = metadata.value("ui").toObject().value("results").toObject();
        QVERIFY2(!declaredResults.isEmpty(), qPrintable(item.first + " has no result description"));
        const QJsonObject describedResults = schema.value("results").toObject();
        const QJsonObject expectedResults{
            {"MeasurementInput", QJsonArray{"point1", "point2", "measurement_input_mode"}},
            {"CropImage", QJsonArray{"crop_count", "crop_images"}},
            {"StrFormatPlugin", QJsonArray{"formattedString"}},
            {"PLCWritePlugin", QJsonArray{"plc_write_success", "plc_write_count"}},
            {"SamPredict", QJsonArray{"region", "score"}}};
        for (const QJsonValue& key : expectedResults.value(item.first).toArray()) {
            const QJsonObject field = describedResults.value(key.toString()).toObject();
            QVERIFY2(!field.value("label").toString().isEmpty(), qPrintable(item.first + ": " + key.toString()));
        }
        const QJsonObject sourcePorts = metadata.value("ports").toObject();
        const QJsonObject exposedPorts = schema.value("ports").toObject();
        for (const QString& direction : {QStringLiteral("inputs"), QStringLiteral("outputs")}) {
            const QJsonArray source = sourcePorts.value(direction).toArray();
            const QJsonArray exposed = exposedPorts.value(direction).toArray();
            QCOMPARE(exposed.size(), source.size());
            for (int i = 0; i < source.size(); ++i) {
                const QJsonObject declared = source[i].toObject();
                const QJsonObject described = exposed[i].toObject();
                for (const QString& field : {QStringLiteral("id"), QStringLiteral("type"), QStringLiteral("required"),
                                             QStringLiteral("multiple")})
                    QCOMPARE(described.value(field), declared.value(field));
                QCOMPARE(described.value("control").toBool(), declared.value("control").toBool());
            }
        }
        ModuleInstance instance;
        instance.id = item.first;
        instance.moduleId = pluginName;
        project->addModule(instance);
        const int undoBefore = actor.undoStack()->count();
        const QJsonObject result =
            actor.executeTool("set_module_params", QJsonObject{{"instanceId", instance.id}, {"values", item.second}});
        QVERIFY2(!result.contains("error"), qPrintable(item.first + ": " + result.value("error").toString()));
        QCOMPARE(actor.undoStack()->count(), undoBefore + 1);
        QCOMPARE(project->moduleById(instance.id)->params, item.second);
        QCOMPARE(actor.executeTool("get_module_params_schema", QJsonObject{{"instanceId", instance.id}})
                     .value("currentParams")
                     .toObject(),
                 item.second);
        if (item.first == QStringLiteral("StrFormatPlugin")) {
            const QJsonObject liveResults =
                actor.executeTool("get_module_params_schema", QJsonObject{{"instanceId", instance.id}})
                    .value("results")
                    .toObject();
            QVERIFY(liveResults.contains(QStringLiteral("formatted_report")));
            QVERIFY(!liveResults.contains(QStringLiteral("formattedString")));
        }
        actor.undoStack()->undo();
        QVERIFY(project->moduleById(instance.id)->params.isEmpty());
        QVERIFY(actor.executeTool("get_module_params_schema", QJsonObject{{"instanceId", instance.id}})
                    .value("currentParams")
                    .toObject()
                    .isEmpty());
        if (item.first == QStringLiteral("StrFormatPlugin"))
            QVERIFY(actor.executeTool("get_module_params_schema", QJsonObject{{"instanceId", instance.id}})
                        .value("results")
                        .toObject()
                        .contains(QStringLiteral("formattedString")));
        actor.undoStack()->redo();
        QCOMPARE(project->moduleById(instance.id)->params, item.second);
        QCOMPARE(actor.executeTool("get_module_params_schema", QJsonObject{{"instanceId", instance.id}})
                     .value("currentParams")
                     .toObject(),
                 item.second);
    }

    const QList<QPair<QString, QJsonObject>> invalidCases{
        {QStringLiteral("MeasurementInput"), QJsonObject{{"points", QJsonArray{QJsonValue(QJsonArray{1.0, "bad"})}}}},
        {QStringLiteral("CropImage"), QJsonObject{{"rectangles", QJsonArray{QJsonValue(QJsonArray{10, 20, -1, 4, 0})}}}},
        {QStringLiteral("StrFormatPlugin"), QJsonObject{{"inputVariables", QJsonArray{1, "valid"}}}},
        {QStringLiteral("PLCWritePlugin"), QJsonObject{{"values", QJsonArray{-1, 2}}}},
        {QStringLiteral("PLCWritePlugin"), QJsonObject{{"port", 0}}},
        {QStringLiteral("SamPredict"), QJsonObject{{"box", QJsonArray{0, 0, -1, 20}}}}};
    for (const auto& item : invalidCases) {
        const QJsonObject before = project->moduleById(item.first)->params;
        const int undoBefore = actor.undoStack()->count();
        const QJsonObject rejected =
            actor.executeTool("set_module_params", QJsonObject{{"instanceId", item.first}, {"values", item.second}});
        QCOMPARE(rejected.value("errorCode").toString(), QStringLiteral("VALIDATION_FAILED"));
        QCOMPARE(project->moduleById(item.first)->params, before);
        QCOMPARE(actor.undoStack()->count(), undoBefore);
    }

    const QString path = m_tempDir.filePath(QStringLiteral("g2_complex_params.dproj"));
    QVERIFY(project->save(path));
    Project reopened;
    QVERIFY(reopened.load(path));
    for (const auto& item : cases)
        QCOMPARE(reopened.moduleById(item.first)->params, item.second);
    project = ProjectManager::instance().openProject(path);
    QVERIFY(project);
    for (const auto& item : cases)
        QCOMPARE(actor.executeTool("get_module_params_schema", QJsonObject{{"instanceId", item.first}})
                     .value("currentParams")
                     .toObject(),
                 item.second);
    QVERIFY(actor.executeTool("get_module_params_schema", QJsonObject{{"instanceId", "StrFormatPlugin"}})
                .value("results")
                .toObject()
                .contains(QStringLiteral("formatted_report")));
    ProjectManager::instance().closeProject();
}

void TestPluginParameterContracts::testGapMeasure3DRoiCentersDescribeInputPixelGrid() {
    QString metadataPath;
    for (const PluginEntry& entry : m_entries) {
        if (entry.name == QStringLiteral("GapMeasure3D")) {
            metadataPath = entry.metadataPath;
            break;
        }
    }
    QVERIFY(!metadataPath.isEmpty());

    QFile metadataFile(metadataPath);
    QVERIFY(metadataFile.open(QIODevice::ReadOnly));
    const QJsonObject parameters = QJsonDocument::fromJson(metadataFile.readAll())
                                       .object()
                                       .value(QStringLiteral("ui"))
                                       .toObject()
                                       .value(QStringLiteral("parameters"))
                                       .toObject();

    ToolSchema::instance().registerDefaultTools();
    AgentActor actor;
    const QJsonObject schema =
        actor.executeTool(QStringLiteral("get_module_params_schema"),
                          QJsonObject{{QStringLiteral("plugin"), QStringLiteral("GapMeasure3D")}});
    QVERIFY2(!schema.contains(QStringLiteral("error")), qPrintable(schema.value(QStringLiteral("error")).toString()));
    const QJsonObject agentParameters = schema.value(QStringLiteral("parameters")).toObject();
    const QString semantic = QStringLiteral("当前输入高度图局部像素");
    for (const QString& key : {QStringLiteral("roiCenterX"), QStringLiteral("roiCenterY")}) {
        const QString metadataLabel = parameters.value(key).toObject().value(QStringLiteral("label")).toString();
        const QString agentLabel = agentParameters.value(key).toObject().value(QStringLiteral("label")).toString();
        QVERIFY2(metadataLabel.contains(semantic),
                 qPrintable(key + QStringLiteral(" metadata label: ") + metadataLabel));
        QCOMPARE(agentLabel, metadataLabel);
    }
}

void TestPluginParameterContracts::testGapMeasure3DZOutputsDoNotClaimMillimeters() {
    QString metadataPath;
    for (const PluginEntry& entry : m_entries) {
        if (entry.name == QStringLiteral("GapMeasure3D")) {
            metadataPath = entry.metadataPath;
            break;
        }
    }
    QVERIFY(!metadataPath.isEmpty());

    QFile metadataFile(metadataPath);
    QVERIFY(metadataFile.open(QIODevice::ReadOnly));
    const QJsonObject metadata = QJsonDocument::fromJson(metadataFile.readAll()).object();
    const QJsonObject parameter = metadata.value(QStringLiteral("ui"))
                                      .toObject()
                                      .value(QStringLiteral("parameters"))
                                      .toObject()
                                      .value(QStringLiteral("zScale"))
                                      .toObject();
    const QString zScaleLabel = parameter.value(QStringLiteral("label")).toString();
    QVERIFY(zScaleLabel.contains(QStringLiteral("单位取决于输入高度值和标定")));
    QVERIFY(!zScaleLabel.contains(QStringLiteral("mm"), Qt::CaseInsensitive));

    const QJsonArray outputs =
        metadata.value(QStringLiteral("ports")).toObject().value(QStringLiteral("outputs")).toArray();
    for (const QString& key : {QStringLiteral("corner_dz_mm"), QStringLiteral("corner_dist_mm")}) {
        const auto it = std::find_if(outputs.cbegin(), outputs.cend(), [&key](const QJsonValue& value) {
            return value.toObject().value(QStringLiteral("id")).toString() == key;
        });
        QVERIFY(it != outputs.cend());
        const QString displayName = it->toObject().value(QStringLiteral("displayName")).toString();
        QVERIFY(!displayName.contains(QStringLiteral("mm"), Qt::CaseInsensitive));
    }
}

void TestPluginParameterContracts::testHeightMapRoiMetadataMatchesAgentSchema() {
    const QMap<QString, QMap<QString, QStringList>> expected{
        {QStringLiteral("3DPreProcessing"),
         {{QStringLiteral("roiCenterX"), {QStringLiteral("当前输入深度/高度图局部像素")}},
          {QStringLiteral("roiCenterY"), {QStringLiteral("当前输入深度/高度图局部像素")}},
          {QStringLiteral("roiWidth"), {QStringLiteral("像素"), QStringLiteral("0=全图")}},
          {QStringLiteral("roiHeight"), {QStringLiteral("像素"), QStringLiteral("0=全图")}}}},
        {QStringLiteral("FitPlane"),
         {{QStringLiteral("roiCenterX"), {QStringLiteral("当前输入高度图局部像素")}},
          {QStringLiteral("roiCenterY"), {QStringLiteral("当前输入高度图局部像素")}},
          {QStringLiteral("roiLength1"), {QStringLiteral("像素"), QStringLiteral("0=全图")}},
          {QStringLiteral("roiLength2"), {QStringLiteral("像素"), QStringLiteral("0=全图")}}}}};

    ToolSchema::instance().registerDefaultTools();
    AgentActor actor;
    for (auto plugin = expected.cbegin(); plugin != expected.cend(); ++plugin) {
        QString metadataPath;
        for (const PluginEntry& entry : m_entries) {
            if (entry.name == plugin.key()) {
                metadataPath = entry.metadataPath;
                break;
            }
        }
        QVERIFY(!metadataPath.isEmpty());
        QFile metadataFile(metadataPath);
        QVERIFY(metadataFile.open(QIODevice::ReadOnly));
        const QJsonObject metadataParameters = QJsonDocument::fromJson(metadataFile.readAll())
                                                   .object()
                                                   .value(QStringLiteral("ui"))
                                                   .toObject()
                                                   .value(QStringLiteral("parameters"))
                                                   .toObject();
        const QJsonObject schema = actor.executeTool(QStringLiteral("get_module_params_schema"),
                                                     QJsonObject{{QStringLiteral("plugin"), plugin.key()}});
        QVERIFY2(!schema.contains(QStringLiteral("error")),
                 qPrintable(plugin.key() + QStringLiteral(": ") + schema.value(QStringLiteral("error")).toString()));
        const QJsonObject agentParameters = schema.value(QStringLiteral("parameters")).toObject();
        for (auto parameter = plugin->cbegin(); parameter != plugin->cend(); ++parameter) {
            const QString metadataLabel =
                metadataParameters.value(parameter.key()).toObject().value(QStringLiteral("label")).toString();
            const QString agentLabel =
                agentParameters.value(parameter.key()).toObject().value(QStringLiteral("label")).toString();
            QCOMPARE(agentLabel, metadataLabel);
            for (const QString& fragment : parameter.value())
                QVERIFY2(metadataLabel.contains(fragment),
                         qPrintable(plugin.key() + QLatin1Char('.') + parameter.key() + QStringLiteral(": ") +
                                    metadataLabel));
        }
    }
}

void TestPluginParameterContracts::testMeasureCirclePixelParametersDescribeInputGrid() {
    QString metadataPath;
    for (const PluginEntry& entry : m_entries) {
        if (entry.name == QStringLiteral("MeasureCircle")) {
            metadataPath = entry.metadataPath;
            break;
        }
    }
    QVERIFY(!metadataPath.isEmpty());

    QFile metadataFile(metadataPath);
    QVERIFY(metadataFile.open(QIODevice::ReadOnly));
    const QJsonObject metadataParameters = QJsonDocument::fromJson(metadataFile.readAll())
                                               .object()
                                               .value(QStringLiteral("ui"))
                                               .toObject()
                                               .value(QStringLiteral("parameters"))
                                               .toObject();
    ToolSchema::instance().registerDefaultTools();
    AgentActor actor;
    const QJsonObject schema =
        actor.executeTool(QStringLiteral("get_module_params_schema"),
                          QJsonObject{{QStringLiteral("plugin"), QStringLiteral("MeasureCircle")}});
    QVERIFY2(!schema.contains(QStringLiteral("error")), qPrintable(schema.value(QStringLiteral("error")).toString()));
    const QJsonObject agentParameters = schema.value(QStringLiteral("parameters")).toObject();
    const QList<QPair<QString, QString>> fields{{QStringLiteral("initialCenterX"), QStringLiteral("初始圆心X")},
                                                {QStringLiteral("initialCenterY"), QStringLiteral("初始圆心Y")},
                                                {QStringLiteral("initialRadius"), QStringLiteral("初始半径")},
                                                {QStringLiteral("searchLength"), QStringLiteral("搜索半长")},
                                                {QStringLiteral("exclusionRadius"), QStringLiteral("残差剔除半径")}};
    for (const auto& field : fields) {
        const QString metadataLabel =
            metadataParameters.value(field.first).toObject().value(QStringLiteral("label")).toString();
        const QString agentLabel =
            agentParameters.value(field.first).toObject().value(QStringLiteral("label")).toString();
        QCOMPARE(agentLabel, metadataLabel);
        QVERIFY(metadataLabel.contains(field.second));
        QVERIFY(metadataLabel.contains(QStringLiteral("当前输入图像局部像素")));
        for (const QString& forbidden : {QStringLiteral("来源"), QStringLiteral("物理"), QStringLiteral("毫米"),
                                         QStringLiteral("World3D"), QStringLiteral("mm")})
            QVERIFY2(!metadataLabel.contains(forbidden),
                     qPrintable(field.first + QStringLiteral(" contains forbidden claim: ") + metadataLabel));
    }
}

void TestPluginParameterContracts::testCropImageRoiMetadataSchemaAndGuiDescribePixels() {
    QString metadataPath;
    for (const PluginEntry& entry : m_entries) {
        if (entry.name == QStringLiteral("CropImage")) {
            metadataPath = entry.metadataPath;
            break;
        }
    }
    QVERIFY(!metadataPath.isEmpty());

    QFile metadataFile(metadataPath);
    QVERIFY(metadataFile.open(QIODevice::ReadOnly));
    const QJsonObject metadataParameters = QJsonDocument::fromJson(metadataFile.readAll())
                                               .object()
                                               .value(QStringLiteral("ui"))
                                               .toObject()
                                               .value(QStringLiteral("parameters"))
                                               .toObject();
    const QString metadataLabel =
        metadataParameters.value(QStringLiteral("rectangles")).toObject().value(QStringLiteral("label")).toString();
    ToolSchema::instance().registerDefaultTools();
    AgentActor actor;
    const QJsonObject schema = actor.executeTool(QStringLiteral("get_module_params_schema"),
                                                 QJsonObject{{QStringLiteral("plugin"), QStringLiteral("CropImage")}});
    QVERIFY2(!schema.contains(QStringLiteral("error")), qPrintable(schema.value(QStringLiteral("error")).toString()));
    const QString agentLabel = schema.value(QStringLiteral("parameters"))
                                   .toObject()
                                   .value(QStringLiteral("rectangles"))
                                   .toObject()
                                   .value(QStringLiteral("label"))
                                   .toString();
    QCOMPARE(agentLabel, metadataLabel);
    for (const QString& fragment :
         {QStringLiteral("当前输入图像局部像素中心"), QStringLiteral("l1/l2 为像素半长"), QStringLiteral("deg 为度")})
        QVERIFY2(metadataLabel.contains(fragment), qPrintable(metadataLabel));
    for (const QString& forbidden : {QStringLiteral("来源图"), QStringLiteral("物理"), QStringLiteral("毫米"),
                                     QStringLiteral("World3D"), QStringLiteral("mm")})
        QVERIFY2(!metadataLabel.contains(forbidden), qPrintable(metadataLabel));

    auto module = PluginManager::instance().createManagedModule(QStringLiteral("CropImage"));
    QVERIFY(module);
    std::unique_ptr<QWidget> config(module->createConfigWidget());
    QVERIFY(config);
    QString guiLabels;
    bool foundFormLabel = false;
    for (QLabel* label : config->findChildren<QLabel*>()) {
        guiLabels += label->text() + QLatin1Char('\n');
        foundFormLabel = foundFormLabel || label->text() == QStringLiteral("矩形数组（输入图像局部像素，半长为像素）");
    }
    for (const QString& fragment :
         {QStringLiteral("当前输入图像局部像素中心"), QStringLiteral("半长1/2 为像素"), QStringLiteral("deg 为度"),
          QStringLiteral("HALCON Length1/Length2"), QStringLiteral("整边长 = 2×半长")})
        QVERIFY2(guiLabels.contains(fragment), qPrintable(guiLabels));
    QVERIFY(foundFormLabel);
    QVERIFY(guiLabels.contains(QStringLiteral("输入图像局部像素")));
}

QTEST_MAIN(TestPluginParameterContracts)
#include "test_pluginparametercontracts.moc"
