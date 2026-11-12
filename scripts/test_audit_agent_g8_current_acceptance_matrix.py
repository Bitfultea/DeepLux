#!/usr/bin/env python3
"""Unit tests for the conservative current G8 acceptance matrix."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

SCRIPT = Path(__file__).with_name("audit_agent_g8_current_acceptance_matrix.py")
SPEC = importlib.util.spec_from_file_location("audit_agent_g8_current_acceptance_matrix", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MATRIX = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MATRIX)


class TestCurrentG8AcceptanceMatrix(unittest.TestCase):
    def test_default_ctest_log_matches_the_current_matrix_source(self):
        import json

        current = json.loads(MATRIX.JSON_PATH.read_text(encoding="utf-8"))
        self.assertEqual(
            MATRIX.DEFAULT_CTEST_LOG.relative_to(MATRIX.ROOT).as_posix(),
            current["sources"]["currentFullCtestLog"],
        )

    def test_selected_qt_uses_link_evidence_and_does_not_guess(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            link = root / "build-codex-agent/tests/CMakeFiles/test_mainwindow.dir/link.txt"
            link.parent.mkdir(parents=True)
            link.write_text("/usr/lib/libQt5Core.so.5.15.3 /usr/lib/libQt5Test.so.5.15.3", encoding="utf-8")
            selected = MATRIX._selected_qt(
                root,
                {"USE_QT6": "ON", "Qt6Core_DIR": "Qt6Core_DIR-NOTFOUND", "Qt5Core_DIR": "/qt5"},
            )
            self.assertEqual(selected["requestedMajor"], 6)
            self.assertEqual(selected["selectedMajor"], 5)
            self.assertTrue(selected["fallback"])
            self.assertEqual(selected["evidencePath"], "build-codex-agent/tests/CMakeFiles/test_mainwindow.dir/link.txt")

            link.write_text("Qt6Core.lib Qt6Test.lib", encoding="utf-8")
            selected = MATRIX._selected_qt(root, {"USE_QT6": "ON"})
            self.assertEqual(selected["selectedMajor"], 6)
            self.assertFalse(selected["fallback"])

            link.write_text("Qt5Core.lib Qt6Core.lib", encoding="utf-8")
            selected = MATRIX._selected_qt(root, {"USE_QT6": "ON"})
            self.assertIsNone(selected["selectedMajor"])
            self.assertIsNone(selected["fallback"])

            link.unlink()
            selected = MATRIX._selected_qt(root, {"USE_QT6": "ON"})
            self.assertIsNone(selected["selectedMajor"])
            selected = MATRIX._selected_qt(root, {"USE_QT6": "ON", "Qt5Core_DIR": "/qt5"})
            self.assertEqual(selected["selectedMajor"], 5)
            self.assertTrue(selected["fallback"])

    def test_plugin_classifications_keep_failures_and_environment_blocks(self):
        self.assertEqual(
            MATRIX.classify_plugin(
                {
                    "binaryPresentNow": True,
                    "validBusinessFixtureStatus": "PASS",
                    "criticalFailureFixtureStatus": "PASS",
                    "fixtureAuditReviewStatus": "PASS",
                }
            ),
            "PASS_SOFTWARE_FIXTURES",
        )
        self.assertEqual(
            MATRIX.classify_plugin(
                {"binaryPresentNow": True, "validBusinessFixtureStatus": "FAIL", "criticalFailureFixtureStatus": "PASS"}
            ),
            "FAIL_BUSINESS_FIXTURE",
        )
        self.assertEqual(
            MATRIX.classify_plugin(
                {"binaryPresentNow": False, "platform": "windows", "validBusinessFixtureStatus": "NOT_AUDITED"}
            ),
            "BLOCKED_PLATFORM",
        )
        self.assertEqual(
            MATRIX.classify_plugin(
                {
                    "binaryPresentNow": False,
                    "platform": "linux",
                    "buildDisabledReason": "MVS SDK unavailable",
                    "validBusinessFixtureStatus": "NOT_AUDITED",
                }
            ),
            "BLOCKED_SDK_OR_BUILD_OPTION",
        )
        self.assertEqual(
            MATRIX.classify_plugin(
                {"binaryPresentNow": False, "platform": "linux", "validBusinessFixtureStatus": "NOT_AUDITED"}
            ),
            "NOT_BUILT",
        )

    def test_operation_classification_requires_resolving_test_and_passing_suite(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "tests/test_fixture.cpp"
            source.parent.mkdir()
            source.write_text(
                "class TestFixture { private slots: void testResolves(); };\n"
                "void TestFixture::testResolves() {}\n",
                encoding="utf-8",
            )
            (root / "tests/CMakeLists.txt").write_text("set(TEST_SOURCES\ntest_fixture.cpp\n)\n", encoding="utf-8")
            base = {"toolMapping": "直接工具", "testId": "testResolves", "evidencePath": "tests/test_fixture.cpp"}
            self.assertEqual(MATRIX.classify_operation(base, root, {"test_fixture"}), "EVIDENCE_LINKED")
            with self.assertRaisesRegex(ValueError, "suite is not passing"):
                MATRIX.classify_operation(base, root, set())
            with self.assertRaisesRegex(ValueError, "testId does not resolve"):
                MATRIX.classify_operation({**base, "testId": "missingTest"}, root, {"test_fixture"})
            with self.assertRaisesRegex(ValueError, "incomplete operation evidence"):
                MATRIX.classify_operation({"operationId": "broken", "testId": "testResolves"}, root, {"test_fixture"})
            unregistered = root / "tests/test_unregistered.cpp"
            unregistered.write_text("void TestFixture::testResolves() {}\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "source is not registered"):
                MATRIX.classify_operation(
                    {**base, "evidencePath": "tests/test_unregistered.cpp"}, root, {"test_unregistered"}
                )
            (root / "tests/CMakeLists.txt").write_text(
                "set(TEST_SOURCES\ntest_fixture.cpp\ntest_missing.cpp\n)\n", encoding="utf-8"
            )
            with self.assertRaisesRegex(ValueError, "source does not exist"):
                MATRIX.classify_operation(
                    {**base, "evidencePath": "tests/test_missing.cpp"}, root, {"test_missing"}
                )
            self.assertEqual(
                MATRIX.classify_operation({"toolMapping": "直接工具"}, root, {"test_fixture"}), "NOT_LINKED"
            )
            self.assertEqual(
                MATRIX.classify_operation({"toolMapping": "人机协作"}, root, set()), "NOT_LINKED"
            )
            self.assertEqual(MATRIX.classify_operation_mapping({"toolMapping": "人机协作"}), "HUMAN_HANDOFF")
            self.assertEqual(MATRIX.classify_operation_mapping({"toolMapping": "未支持"}), "UNSUPPORTED")
            self.assertEqual(MATRIX.classify_operation_mapping({"toolMapping": "通用工具组合"}), "TOOL_COMPOSITION")

    def test_duplicate_identity_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "duplicate plugin identity"):
            MATRIX._validate_unique([{"pluginId": "p"}, {"pluginId": "p"}], "pluginId", "plugin")

    def test_current_binary_prefers_build_target_over_historical_runtime_copy(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            binary = root / "build-codex-agent/lib/libDemoPlugin.so"
            binary.parent.mkdir(parents=True)
            binary.write_bytes(b"demo")
            runtime_copy = root / "build-codex-agent/runtime/plugins/Demo/libDemoPlugin.so"
            runtime_copy.parent.mkdir(parents=True)
            runtime_copy.write_bytes(b"stale-demo")
            self.assertEqual(
                MATRIX._resolve_binary_path(
                    root,
                    {
                        "cmakeTarget": "DemoPlugin",
                        "binaryPath": "build-codex-agent/runtime/plugins/Demo/libDemoPlugin.so",
                    },
                ),
                "build-codex-agent/lib/libDemoPlugin.so",
            )

    def test_stale_inventory_is_rejected_before_generation(self):
        import json

        current = json.loads(MATRIX.INVENTORY_PATH.read_text(encoding="utf-8"))
        stale = json.loads(json.dumps(current))
        stale["counts"]["builtBinariesPresentNow"] -= 1
        with patch.object(MATRIX._inventory_module, "build_inventory", return_value=stale):
            with self.assertRaisesRegex(ValueError, "candidate inventory is stale"):
                MATRIX.build_matrix(MATRIX.ROOT, MATRIX.DEFAULT_CTEST_LOG)

    def test_missing_fixture_evidence_is_rejected_by_inventory_audit(self):
        with patch.object(
            MATRIX._inventory_module,
            "build_inventory",
            side_effect=ValueError("fixture audit evidencePath does not exist"),
        ):
            with self.assertRaisesRegex(ValueError, "fixture audit evidencePath does not exist"):
                MATRIX.build_matrix(MATRIX.ROOT, MATRIX.DEFAULT_CTEST_LOG)


if __name__ == "__main__":
    unittest.main()
