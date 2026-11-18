#!/usr/bin/env python3
"""Behavior tests for GitHub Actions plugin/profile isolation setup."""

from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from configure_ci_plugin_isolation import configure_isolation


class PluginIsolationConfigTest(unittest.TestCase):
    def run_config(self, *, is_windows: bool) -> tuple[Path, Path, str]:
        temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(temp_dir.cleanup)
        root = Path(temp_dir.name)
        env_file = root / "github-env"
        app_data = (root / f"deeplux-appdata-test" if is_windows else root / "deeplux-home-test" / ".deeplux")
        plugins = app_data / "plugins"
        plugins.mkdir(parents=True)
        (plugins / "stale.txt").write_text("stale", encoding="utf-8")

        configure_isolation("test", root, env_file, is_windows=is_windows)
        profile = root / ("deeplux-userprofile-test" if is_windows else "deeplux-home-test")
        return app_data, profile, env_file.read_text(encoding="utf-8")

    def test_linux_uses_isolated_home_and_clears_plugin_directory(self) -> None:
        app_data, profile, env = self.run_config(is_windows=False)
        self.assertIn(f"HOME={profile}\n", env)
        self.assertIn(f"DEEPLUX_APP_DATA_DIR={app_data}\n", env)
        self.assertTrue(app_data.is_dir())
        self.assertFalse((app_data / "plugins").exists())

    def test_windows_uses_isolated_userprofile_and_appdata(self) -> None:
        app_data, profile, env = self.run_config(is_windows=True)
        self.assertIn(f"USERPROFILE={profile}\n", env)
        self.assertIn(f"DEEPLUX_APP_DATA_DIR={app_data}\n", env)
        self.assertTrue(app_data.is_dir())
        self.assertFalse((app_data / "plugins").exists())


if __name__ == "__main__":
    unittest.main()
