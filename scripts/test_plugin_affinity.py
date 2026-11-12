#!/usr/bin/env python3

import sys
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).parent))
import check_plugin_affinity as LINT

RULES = {
    "forbidden_identifiers": ["qApp", "QWidget"],
    "forbidden_calls": ["moveToThread", "deleteLater", "processEvents"],
    "forbidden_sequences": [
        {"tokens": ["new", "QObject"]},
        {"tokens": ["QMetaObject", "::", "invokeMethod", "("]},
    ],
    "global_manager_write_calls": ["setVariableValue"],
    "run_engine_write_calls": ["setOutput", "stop"],
    "run_engine_read_allowlist": [
        {"plugin_id": "com.test.if", "function": "readOutput", "method": "getOutput"}
    ],
}


class PluginAffinityLintTest(unittest.TestCase):
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
