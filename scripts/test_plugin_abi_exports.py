#!/usr/bin/env python3
import os
import sys
import tempfile
import unittest
from pathlib import Path

from check_plugin_abi_exports import is_test_hook, plugin_binaries, release_binaries


class PluginAbiExportScannerTests(unittest.TestCase):
    def test_test_hook_marker_does_not_match_for_testing_dependency_symbols(self):
        self.assertTrue(is_test_hook("setNowForTestERK9QDateTime"))
        self.assertTrue(is_test_hook("setNowForTest(QDateTime const&)"))
        self.assertFalse(is_test_hook("PeekCleanupListForTesting(void)"))

    def test_build_under_tests_parent_is_scanned(self):
        suffix = ".dll" if os.name == "nt" else ".so"
        with tempfile.TemporaryDirectory() as temporary:
            build_dir = Path(temporary) / "tests" / "project" / "build"
            plugin = build_dir / "lib" / f"ExamplePlugin{suffix}"
            core = build_dir / "lib" / f"deeplux_core{suffix}"
            app_suffix = ".exe" if os.name == "nt" else ""
            app = build_dir / "bin" / f"DeepLux{app_suffix}"
            test_app = build_dir / "bin" / f"test_probe{app_suffix}"
            plugins = [plugin]
            plugin.parent.mkdir(parents=True)
            app.parent.mkdir(parents=True)
            plugin.touch()
            core.touch()
            app.touch()
            app.chmod(0o755)
            test_app.touch()
            test_app.chmod(0o755)
            if os.name != "nt" and sys.platform != "darwin":
                versioned_plugin = build_dir / "lib" / "VersionedPlugin.so.1"
                versioned_plugin.touch()
                plugins.append(versioned_plugin)

            self.assertEqual(plugin_binaries(build_dir), sorted(plugins))
            self.assertEqual(release_binaries(build_dir), sorted([core, app, *plugins]))


if __name__ == "__main__":
    unittest.main()
