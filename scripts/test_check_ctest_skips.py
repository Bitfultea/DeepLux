#!/usr/bin/env python3
import unittest

from check_ctest_skips import check


class CheckCtestSkipsTests(unittest.TestCase):
    def test_no_skips_passes(self):
        errors, count = check("PASS   : Test::case()", {})
        self.assertEqual((errors, count), ([], 0))

    def test_allowlisted_reason_passes(self):
        allowlist = {"Test::case": {"reason_contains": ["requires GPU"]}}
        errors, count = check("SKIP   : Test::case() requires GPU", allowlist)
        self.assertEqual((errors, count), ([], 1))

    def test_unknown_skip_fails(self):
        errors, count = check("SKIP   : Test::case() unavailable", {})
        self.assertEqual(count, 1)
        self.assertEqual(len(errors), 1)

    def test_changed_reason_fails(self):
        allowlist = {"Test::case": {"reason_contains": ["requires GPU"]}}
        errors, _ = check("SKIP   : Test::case() timeout", allowlist)
        self.assertEqual(len(errors), 1)


if __name__ == "__main__":
    unittest.main()
