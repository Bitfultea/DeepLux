#!/usr/bin/env python3

import json
import sys
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).parent))
import check_plugin_affinity as LINT

RULES = {
    "forbidden_identifiers": ["qApp", "QWidget", "QTimer", "QTcpSocket", "QSerialPort", "QProcess"],
    "forbidden_calls": ["moveToThread", "deleteLater", "processEvents"],
    "forbidden_sequences": [
        {"tokens": ["new", "QObject"]},
        {"tokens": ["QMetaObject", "::", "invokeMethod", "("]},
    ],
    "global_manager_write_calls": ["setVariableValue"],
    "run_engine_write_calls": ["setOutput", "stop"],
    "global_mutating_methods": ["append", "assign", "clear", "insert", "push_back", "remove", "resize"],
    "run_engine_read_allowlist": [
        {"plugin_id": "com.test.if", "function": "readOutput", "method": "getOutput"}
    ],
}


class PluginAffinityLintTest(unittest.TestCase):
    def test_delay_is_not_parallel_eligible(self):
        metadata_path = LINT.METADATA_ROOT / "logic" / "Delay" / "metadata.json"
        execution = json.loads(metadata_path.read_text(encoding="utf-8")).get("execution", {})
        self.assertFalse(execution.get("threadSafe", False))
        self.assertEqual(execution.get("affinity", "gui"), "worker")

    def scan(self, source, plugin_id="com.test.plugin"):
        return LINT.analyze_source("fixture.cpp", source, plugin_id, RULES)

    def test_ignores_config_widget_and_comments(self):
        findings = self.scan(
            """
            bool Demo::process() { return helper(); }
            bool Demo::helper() { const char* text = \"qApp QWidget\"; return true; }
            QWidget* Demo::createConfigWidget() { return new QWidget(); }
            // process body may use no GUI APIs
            """
        )
        self.assertEqual(findings, [])

    def test_reaches_private_helpers(self):
        findings = self.scan(
            """
            bool Demo::process() { return helper(); }
            bool Demo::helper() { QMetaObject::invokeMethod(this, \"refresh\"); return true; }
            """
        )
        self.assertTrue(any("QMetaObject :: invokeMethod" in item for item in findings), findings)

    def test_reaches_helpers_in_other_source_files(self):
        findings = LINT.analyze_plugin(
            {
                "plugin.cpp": "bool Demo::process() { return sharedHelper(); }",
                "helper.cpp": "bool sharedHelper() { qApp->processEvents(); return true; }",
            },
            "com.test.plugin",
            RULES,
        )
        self.assertTrue(any("helper.cpp" in item and "qApp" in item for item in findings), findings)

    def test_does_not_reach_unrelated_same_name_member_helper(self):
        findings = self.scan(
            """
            struct Plugin { bool process(); bool helper(); };
            struct Config { bool helper(); };
            bool Plugin::process() { return helper(); }
            bool Plugin::helper() { return true; }
            bool Config::helper() { qApp->processEvents(); return true; }
            """
        )
        self.assertEqual(findings, [])

    def test_reaches_this_and_qualified_helpers(self):
        findings = self.scan(
            """
            struct Plugin { bool process(); bool helper(); };
            bool Plugin::process() { return this->helper(); }
            bool Plugin::helper() { qApp->processEvents(); return true; }
            """
        )
        self.assertTrue(any("::helper:" in item and "qApp" in item for item in findings), findings)

        findings = self.scan(
            """
            struct Plugin { bool process(); };
            namespace Helpers { bool run(); }
            bool Plugin::process() { return Helpers::run(); }
            bool Helpers::run() { qApp->processEvents(); return true; }
            """
        )
        self.assertTrue(any("::run:" in item and "qApp" in item for item in findings), findings)

    def test_scans_immediately_invoked_lambda_body(self):
        findings = self.scan(
            """
            bool Demo::process() { [] { qApp->processEvents(); }(); return true; }
            """
        )
        self.assertTrue(any("qApp" in item for item in findings), findings)
        self.assertTrue(any("processEvents()" in item for item in findings), findings)

    def test_detects_thread_and_engine_mutations(self):
        findings = self.scan(
            """
            bool Demo::process() {
                QObject* object = new QObject();
                object->moveToThread(worker);
                RunEngine::instance().stop();
                return true;
            }
            """
        )
        self.assertTrue(any("new QObject" in item for item in findings), findings)
        self.assertTrue(any("moveToThread()" in item for item in findings), findings)
        self.assertTrue(any("RunEngine write call stop()" in item for item in findings), findings)

    def test_does_not_scan_unreachable_helper(self):
        findings = self.scan(
            """
            bool Demo::process() { return true; }
            bool Demo::unused() { qApp->processEvents(); return true; }
            """
        )
        self.assertEqual(findings, [])

    def test_detects_global_writes_and_manager_mutations(self):
        findings = self.scan(
            """
            namespace Outer {
            namespace {
                int sharedCount = 0, sharedSecond = 0;
                int sharedValues[2] = {};
                struct SharedState { int value; };
                SharedState sharedState{};
            }
            }
            bool Demo::process() {
                static int localCount = 0;
                ++sharedCount;
                ++sharedSecond;
                sharedValues[0] = 1;
                sharedState.value++;
                ++localCount;
                GlobalVarManager::instance().setVariableValue(\"x\", 1);
                return true;
            }
            """
        )
        self.assertTrue(any("write to file-scope variable sharedCount" in item for item in findings), findings)
        self.assertTrue(any("write to file-scope variable sharedSecond" in item for item in findings), findings)
        self.assertTrue(any("write to file-scope variable sharedValues" in item for item in findings), findings)
        self.assertTrue(any("write to file-scope variable sharedState" in item for item in findings), findings)
        self.assertTrue(any("write to function-static variable localCount" in item for item in findings), findings)
        self.assertTrue(any("GlobalVarManager write call" in item for item in findings), findings)

    def test_detects_in_place_file_scope_mutations(self):
        findings = self.scan(
            """
            std::vector<int> cache;
            struct SharedState { void clear(); };
            SharedState sharedState;
            bool Demo::process() {
                cache.push_back(1);
                sharedState.clear();
                return true;
            }
            """
        )
        self.assertTrue(any("cache via push_back()" in item for item in findings), findings)
        self.assertTrue(any("sharedState via clear()" in item for item in findings), findings)

    def test_ignores_local_shadow_and_detects_reference_alias_mutation(self):
        findings = self.scan(
            """
            std::vector<int> cache;
            bool Demo::process() {
                std::vector<int> cache;
                cache.push_back(1);
                auto& shared = cache;
                shared.push_back(2);
                return true;
            }
            """
        )
        self.assertEqual(findings, [])

        findings = self.scan(
            """
            std::vector<int> cache;
            bool Demo::process() {
                auto& shared = cache;
                shared.push_back(2);
                return true;
            }
            """
        )
        self.assertTrue(any("cache via push_back()" in item for item in findings), findings)

        findings = self.scan(
            """
            std::vector<int> cache;
            bool Demo::process() {
                auto* pointer = &cache;
                pointer->push_back(2);
                cache.assign(1, 2);
                return true;
            }
            """
        )
        self.assertTrue(any("cache via push_back()" in item for item in findings), findings)
        self.assertTrue(any("cache via assign()" in item for item in findings), findings)

        findings = self.scan(
            """
            std::vector<int> cache;
            bool Demo::process() {
                auto* pointer = &cache;
                (*pointer).push_back(2);
                return true;
            }
            """
        )
        self.assertTrue(any("cache via push_back()" in item for item in findings), findings)

        findings = self.scan(
            """
            std::vector<std::vector<int>> cache;
            bool Demo::process() {
                auto* pointer = &cache;
                (*pointer)[0].push_back(2);
                return true;
            }
            """
        )
        self.assertTrue(any("cache via push_back()" in item for item in findings), findings)

        findings = self.scan(
            """
            std::vector<int> cache;
            std::vector<int> other;
            bool Demo::process() {
                std::vector<int> other;
                std::vector<int> local;
                auto* pointer = &cache;
                pointer = &local;
                pointer->push_back(2);
                pointer = &other;
                pointer->push_back(3);
                return true;
            }
            """
        )
        self.assertEqual(findings, [])

    def test_detects_combined_struct_and_global_instance_declaration(self):
        findings = self.scan(
            """
            struct SharedState { void clear(); } sharedState;
            bool Demo::process() { sharedState.clear(); return true; }
            """
        )
        self.assertTrue(any("sharedState via clear()" in item for item in findings), findings)

        findings = self.scan(
            """
            struct SharedState { void clear(); } first, second;
            bool Demo::process() { first.clear(); second.clear(); return true; }
            """
        )
        self.assertTrue(any("first via clear()" in item for item in findings), findings)
        self.assertTrue(any("second via clear()" in item for item in findings), findings)

    def test_reference_alias_shadowing_and_assignment(self):
        findings = self.scan(
            """
            std::vector<int> cache;
            bool Demo::process() {
                auto& shared = cache;
                { std::vector<int> shared; shared.push_back(1); }
                return true;
            }
            """
        )
        self.assertEqual(findings, [])

        findings = self.scan(
            """
            int state = 0;
            bool Demo::process() { auto& alias = state; alias = 2; return true; }
            """
        )
        self.assertTrue(any("state via reference alias alias" in item for item in findings), findings)

    def test_rejects_thread_sensitive_qt_api_construction(self):
        findings = self.scan(
            """
            bool Demo::process() {
                QTimer timer;
                timer.start();
                QTcpSocket socket;
                QSerialPort serial;
                QProcess process;
                return true;
            }
            """
        )
        for api in ("QTimer", "QTcpSocket", "QSerialPort", "QProcess"):
            self.assertTrue(any(api in item for item in findings), findings)

    def test_rejects_thread_sensitive_qt_type_aliases(self):
        findings = self.scan(
            """
            using Timer = QTimer;
            typedef QTcpSocket Socket;
            bool Demo::process() { Timer timer; Socket socket; return true; }
            """
        )
        self.assertTrue(any("forbidden execution-path symbol Timer" in item for item in findings), findings)

        self.assertTrue(any("forbidden execution-path symbol Socket" in item for item in findings), findings)

        findings = self.scan(
            """
            using Timer = const QTimer;
            bool Demo::process() { Timer timer; return true; }
            """
        )
        self.assertTrue(any("forbidden execution-path symbol Timer" in item for item in findings), findings)

        findings = self.scan(
            """
            namespace Gui { using Timer = QTimer; }
            namespace Worker {
                struct Timer {};
                bool process() { Timer timer; return true; }
            }
            """
        )
        self.assertEqual(findings, [])

        findings = self.scan(
            """
            namespace A::Gui { using Timer = QTimer; }
            namespace A::Worker {
                struct Timer {};
                bool process() { Timer timer; return true; }
            }
            """
        )
        self.assertEqual(findings, [])

        findings = self.scan(
            """
            using Timer = QTimer;
            void unrelated() { struct Timer {}; }
            bool Demo::process() { Timer timer; return true; }
            """
        )
        self.assertTrue(any("forbidden execution-path symbol Timer" in item for item in findings), findings)

        findings = self.scan(
            """
            namespace A::Gui { using Timer = QTimer; }
            namespace A::Gui::Worker {
                struct Timer {};
                bool Demo::process() { A::Gui::Timer timer; return true; }
            }
            """
        )
        self.assertTrue(any("forbidden execution-path symbol Timer" in item for item in findings), findings)

    def test_detects_alias_in_conditional_noexcept_entry(self):
        findings = self.scan(
            """
            bool Demo::process() noexcept(true) { QTimer timer; return true; }
            """
        )
        self.assertTrue(any("QTimer" in item for item in findings), findings)

    def test_type_aliases_do_not_leak_across_translation_units(self):
        findings = LINT.analyze_plugin(
            {
                "one.cpp": "namespace { using Timer = QTimer; }",
                "two.cpp": """
                    namespace { struct Timer {}; }
                    bool Demo::process() { Timer timer; return true; }
                """,
            },
            "com.test.plugin",
            RULES,
        )
        self.assertEqual(findings, [])

        findings = LINT.analyze_plugin(
            {
                "plugin.cpp": '#include "gui_alias.h"\nbool Demo::process() { Timer timer; return true; }',
                "gui_alias.h": "using Timer = QTimer;",
            },
            "com.test.plugin",
            RULES,
        )
        self.assertTrue(any("forbidden execution-path symbol Timer" in item for item in findings), findings)

        findings = self.scan(
            """
            namespace {
                using Timer = QTimer;
            }
            namespace {
                bool Demo::process() { Timer timer; return true; }
            }
            """
        )
        self.assertTrue(any("forbidden execution-path symbol Timer" in item for item in findings), findings)

        findings = self.scan(
            """
            namespace { using Timer = QTimer; }
            bool Demo::process() { Timer timer; return true; }
            """
        )
        self.assertTrue(any("forbidden execution-path symbol Timer" in item for item in findings), findings)

        findings = self.scan(
            """
            namespace A {
                namespace { using Timer = QTimer; }
                bool Demo::process() { Timer timer; return true; }
            }
            """
        )
        self.assertTrue(any("forbidden execution-path symbol Timer" in item for item in findings), findings)

    def test_local_type_aliases_stay_with_their_function(self):
        findings = self.scan(
            """
            namespace Worker {
                bool Demo::process() { struct Timer {}; Timer timer; return true; }
                bool Demo::unusedHelper() { using Timer = QTimer; return true; }
            }
            """
        )
        self.assertEqual(findings, [])

    def test_qualified_and_imported_type_aliases_are_detected(self):
        findings = self.scan(
            """
            namespace Gui { using Timer = QTimer; }
            namespace Demo { bool process() { Gui::Timer timer; return true; } }
            """
        )
        self.assertTrue(any("forbidden execution-path symbol Timer" in item for item in findings), findings)

        findings = self.scan(
            """
            namespace Gui { using Timer = QTimer; }
            namespace Worker {
                using Gui::Timer;
                bool process() { Timer timer; return true; }
            }
            """
        )
        self.assertTrue(any("forbidden execution-path symbol Timer" in item for item in findings), findings)

    def test_local_type_shadow_suppresses_namespace_alias(self):
        findings = self.scan(
            """
            namespace Worker {
                using Timer = QTimer;
                bool process() { struct Timer {}; Timer timer; return true; }
            }
            """
        )
        self.assertEqual(findings, [])

        findings = self.scan(
            """
            namespace Worker {
                using Timer = QTimer;
                bool process() {
                    Timer outer;
                    { struct Timer {}; Timer local; }
                    return true;
                }
            }
            """
        )
        self.assertTrue(any("forbidden execution-path symbol Timer" in item for item in findings), findings)

    def test_unqualified_helper_does_not_reach_sibling_namespace(self):
        findings = self.scan(
            """
            namespace A { bool process() { return helper(); } }
            namespace B { bool helper() { qApp->processEvents(); return true; } }
            """
        )
        self.assertEqual(findings, [])

        findings = self.scan(
            """
            bool Demo::process() {
                using Timer = QTimer;
                Timer timer;
                return true;
            }
            """
        )
        self.assertTrue(any("forbidden execution-path symbol Timer" in item for item in findings), findings)

        findings = self.scan(
            """
            namespace {
                auto unusedHelper() -> int { using Timer = QTimer; return 0; }
                bool Demo::process() { struct Timer {}; Timer timer; return true; }
            }
            """
        )
        self.assertEqual(findings, [])

        findings = self.scan(
            """
            namespace A::Gui { using Timer = QTimer; }
            namespace A::Gui::Worker {
                struct Timer {};
                bool Demo::process() { Timer timer; return true; }
            }
            """
        )
        self.assertEqual(findings, [])

    def test_detects_static_class_member_writes(self):
        findings = self.scan(
            """
            struct Shared { inline static int count = 0; };
            bool Demo::process() { ++Shared::count; return true; }
            """
        )
        self.assertTrue(any("write to static member Shared::count" in item for item in findings), findings)

    def test_ignores_local_shadow_of_static_member(self):
        findings = self.scan(
            """
            struct Shared { inline static int count = 0; };
            bool Shared::process() { int count = 0; ++count; return true; }
            """
        )
        self.assertEqual(findings, [])

    def test_detects_singleton_write_aliases(self):
        findings = self.scan(
            """
            bool Demo::process() {
                auto& engine = RunEngine::instance();
                engine.stop();
                {
                    struct FakeEngine { void stop(); };
                    FakeEngine engine;
                    engine.stop();
                }
                auto& variables = GlobalVarManager::instance();
                variables.setVariableValue("x", 1);
                return true;
            }
            """
        )
        self.assertEqual(sum("RunEngine write call stop()" in item for item in findings), 1, findings)
        self.assertTrue(any("GlobalVarManager write call setVariableValue()" in item for item in findings), findings)

    def test_run_engine_read_requires_exact_allowlist(self):
        source = """
            bool Demo::process() { return readOutput(); }
            bool Demo::readOutput() { return RunEngine::instance().getOutput(\"a\", \"b\").toBool(); }
            """
        self.assertEqual(self.scan(source, "com.test.if"), [])
        self.assertTrue(self.scan(source, "com.test.other"))


if __name__ == "__main__":
    unittest.main()
