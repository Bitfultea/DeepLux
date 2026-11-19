#!/usr/bin/env python3
"""Behavior tests for the CI plugin-copy verifier."""

from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from verify_ci_plugin_sync import verify_plugin_sync


class PluginSyncVerifierTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)
        self.build = self.root / "build"
        self.artifact = self.build / "lib" / "libFitCirclePlugin.so"
        self.synced = self.build / "plugins" / "FitCircle" / "libFitCirclePlugin.so"
        self.artifact.parent.mkdir(parents=True)
        self.synced.parent.mkdir(parents=True)

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def test_accepts_exact_copy(self) -> None:
        self.artifact.write_bytes(b"plugin artifact")
        self.synced.write_bytes(b"plugin artifact")
        verify_plugin_sync(self.build, "FitCirclePlugin")

    def test_rejects_different_copy(self) -> None:
        self.artifact.write_bytes(b"plugin artifact")
        self.synced.write_bytes(b"different plugin")
        with self.assertRaisesRegex(RuntimeError, "differs from every"):
            verify_plugin_sync(self.build, "FitCirclePlugin")

    def test_accepts_multiconfig_runtime_layout(self) -> None:
        self.synced.unlink(missing_ok=True)
        self.synced.parent.rmdir()
        self.synced = self.build / "bin" / "plugins" / "FitCircle" / "libFitCirclePlugin.so"
        self.synced.parent.mkdir(parents=True)
        self.artifact.write_bytes(b"plugin artifact")
        self.synced.write_bytes(b"plugin artifact")
        verify_plugin_sync(self.build, "FitCirclePlugin")


if __name__ == "__main__":
    unittest.main()
