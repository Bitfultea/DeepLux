#!/usr/bin/env python3
import unittest

from classify_tsan import classify


class ClassifyTsanTests(unittest.TestCase):
    def test_qt_header_template_summary_stays_unclassified(self):
        candidates, unclassified = classify(
            "SUMMARY: ThreadSanitizer: data race /usr/include/x86_64-linux-gnu/qt5/QtCore/qhash.h:291 in detach()",
            [r"SUMMARY: ThreadSanitizer: data race .*libQt[56]Core\.so(?:\.[0-9]+)*.*"],
        )
        self.assertEqual(candidates, [])
        self.assertEqual(len(unclassified), 1)

    def test_project_summary_remains_unclassified(self):
        candidates, unclassified = classify(
            "SUMMARY: ThreadSanitizer: data race src/core/engine/RunEngine.cpp:143 in operator()",
            [r"SUMMARY: ThreadSanitizer: data race .*libQt[56]Core\.so(?:\.[0-9]+)*.*"],
        )
        self.assertEqual(candidates, [])
        self.assertEqual(len(unclassified), 1)

    def test_qt_shared_library_location_is_only_a_manual_review_candidate(self):
        candidates, unclassified = classify(
            "SUMMARY: ThreadSanitizer: data race /usr/lib/libQt5Core.so.5 in QThreadPoolThread::run()",
            [r"SUMMARY: ThreadSanitizer: data race .*libQt[56]Core\.so(?:\.[0-9]+)*.*"],
        )
        self.assertEqual(len(candidates), 1)
        self.assertEqual(unclassified, [])

    def test_no_report_does_not_invent_a_pass(self):
        candidates, unclassified = classify("FATAL: ThreadSanitizer: unexpected memory mapping", [])
        self.assertEqual((candidates, unclassified), ([], []))


if __name__ == "__main__":
    unittest.main()
