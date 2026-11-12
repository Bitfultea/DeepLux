import importlib.util
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("audit_agent_g8_plugin_coverage.py")
SPEC = importlib.util.spec_from_file_location("audit_agent_g8_plugin_coverage", SCRIPT)
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)


class TestAgentG8PluginCoverageInventory(unittest.TestCase):
    def test_ctest_parser_requires_complete_success_summary(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "ctest.log"
            path.write_text(
                "1/1 Test #1: test_fixture ........ Passed 0.01 sec\n"
                "100% tests passed, 0 tests failed out of 1\n",
                encoding="utf-8",
            )
            self.assertEqual(AUDIT.full_ctest_summary(path), (1, 1, 0, 0))
            path.write_text("0% tests passed, 1 tests failed out of 1\n", encoding="utf-8")
            self.assertIsNone(AUDIT.full_ctest_summary(path))

    def test_ctest_parser_rejects_failed_or_conflicting_duplicate_rows(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "ctest.log"
            path.write_text(
                "1/1 Test #1: test_fixture ........ Failed 0.01 sec\n"
                "1/1 Test #1: test_fixture ........ Passed 0.01 sec\n"
                "100% tests passed, 0 tests failed out of 1\n",
                encoding="utf-8",
            )
            self.assertIsNone(AUDIT.full_ctest_summary(path))

    def test_ctest_parser_rejects_non_contiguous_or_zero_based_numbering(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "ctest.log"
            path.write_text(
                "0/2 Test #0: test_zero ........ Passed 0.01 sec\n"
                "2/2 Test #2: test_two ........ Passed 0.01 sec\n"
                "100% tests passed, 0 tests failed out of 2\n",
                encoding="utf-8",
            )
            self.assertIsNone(AUDIT.full_ctest_summary(path))

    def test_ctest_parser_accepts_parallel_completion_order(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "ctest-parallel.log"
            path.write_text(
                "1/3 Test #2: test_two ........ Passed 0.01 sec\n"
                "2/3 Test #3: test_three ...... Passed 0.01 sec\n"
                "3/3 Test #1: test_one ........ Passed 0.01 sec\n"
                "100% tests passed, 0 tests failed out of 3\n",
                encoding="utf-8",
            )
            self.assertEqual(AUDIT.full_ctest_summary(path), (3, 3, 0, 0))

    def test_plugin_source_match_requires_exact_name_or_plugin_reference(self):
        plugin = {
            "dirName": "FitCircle",
            "sourcePath": "src/plugins/geometry/FitCircle",
            "cmakeTarget": "FitCirclePlugin",
        }
        self.assertEqual(AUDIT.source_mentions_plugin('#include "plugins/geometry/FitCircle/FitCirclePlugin.h"', plugin),
                         ["direct_plugin_header"])
        self.assertEqual(AUDIT.source_mentions_plugin('auto target = FitCirclePlugin();', plugin),
                         ["direct_plugin_symbol"])
        self.assertEqual(AUDIT.source_mentions_plugin('testFitCircleOutput();', plugin), ["exact_name_token"])
        self.assertEqual(AUDIT.source_mentions_plugin('testFitCirclesOutput();', plugin), [])

    def test_qt_test_case_name_extractor_lists_slots_and_definitions(self):
        text = """
        private slots:
            void testValidFlow();
            void testInlineFlow() {}
        };
        void TestExample::testValidFlow() {}
        void TestExample::testMissingInputFails() {}
        """
        self.assertEqual(
            AUDIT.qt_test_case_names(text),
            ["testInlineFlow", "testMissingInputFails", "testValidFlow"],
        )

    def test_qt_test_case_name_extractor_does_not_stop_at_lambda_closure(self):
        text = """
        private slots:
            void testBeforeLambda() {}
            void testLambdaBody() {
                auto configure = []() {
                    return true;
                };
            }
            void testAfterLambda() {}
        };
        """
        self.assertEqual(
            AUDIT.qt_test_case_names(text),
            ["testAfterLambda", "testBeforeLambda", "testLambdaBody"],
        )

    def test_qt_test_case_name_extractor_finds_real_inline_suite(self):
        source = AUDIT.ROOT / "tests/test_batch4.cpp"
        self.assertIn("testCropAxisAligned", AUDIT.qt_test_case_names(source.read_text(encoding="utf-8")))

    def test_qt_test_case_name_extractor_includes_non_test_prefixed_slots(self):
        interaction = (AUDIT.ROOT / "tests/test_3d_interaction.cpp").read_text(encoding="utf-8")
        draft_restore = (AUDIT.ROOT / "tests/test_agent_draft_restore.cpp").read_text(encoding="utf-8")
        self.assertIn("interactionModeKeepsCoarseLodUntilReleased", AUDIT.qt_test_case_names(interaction))
        self.assertIn("rejectedSendRestoresDraft", AUDIT.qt_test_case_names(draft_restore))
        self.assertNotIn("initTestCase", AUDIT.qt_test_case_names(draft_restore))

    def test_test_source_inventory_includes_conditional_linux_suite(self):
        sources = AUDIT.test_sources_from_cmake(AUDIT.ROOT / "tests/CMakeLists.txt")
        self.assertIn("test_v4l2discovery.cpp", sources)
        self.assertIn("test_agent_plugin_g5.cpp", sources)
        self.assertIn("test_agent_variable_plugins_g8.cpp", sources)
        self.assertEqual(len(sources), len(set(sources)))

    def test_source_metadata_identity_set_matches_baseline(self):
        import json

        baseline = json.loads(AUDIT.BASELINE_PATH.read_text(encoding="utf-8"))
        expected = {(item["dirName"], item["pluginId"]) for item in baseline["plugins"]}
        self.assertEqual(AUDIT.source_inventory(), expected)

    def test_default_ctest_log_matches_generated_inventory(self):
        import json

        inventory = json.loads(AUDIT.JSON_PATH.read_text(encoding="utf-8"))
        self.assertEqual(AUDIT.DEFAULT_CTEST_LOG, AUDIT.ROOT / inventory["fullCtestLog"])

    def test_fixture_test_id_validation_rejects_unpassed_suite_and_missing_case(self):
        import json

        baseline = json.loads(AUDIT.BASELINE_PATH.read_text(encoding="utf-8"))
        plugin = next(item for item in baseline["plugins"] if item["dirName"] == "GrabImage")
        source = AUDIT.ROOT / "tests/test_agent_camera_lifecycle_g5.cpp"
        text = source.read_text(encoding="utf-8")
        source_texts = [(source, text, AUDIT.qt_test_case_names(text))]
        valid_id = "test_agent_camera_lifecycle_g5::testAgentAsyncSessionFeedsGrabImage"
        with self.assertRaisesRegex(ValueError, "not passing in CTest"):
            AUDIT.validate_fixture_test_id(valid_id, ("GrabImage", plugin["pluginId"]), plugin, set(), source_texts)
        with self.assertRaisesRegex(ValueError, "does not resolve to a matching test source"):
            AUDIT.validate_fixture_test_id(
                "test_agent_camera_lifecycle_g5::testMissingCameraFixture",
                ("GrabImage", plugin["pluginId"]),
                plugin,
                {"test_agent_camera_lifecycle_g5"},
                source_texts,
            )

    def test_reviewed_fixture_audits_are_explicit_and_resolve_to_ctest_cases(self):
        import json

        inventory = json.loads(AUDIT.JSON_PATH.read_text(encoding="utf-8"))
        audits = json.loads(AUDIT.FIXTURE_AUDITS_PATH.read_text(encoding="utf-8"))
        audited = {item["dirName"]: item for item in inventory["plugins"] if item["fixtureAuditTestId"]}
        self.assertEqual(
            audited["GrabImage"]["fixtureAuditSupportingTestIds"],
            ["test_agent_camera_lifecycle_g5::testAgentAsyncSessionFeedsGrabImage"],
        )
        self.assertEqual(
            set(audited),
            {
                "VarDefinePlugin",
                "VarSetPlugin",
                "QRCode",
                "ShowImage",
                "PLCReadPlugin",
                "MathPlugin",
                "CreateString",
                "SplitString",
                "StrFormatPlugin",
                "SystemTime",
                "WriteText",
                "TimeSlice",
                "Condition",
                "Delay",
                "DataCheck",
                "TableOutPut",
                "SaveData",
                "SaveImage",
                "DisplayData",
                "ColorRecognition",
                "GrabImage",
                "Blob",
                "FindCircle",
                "CropImage",
                "MeasureCircle",
                "MeasureLine",
                "MeasureRect",
                "LinesDistance",
                "MeasureGap",
                "PointSurfaceDistance",
                "NPointCalibration",
                "PLCCommunicatePlugin",
                "PLCWritePlugin",
                "TCPClient",
                "TCPServer",
                "SerialPort",
                "Folder",
                "If",
                "Loop",
                "ShowPoint",
                "RegionStats",
                "Parallel",
                "Matching",
                "SamPredict",
                "EdgeDefectDetection",
                "JiErHanDefectsDet",
                "ImageScript",
                "FreeformSurface",
                "LoadPointCloud",
                "JigsawPuzzle",
                "QueueOut",
                "QueueIn",
                "GapMeasure3D",
                "PerProcessing",
                "PreProcessing3D",
                "While",
                "StopWhile",
                "V4L2Camera",
                "DefectDetection",
                "MeasurementInput",
                "DistancePP",
                "DistancePL",
                "FitCircle",
                "FitLine",
                "FitEllipse",
                "CalculateOffset",
                "FitPlane",
            },
        )
        self.assertTrue(all(audit["evidencePath"] == inventory["fullCtestLog"] for audit in audits["audits"]))
        for plugin in audited.values():
            expected_business_status = "FAIL" if plugin["dirName"] == "DefectDetection" else "PASS"
            self.assertEqual(plugin["validBusinessFixtureStatus"], expected_business_status)
            self.assertEqual(plugin["criticalFailureFixtureStatus"], "PASS")
            self.assertEqual(plugin["fixtureAuditReviewStatus"], "PASS")
            expected_suite, expected_case = {
                "VarDefinePlugin": ("test_agent_variable_plugins_g8", "testLocalizedPluginNamesConfigureRunAndReportBusinessResults"),
                "VarSetPlugin": ("test_agent_variable_plugins_g8", "testLocalizedPluginNamesConfigureRunAndReportBusinessResults"),
                "QRCode": ("test_agent_visual_plugins_g8", "testAgentLoadsDecodesAndDisplaysRealImageOutput"),
                "ShowImage": ("test_agent_visual_plugins_g8", "testAgentLoadsDecodesAndDisplaysRealImageOutput"),
                "PLCReadPlugin": ("test_agent_core_plugins_g8", "testAgentConfigurationReadsModbusAndReportsProtocolFailure"),
                "MathPlugin": ("test_agent_core_plugins_g8", "testAgentConfigurationCalculatesAndRejectsDivisionByZero"),
                "CreateString": (
                    "test_agent_core_plugins_g8",
                    "testAgentConfigurationCreatesStringAndRejectsEmptyOutputName",
                ),
                "SplitString": (
                    "test_agent_core_plugins_g8",
                    "testAgentConfigurationSplitsStringAndReportsEmptyInput",
                ),
                "StrFormatPlugin": (
                    "test_agent_core_plugins_g8",
                    "testAgentConfigurationFormatsStringAndRejectsEmptyOutputName",
                ),
                "SystemTime": (
                    "test_agent_core_plugins_g8",
                    "testAgentConfigurationReadsSystemTimeAndRejectsEmptyFormat",
                ),
                "WriteText": (
                    "test_agent_core_plugins_g8",
                    "testAgentConfiguresWriteTextAndPropagatesOpenFailure",
                ),
                "TimeSlice": (
                    "test_agent_core_plugins_g8",
                    "testAgentRunsTimeSliceAndRejectsStopWithoutStartInput",
                ),
                "Condition": (
                    "test_agent_core_plugins_g8",
                    "testAgentEvaluatesConditionFromUpstreamAndReportsMissingVariable",
                ),
                "Delay": (
                    "test_agent_core_plugins_g8",
                    "testAgentRunsConfiguredDelayAndRejectsZeroDuration",
                ),
                "DataCheck": (
                    "test_agent_core_plugins_g8",
                    "testAgentChecksUpstreamValueAndReportsMissingCheckData",
                ),
                "TableOutPut": (
                    "test_agent_core_plugins_g8",
                    "testAgentConfiguresTableOutputAndRejectsInvalidDimensions",
                ),
                "SaveData": (
                    "test_agent_core_plugins_g8",
                    "testAgentConfiguresSaveDataAndReportsWriteFailure",
                ),
                "SaveImage": (
                    "test_agent_visual_plugins_g8",
                    "testAgentConfirmsSaveImageAndReportsEncoderFailure",
                ),
                "DisplayData": (
                    "test_agent_visual_plugins_g8",
                    "testAgentOverlaysDisplayDataAndAttributesMissingImageFailure",
                ),
                "ColorRecognition": (
                    "test_agent_visual_plugins_g8",
                    "testAgentRecognizesConfiguredColorAndReportsAbsentTarget",
                ),
                "GrabImage": (
                    "test_agent_visual_plugins_g8",
                    "testAgentLoadsGrabImagePathDemoAndFolderModes",
                ),
                "Folder": (
                    "test_agent_folder_execution_g5",
                    "testFolderDeleteRequiresConfirmationForRunFlowAndStepOnce",
                ),
                "If": (
                    "test_acceptance_flows",
                    "testAgentRunFlowPreservesIfBranchSemantics",
                ),
                "Loop": (
                    "test_acceptance_flows",
                    "testAgentRunFlowPreservesLoopCount",
                ),
                "ShowPoint": (
                    "test_acceptance_flows",
                    "testAgentShowPointOutputsDeclareCurrentImagePixelUnits",
                ),
                "RegionStats": (
                    "test_sam_region_flow",
                    "testAgentRegionStatsCentroidUnknownProducerHasNoSourceMapping",
                ),
                "Parallel": (
                    "test_acceptance_flows",
                    "testAgentRunFlowPreservesParallelAllJoin",
                ),
                "Matching": (
                    "test_acceptance_flows",
                    "testAgentMatchingScalarsDeclareLocalPixelUnits",
                ),
                "SamPredict": (
                    "test_sam_region_flow",
                    "testAgentSamPredictRegionCoordinatesAfterNonzeroCrop",
                ),
                "EdgeDefectDetection": (
                    "test_acceptance_flows",
                    "testAgentEdgeDefectDeviationUnitsRequireImageLocalPointSetSource",
                ),
                "JiErHanDefectsDet": (
                    "test_acceptance_flows",
                    "testAgentJiErHanDefectRegionMapsThroughCropOrigin",
                ),
                "ImageScript": (
                    "test_agent_visual_plugins_g8",
                    "testAgentImageScriptInvertAndRejectsInvalidConfigurationAndMissingImage",
                ),
                "FreeformSurface": (
                    "test_agent_visual_plugins_g8",
                    "testAgentLoadPointCloudFeedsFreeformSurfaceAndRejectsLegacyParameter",
                ),
                "LoadPointCloud": (
                    "test_agent_visual_plugins_g8",
                    "testAgentLoadPointCloudFeedsFreeformSurfaceAndRejectsLegacyParameter",
                ),
                "JigsawPuzzle": (
                    "test_agent_visual_plugins_g8",
                    "testAgentJigsawPuzzleSplitsAndReassemblesGridAndRejectsUndersizedInput",
                ),
                "QueueOut": (
                    "test_agent_visual_plugins_g8",
                    "testAgentQueueOutEmptyResultUsesConfiguredOutputAndReportsZeroSize",
                ),
                "QueueIn": (
                    "test_agent_visual_plugins_g8",
                    "testAgentQueueInRoundTripsConfiguredStringAndRejectsMissingData",
                ),
                "GapMeasure3D": (
                    "test_agent_visual_plugins_g8",
                    "testAgentMeasuresGapFromHeightImageAndReportsNotFoundAndInvalidRoi",
                ),
                "PerProcessing": (
                    "test_agent_visual_plugins_g8",
                    "testAgentPerProcessingGaussianBlurAndRejectsUnsupportedTypeAndMissingImage",
                ),
                "PreProcessing3D": (
                    "test_agent_visual_plugins_g8",
                    "testAgentPreProcessing3DReportsHeightMapAndRejectsColorInput",
                ),
                "While": (
                    "test_agent_visual_plugins_g8",
                    "testAgentWhileFalseConditionAndRejectsInvalidComparisonAndLimit",
                ),
                "StopWhile": (
                    "test_agent_visual_plugins_g8",
                    "testAgentStopWhileExitsWhileBodyAndContinuesAfterFlow",
                ),
                "V4L2Camera": (
                    "test_v4l2discovery",
                    "testAgentListsFakeV4L2DiscoveryAndReleasesSkippedDeviceFd",
                ),
                "DefectDetection": (
                    "test_agent_core_plugins_g8",
                    "testAgentReportsHymsonSdkUnavailableForDefectDetection",
                ),
                "Blob": (
                    "test_agent_visual_plugins_g8",
                    "testAgentDetectsConfiguredBlobAndReportsEmptyDetection",
                ),
                "FindCircle": (
                    "test_agent_visual_plugins_g8",
                    "testAgentFindsConfiguredCircleAndReportsNoCircle",
                ),
                "CropImage": (
                    "test_agent_visual_plugins_g8",
                    "testAgentCropsMultipleRectanglesAndReportsNoIntersection",
                ),
                "MeasureCircle": (
                    "test_agent_visual_plugins_g8",
                    "testAgentMeasuresConfiguredCircleAndReportsInsufficientEdges",
                ),
                "MeasureLine": (
                    "test_agent_visual_plugins_g8",
                    "testAgentMeasuresConfiguredLineAndReportsNoLine",
                ),
                "MeasureRect": (
                    "test_agent_visual_plugins_g8",
                    "testAgentMeasuresRectangleAndReportsNoContour",
                ),
                "LinesDistance": (
                    "test_agent_visual_plugins_g8",
                    "testAgentMeasuresFiniteLineSegmentsAndRequiresBothInputs",
                ),
                "MeasureGap": (
                    "test_agent_visual_plugins_g8",
                    "testAgentMeasures2DGapAndRequiresSecondPoint",
                ),
                "PointSurfaceDistance": (
                    "test_agent_visual_plugins_g8",
                    "testAgentMeasuresWorldPointToPlaneAndRequiresPlane",
                ),
                "NPointCalibration": (
                    "test_agentphase0_repro",
                    "testG3NPointCalibrationOutputPortsByRunId",
                ),
                "PLCCommunicatePlugin": (
                    "test_agent_plccommunicate_execution_g5",
                    "testStepOnceExpiresDriftRejectsAndConfirmsSingleFC03",
                ),
                "PLCWritePlugin": (
                    "test_agent_plcwrite_execution_g5",
                    "testPLCWriteRequiresConfirmationForRunFlowAndStepOnce",
                ),
                "TCPClient": (
                    "test_agent_tcpclient_execution_g5",
                    "testRunFlowBindsEndpointExpiresOnPortChangeAndReadsResponse",
                ),
                "TCPServer": (
                    "test_agent_tcpserver_execution_g5",
                    "testRunFlowBindsLocalEndpointExpiresOnPortChangeAndReadsRequest",
                ),
                "SerialPort": (
                    "test_agent_serialport_execution_g5",
                    "testRunFlowRejectsAndExpiresSerialTargetThenReadsPTYReply",
                ),
                "MeasurementInput": (
                    "test_agent_visual_plugins_g8",
                    "testAgentSuppliesPointPairAndReportsDimensionMismatch",
                ),
                "DistancePP": (
                    "test_agent_visual_plugins_g8",
                    "testAgentMeasuresPointPairAndValidatesRequiredDistanceInput",
                ),
                "DistancePL": (
                    "test_agent_visual_plugins_g8",
                    "testAgentMeasuresPointToLineAndValidatesRequiredLineInput",
                ),
                "FitCircle": (
                    "test_agent_visual_plugins_g8",
                    "testAgentFitsPointSetAndReportsRadiusConstraintFailure",
                ),
                "FitLine": (
                    "test_agent_visual_plugins_g8",
                    "testAgentFitsPointSetAndRejectsDegenerateLine",
                ),
                "FitEllipse": (
                    "test_agent_visual_plugins_g8",
                    "testAgentFitsEllipseAndReportsAxisRangeFailure",
                ),
                "CalculateOffset": (
                    "test_agent_visual_plugins_g8",
                    "testAgentCalculatesOffsetFromPointPortAndRejectsInvalidPoint3D",
                ),
                "FitPlane": (
                    "test_agent_visual_plugins_g8",
                    "testAgentFitsHeightMapAndReportsNoValidPixels",
                ),
                "MeasureLine": (
                    "test_agent_visual_plugins_g8",
                    "testAgentMeasuresConfiguredLineAndReportsNoLine",
                ),
            }[plugin["dirName"]]
            self.assertTrue(
                any(
                    candidate["ctestSuite"] == expected_suite
                    and candidate["suiteResult"] == "PASS"
                    and expected_case in candidate["sourceQtTestCaseNames"]
                    for candidate in plugin["testCandidates"]
                )
            )
        self.assertTrue(audited["DefectDetection"]["binaryPresentNow"])


if __name__ == "__main__":
    unittest.main()
