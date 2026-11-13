#!/usr/bin/env python3

import unittest
import subprocess

from check_docs_status import ROOT, requires_status_update, validate_status


CURRENT_REV = subprocess.run(
    ["git", "rev-parse", "--short=12", "HEAD"], cwd=ROOT, check=True, capture_output=True, text=True
).stdout.strip()
VALID_STATUS = """\
- Date: 2026-11-13 (Asia/Shanghai)
- Source revision: `REVISION_PLACEHOLDER`
- Verification: `ctest` — 10/10 passed
- Evidence: [report](reports/background-execution-acceptance.md#section-13)
- OPEN-BE-01: OPEN — pending representative workload
"""
VALID_STATUS = VALID_STATUS.replace("REVISION_PLACEHOLDER", CURRENT_REV)


class DocsStatusTest(unittest.TestCase):
    def test_accepts_complete_status(self):
        self.assertEqual(validate_status(VALID_STATUS), [])

    def test_requires_all_four_snapshot_fields(self):
        errors = validate_status("- Date: today\n- OPEN-BE-01: OPEN\n")
        self.assertGreaterEqual(len(errors), 4)

    def test_rejects_placeholder_revision_result_and_evidence(self):
        invalid = VALID_STATUS.replace(f"`{CURRENT_REV}`", "TBD").replace("`ctest` — 10/10 passed", "not run")
        invalid = invalid.replace("[report](reports/background-execution-acceptance.md#section-13)", "none")
        errors = validate_status(invalid)
        self.assertTrue(any("Source revision" in error for error in errors), errors)
        self.assertTrue(any("command and an explicit result" in error for error in errors), errors)
        self.assertTrue(any("Markdown link" in error for error in errors), errors)

    def test_rejects_impossible_calendar_date(self):
        errors = validate_status(VALID_STATUS.replace("2026-11-13", "2026-99-99"))
        self.assertTrue(any("valid YYYY-MM-DD" in error for error in errors), errors)

    def test_rejects_unresolvable_git_revision(self):
        errors = validate_status(VALID_STATUS.replace(CURRENT_REV[:7], "deadbee"))
        self.assertTrue(any("resolve to a commit" in error for error in errors), errors)

    def test_validates_every_revision_and_rejects_duplicate_fields(self):
        appended_revision = VALID_STATUS.replace(f"`{CURRENT_REV}`", f"`{CURRENT_REV}` deadbee")
        errors = validate_status(appended_revision)
        self.assertTrue(any("does not resolve to a commit: deadbee" in error for error in errors), errors)

        duplicate_field = VALID_STATUS + "- Source revision: `deadbee`\n"
        errors = validate_status(duplicate_field)
        self.assertTrue(any("must appear exactly once" in error for error in errors), errors)

    def test_requires_explicit_open_item_state(self):
        errors = validate_status(VALID_STATUS.replace("OPEN-BE-01: OPEN", "OPEN-BE-01: pending"))
        self.assertTrue(any("OPEN or CLOSED" in error for error in errors), errors)

    def test_requires_status_for_implementation_changes(self):
        self.assertTrue(requires_status_update(["src/core/engine/RunEngine.cpp"]))
        self.assertTrue(requires_status_update(["tools/sam_server/sam_server.py"]))
        self.assertTrue(requires_status_update([".github/workflows/ci.yml"]))
        self.assertTrue(requires_status_update(["CMakeLists.txt"]))
        self.assertFalse(requires_status_update(["docs/plugins.md"]))


if __name__ == "__main__":
    unittest.main()
