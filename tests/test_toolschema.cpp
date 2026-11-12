#include "core/agent/ToolSchema.h"

#include <QtTest/QtTest>

using namespace DeepLux;

class TestToolSchema : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        ToolSchema::instance().registerDefaultTools();
    }

    void testHasTool() {
        QVERIFY(ToolSchema::instance().hasTool("create_project"));
        QVERIFY(ToolSchema::instance().hasTool("add_module"));
        QVERIFY(ToolSchema::instance().hasTool("run_flow"));
        QVERIFY(ToolSchema::instance().hasTool("step_once"));
        QVERIFY(ToolSchema::instance().hasTool("reset_step"));
        QVERIFY(ToolSchema::instance().hasTool("validate_flow"));
        QVERIFY(!ToolSchema::instance().hasTool("nonexistent"));
    }

    void testModuleOutputCoordinateGuidancePreservesToolSemantics() {
        const ToolDefinition tool = ToolSchema::instance().findTool(QStringLiteral("get_module_output"));
        QCOMPARE(tool.name, QStringLiteral("get_module_output"));
        const QJsonObject function = tool.toOpenAIFunction().value(QStringLiteral("function")).toObject();
        QCOMPARE(function.value(QStringLiteral("name")).toString(), QStringLiteral("get_module_output"));
        const QString description = function.value(QStringLiteral("description")).toString();
        const QStringList requiredFragments{
            QStringLiteral("coordinateSystem=image_pixels"),
            QStringLiteral("sourceCoordinates.localToSource"),
            QStringLiteral("row-major 3x3 matrix M"),
            QStringLiteral("integer (x,y) as pixel centers"),
            QStringLiteral("[u,v,w]^T = M * [x,y,1]^T"),
            QStringLiteral("x'=u/w=(m00*x+m01*y+m02)/w"),
            QStringLiteral("y'=v/w=(m10*x+m11*y+m12)/w"),
            QStringLiteral("w=m20*x+m21*y+m22"),
            QStringLiteral("16 * double epsilon"),
            QStringLiteral("do not claim source-image coordinates"),
            QStringLiteral("lastCompletedRunId"),
            QStringLiteral("select the exact output namespace"),
            QStringLiteral("arrays are paginated"),
            QStringLiteral("maskOffset chunks through maskExport"),
            QStringLiteral("polygonOffset/polygonLimit"),
            QStringLiteral("the whole requested page maps"),
            QStringLiteral("PointCloudData summaries and previews use coordinateSystem=unspecified"),
            QStringLiteral("do not infer units, world-frame transforms, or source-image mappings"),
            QStringLiteral("For FreeformSurface surface_roughness and surface_area"),
            QStringLiteral("Their physical dimensions are length and length squared only when all three axes use a "
                           "common physical unit/scale"),
            QStringLiteral("PointCloudData does not declare units and LoadPointCloud can scale axes independently"),
            QStringLiteral("do not infer mm, mm^2, or a world frame"),
            QStringLiteral("CalculateOffset offset_x and offset_y are raw current-minus-target numeric differences"),
            QStringLiteral(
                "only compare or interpret them physically when both coordinates use the same frame and unit"),
            QStringLiteral("The plugin does not transform frames or units"),
            QStringLiteral("current_point overrides only X/Y and ignores Z"),
            QStringLiteral("current_angle, when present, overrides the parameter angle"),
            QStringLiteral("offset_a is the degree difference normalized to (-180,180]"),
            QStringLiteral("height_map_local_scaled_xyz"),
            QStringLiteral("X=column*pixelSizeX, Y=row*pixelSizeY, Z=height*zScale"),
            QStringLiteral("unitBasis lists the scaling parameter names, not units or values"),
            QStringLiteral("units are unknown; do not infer mm or a world frame"),
            QStringLiteral("This guidance applies only to FitPlane port plane"),
            QStringLiteral("FitPlane scalar outputs plane_d, flatness, max_deviation, min_deviation, and rms"),
            QStringLiteral("pixelSizeX/pixelSizeY/zScale-scaled coordinates"),
            QStringLiteral("do not declare units or establish a shared physical scale"),
            QStringLiteral("external confirmation that all axes use a consistent engineering unit"),
            QStringLiteral("plane_nx/ny/nz components are the dimensionless unit-normal components of that plane "
                           "in those scaled XYZ axes, oriented toward +Z by the FitPlane algorithm"),
            QStringLiteral("they do not declare a physical unit or external world reference"),
            QStringLiteral("This guidance applies only to those five scalar outputs"),
            QStringLiteral("For 3DPreProcessing min_height and max_height, values preserve the numeric scale of "
                           "valid input height pixels"),
            QStringLiteral("the plugin performs no physical unit conversion"),
            QStringLiteral("input-height units and an external Z reference are not declared"),
            QStringLiteral("does not apply to valid_pixel_count, filtered_pixel_count, or height_invalid_value"),
            QStringLiteral("MeasurementInput ports point and plane"),
            QStringLiteral("measurement_input_mode=point_plane and coordinate_frame=World3D"),
            QStringLiteral("type is Point3D or Plane3D and coordinateFrame repeats the plugin frame label"),
            QStringLiteral("A Point3D value is three XYZ numbers; a Plane3D value is three XYZ points in order"),
            QStringLiteral("units, axis definition, and external reference are unknown"),
            QStringLiteral("Do not infer physical units or add image-source coordinates"),
            QStringLiteral("not evidence that the values came from a point cloud"),
            QStringLiteral("For PointSurfaceDistance port foot_point, Point3D/coordinateFrame=World3D"),
            QStringLiteral("the same run carries coordinate_frame=World3D and the computed foot is finite"),
            QStringLiteral("The frame label is inherited from the 3D input"),
            QStringLiteral("Do not map this 3D point through image provenance"),
            QStringLiteral("For PointSurfaceDistance keys foot_x, foot_y, and foot_z"),
            QStringLiteral("coordinateFrame=World3D is included only when each queried finite numeric key matches "
                           "its component in the same output's valid foot_point"),
            QStringLiteral("legacy key remains a raw value without a frame claim"),
            QStringLiteral("PointSurfaceDistance distance is the raw perpendicular distance from input XYZ values"),
            QStringLiteral("the plugin does not convert units"),
            QStringLiteral("coordinate_frame=World3D labels only the input frame"),
            QStringLiteral("does not declare units, axis definitions, or an external reference"),
            QStringLiteral("Do not infer mm or other physical units. This guidance applies only to distance, not "
                           "foot_x/y/z or foot_point"),
            QStringLiteral("For EdgeDefectDetection defect_regions, columnUnits marks only max_deviation and "
                           "mean_deviation as px"),
            QStringLiteral("image/reference_edge are connected to image/fit_points from one MeasurementInput "
                           "instance"),
            QStringLiteral("The table has no single unit; angle, index, count, and polarity columns do not inherit "
                           "px"),
            QStringLiteral("For FitLine keys line_row1, line_col1, line_row2, and line_col2, unit=px is reported "
                           "only when the same ImageLocalPixel run has a valid finite Line2D"),
            QStringLiteral("row is y and col is x in the current image-local pixel grid"),
            QStringLiteral("this guidance does not assign units or redefine line_phi/line_rho"),
            QStringLiteral("For LinesDistance nearest_x1/y1/x2/y2, px requires same-source MeasurementInput "
                           "image/line1/line2, line_pair, ImageLocalPixel"),
            QStringLiteral("an exact match to the same run's nearest_segment endpoint"),
            QStringLiteral("The scalar has no source coordinate mapping; distance remains unlabelled"),
            QStringLiteral(
                "For MeasureGap gap, gap_distance, gap_delta_x, and gap_delta_y, px requires a same-run "
                "2D point_pair from same-source MeasurementInput point1/point2 with matching finite outputs"),
            QStringLiteral("Three-element points remain unlabelled even when z=0; gap_delta_z is unlabelled"),
            QStringLiteral("ImageLocalPixel alone does not prove the source or output correspondence"),
            QStringLiteral("GapMeasure3D gap_width, gap_offset_width, and corner_dx_mm use mm when gap_found is true"),
            QStringLiteral("query gap_found first because otherwise these values are the measureFailValue sentinel"),
            QStringLiteral("legacy keys corner_dz_mm and corner_dist_mm do not guarantee mm"),
            QStringLiteral("their physical units are unknown because zScale and input-height units are unspecified"),
            QStringLiteral("legacy numeric hypot(corner_dx_mm, corner_dz_mm)"),
            QStringLiteral("only when X and Z are calibrated in the same physical unit"),
            QStringLiteral("optional bounded preview"),
            QStringLiteral("STALE_OUTPUT"),
            QStringLiteral("uncertain with retryable=false; do not repeat it.")};
        for (const QString& fragment : requiredFragments)
            QVERIFY2(description.contains(fragment),
                     qPrintable(QStringLiteral("Missing description fragment: %1").arg(fragment)));
        const auto stringParameter = [](const QString& description) {
            return QJsonObject{{"type", QStringLiteral("string")}, {"description", description}};
        };
        const auto booleanParameter = [](const QString& description) {
            return QJsonObject{{"type", QStringLiteral("boolean")}, {"description", description}};
        };
        const auto integerParameter = [](const QString& description) {
            return QJsonObject{{"type", QStringLiteral("integer")}, {"description", description}};
        };
        const QJsonObject expectedParameters{
            {"type", QStringLiteral("object")},
            {"properties",
             QJsonObject{
                 {"runId", stringParameter(QStringLiteral("Completed run ID; cycle mode uses lastCompletedRunId"))},
                 {"instanceId", stringParameter(QStringLiteral("Module instance ID"))},
                 {"port", stringParameter(QStringLiteral("Output port ID (mutually exclusive with key)"))},
                 {"key", stringParameter(QStringLiteral("Named data key (omit for summary)"))},
                 {"preview",
                  booleanParameter(QStringLiteral("Include a bounded image, region, or point-cloud preview"))},
                 {"offset", integerParameter(QStringLiteral("Zero-based row offset for array results (default 0)"))},
                 {"limit", integerParameter(QStringLiteral("Rows per page, 1..1000 (default 100)"))},
                 {"maskOffset",
                  integerParameter(QStringLiteral("Zero-based RLE character offset for a region mask chunk"))},
                 {"polygonOffset",
                  integerParameter(QStringLiteral("Zero-based vertex offset for a Region2D polygon page (default 0)"))},
                 {"polygonLimit",
                  integerParameter(QStringLiteral("Vertices per Region2D polygon page, 1..1000 (default 100)"))}}},
            {"required", QJsonArray{QStringLiteral("runId"), QStringLiteral("instanceId")}}};
        QCOMPARE(function.value(QStringLiteral("parameters")).toObject(), expectedParameters);
    }

    void testCalibrationParamsSchemaUsesPluginCoordinateDescriptions() {
        const ToolDefinition tool = ToolSchema::instance().findTool(QStringLiteral("get_module_params_schema"));
        const QJsonObject function = tool.toOpenAIFunction().value(QStringLiteral("function")).toObject();
        const QString description = function.value(QStringLiteral("description")).toString();
        QVERIFY(
            description.contains(QStringLiteral("Use nested parameter descriptions to interpret coordinate fields")));
        QVERIFY(description.contains(QStringLiteral("image points use current-image local pixels")));
        QVERIFY(description.contains(
            QStringLiteral("world points and reprojection_error retain the caller-defined target-plane scale")));
        QVERIFY(description.contains(
            QStringLiteral("Do not infer millimeters, a fixed world reference, or image-source provenance")));
    }

    void testOpenSystemSettingsIsReadOnlyGuiNavigation() {
        const ToolDefinition tool = ToolSchema::instance().findTool(QStringLiteral("open_system_settings"));
        QCOMPARE(tool.name, QStringLiteral("open_system_settings"));
        QVERIFY(!tool.dangerous);
        QVERIFY(tool.params.isEmpty());
        QVERIFY(tool.description.contains(QStringLiteral("user must review")));
        QVERIFY(tool.description.contains(QStringLiteral("does not mean anything was saved")));
    }

    void testOpenSamEnvironmentSetupIsReadOnlyGuiNavigation() {
        const ToolDefinition tool = ToolSchema::instance().findTool(QStringLiteral("open_sam_environment_setup"));
        QCOMPARE(tool.name, QStringLiteral("open_sam_environment_setup"));
        QVERIFY(!tool.dangerous);
        QVERIFY(tool.params.isEmpty());
        QVERIFY(tool.description.contains(QStringLiteral("does not initialize")));
        QVERIFY(tool.description.contains(QStringLiteral("start the server")));
    }

    void testOpenSamAnnotatorDialogIsSafeGuiHandoff() {
        const ToolDefinition tool = ToolSchema::instance().findTool(QStringLiteral("open_sam_annotator_dialog"));
        QCOMPARE(tool.name, QStringLiteral("open_sam_annotator_dialog"));
        QVERIFY(!tool.dangerous);
        QVERIFY(tool.params.isEmpty());
        QVERIFY(tool.description.contains(QStringLiteral("does not prepare the backend")));
        QVERIFY(tool.description.contains(QStringLiteral("existing annotation image/session is preserved")));
    }

    void testOpenSamImagePickerIsUserDrivenAndDoesNotSelectImage() {
        const ToolDefinition tool = ToolSchema::instance().findTool(QStringLiteral("open_sam_image_picker_for_user"));
        QCOMPARE(tool.name, QStringLiteral("open_sam_image_picker_for_user"));
        QVERIFY(!tool.dangerous);
        QVERIFY(tool.params.isEmpty());
        QVERIFY(tool.description.contains(QStringLiteral("choose a file manually")));
        QVERIFY(tool.description.contains(QStringLiteral("does not select or read an image")));
    }

    void testOpenUserLoginDialogIsManualCredentialHandoff() {
        const ToolDefinition tool = ToolSchema::instance().findTool(QStringLiteral("open_user_login_dialog"));
        QCOMPARE(tool.name, QStringLiteral("open_user_login_dialog"));
        QVERIFY(!tool.dangerous);
        QVERIFY(tool.params.isEmpty());
        QVERIFY(tool.description.contains(QStringLiteral("enter and submit credentials manually")));
        QVERIFY(tool.description.contains(QStringLiteral("does not mean")));
    }

    void testOpenAgentSettingsDialogIsManualAndDoesNotExposeValues() {
        const ToolDefinition tool = ToolSchema::instance().findTool(QStringLiteral("open_agent_settings_dialog"));
        QCOMPARE(tool.name, QStringLiteral("open_agent_settings_dialog"));
        QVERIFY(!tool.dangerous);
        QVERIFY(tool.params.isEmpty());
        QVERIFY(tool.description.contains(QStringLiteral("OK or Cancel manually")));
        QVERIFY(tool.description.contains(QStringLiteral("does not save settings")));
        QVERIFY(tool.description.contains(QStringLiteral("does not read or return the API key")));
    }

    void testOpenAboutDialogIsReadOnlyNavigation() {
        const ToolDefinition tool = ToolSchema::instance().findTool(QStringLiteral("open_about_dialog"));
        QCOMPARE(tool.name, QStringLiteral("open_about_dialog"));
        QVERIFY(!tool.dangerous);
        QVERIFY(tool.params.isEmpty());
        QVERIFY(tool.description.contains(QStringLiteral("does not check licensing")));
    }

    void testOpenKnowledgeBaseDialogIsManualAndDoesNotReturnDocuments() {
        const ToolDefinition tool = ToolSchema::instance().findTool(QStringLiteral("open_knowledge_base_dialog"));
        QCOMPARE(tool.name, QStringLiteral("open_knowledge_base_dialog"));
        QVERIFY(!tool.dangerous);
        QVERIFY(tool.params.isEmpty());
        QVERIFY(tool.description.contains(QStringLiteral("manually")));
        QVERIFY(tool.description.contains(QStringLiteral("does not read or return document contents")));
    }

    void testFindTool() {
        ToolDefinition t = ToolSchema::instance().findTool("create_project");
        QCOMPARE(t.name, QString("create_project"));
        // 阶段1 §3：新增 onUnsaved（未保存工程切换策略 save|discard|cancel，默认 cancel）
        QCOMPARE(t.params.size(), 2);
        QCOMPARE(t.params[0].name, QString("name"));
        QCOMPARE(t.params[1].name, QString("onUnsaved"));
        QCOMPARE(t.params[1].type, QString("enum"));
        QVERIFY(t.params[1].enumValues.contains("cancel"));
    }

    void testAvailablePluginsDoesNotClaimDeviceReadiness() {
        const ToolDefinition tool = ToolSchema::instance().findTool(QStringLiteral("get_available_plugins"));
        QVERIFY(tool.description.contains(QStringLiteral("loaded state")));
        QVERIFY(tool.description.contains(QStringLiteral("separate camera-driver catalog")));
        QVERIFY(tool.description.contains(QStringLiteral("platformCompatibility")));
        QVERIFY(tool.description.contains(QStringLiteral("does not prove")));
        QVERIFY(!tool.dangerous);
    }

    void testGetAnnotationSessionSchemaIsReadOnlyAndPaged() {
        const ToolDefinition tool = ToolSchema::instance().findTool(QStringLiteral("get_annotation_session"));
        QCOMPARE(tool.name, QStringLiteral("get_annotation_session"));
        QVERIFY(!tool.dangerous);
        QVERIFY(tool.description.contains(QStringLiteral("pixel coordinates of that session's bound image")));
        QVERIFY(tool.description.contains(QStringLiteral("crop-local")));
        QVERIFY(tool.description.contains(QStringLiteral("do not map to the parent image")));
        QCOMPARE(tool.params.size(), 4);
        QCOMPARE(tool.params.at(0).name, QStringLiteral("sessionPath"));
        QCOMPARE(tool.params.at(0).type, QStringLiteral("string"));
        QVERIFY(tool.params.at(0).required);
        QCOMPARE(tool.params.at(1).type, QStringLiteral("integer"));
        QCOMPARE(tool.params.at(2).type, QStringLiteral("integer"));
        QCOMPARE(tool.params.at(3).name, QStringLiteral("includePolygon"));
        QCOMPARE(tool.params.at(3).type, QStringLiteral("boolean"));

        const QJsonObject function = tool.toOpenAIFunction().value(QStringLiteral("function")).toObject();
        const QJsonObject parameters = function.value(QStringLiteral("parameters")).toObject();
        const QJsonObject properties = parameters.value(QStringLiteral("properties")).toObject();
        QVERIFY(
            parameters.value(QStringLiteral("required")).toArray().contains(QJsonValue(QStringLiteral("sessionPath"))));
        QCOMPARE(properties.value(QStringLiteral("offset")).toObject().value(QStringLiteral("default")).toInt(), 0);
        QCOMPARE(properties.value(QStringLiteral("limit")).toObject().value(QStringLiteral("default")).toInt(), 20);
        QVERIFY(properties.value(QStringLiteral("includePolygon"))
                    .toObject()
                    .value(QStringLiteral("description"))
                    .toString()
                    .contains(QStringLiteral("session image pixel frame")));
        QCOMPARE(
            properties.value(QStringLiteral("includePolygon")).toObject().value(QStringLiteral("default")).toBool(),
            false);
    }

    void testActiveSamMaskToolsDeclarePrivacyAndConfirmationBoundaries() {
        const ToolDefinition list = ToolSchema::instance().findTool(QStringLiteral("list_active_annotation_objects"));
        QVERIFY(!list.name.isEmpty());
        QVERIFY(!list.dangerous);
        QCOMPARE(list.params.size(), 2);
        QCOMPARE(list.params.at(0).name, QStringLiteral("offset"));
        QCOMPARE(list.params.at(0).defaultValue.toInt(), 0);
        QCOMPARE(list.params.at(1).name, QStringLiteral("limit"));
        QCOMPARE(list.params.at(1).defaultValue.toInt(), 20);

        const ToolDefinition edit = ToolSchema::instance().findTool(QStringLiteral("edit_active_annotation_masks"));
        QVERIFY(!edit.name.isEmpty());
        QVERIFY(edit.dangerous);
        QCOMPARE(edit.params.size(), 4);
        QVERIFY(edit.params.at(0).enumValues.contains(QStringLiteral("erase_stroke")));
        QCOMPARE(edit.params.at(1).name, QStringLiteral("objectIds"));
        QCOMPARE(edit.params.at(1).type, QStringLiteral("string_array"));
        QCOMPARE(edit.params.at(2).name, QStringLiteral("radiusPx"));
        QVERIFY(edit.params.at(2).description.contains(QStringLiteral("active annotation image pixel units")));
        QCOMPARE(edit.params.at(3).name, QStringLiteral("strokePoints"));
        QVERIFY(edit.description.contains(QStringLiteral("never coordinates")));
        QVERIFY(edit.description.contains(QStringLiteral("active annotation image pixel coordinates")));
        QVERIFY(edit.description.contains(QStringLiteral("crop-local")));
        QVERIFY(edit.description.contains(QStringLiteral("do not map to the parent image")));
        QVERIFY(!edit.description.contains(QStringLiteral("original image pixel coordinates")));
        const QJsonObject function = edit.toOpenAIFunction().value(QStringLiteral("function")).toObject();
        const QJsonObject points = function.value(QStringLiteral("parameters"))
                                       .toObject()
                                       .value(QStringLiteral("properties"))
                                       .toObject()
                                       .value(QStringLiteral("strokePoints"))
                                       .toObject();
        QCOMPARE(points.value(QStringLiteral("minItems")).toInt(), 1);
        QCOMPARE(points.value(QStringLiteral("maxItems")).toInt(), 4096);
        const QJsonObject point = points.value(QStringLiteral("items")).toObject();
        QCOMPARE(point.value(QStringLiteral("minItems")).toInt(), 2);
        QCOMPARE(point.value(QStringLiteral("maxItems")).toInt(), 2);
        QCOMPARE(point.value(QStringLiteral("items")).toObject().value(QStringLiteral("type")).toString(),
                 QStringLiteral("number"));
    }

    void testActiveSamCategoryToolsDeclareReadAndWriteBoundaries() {
        const ToolDefinition list =
            ToolSchema::instance().findTool(QStringLiteral("list_active_annotation_categories"));
        QVERIFY(!list.name.isEmpty());
        QVERIFY(!list.dangerous);
        QCOMPARE(list.params.size(), 2);
        QCOMPARE(list.params.at(0).defaultValue.toInt(), 0);
        QCOMPARE(list.params.at(1).defaultValue.toInt(), 20);

        const ToolDefinition edit =
            ToolSchema::instance().findTool(QStringLiteral("edit_active_annotation_categories"));
        QVERIFY(!edit.name.isEmpty());
        QVERIFY(edit.dangerous);
        QCOMPARE(edit.params.size(), 4);
        QVERIFY(edit.params.at(0).enumValues.contains(QStringLiteral("add_category")));
        QVERIFY(edit.params.at(0).enumValues.contains(QStringLiteral("remove_category")));
        QVERIFY(edit.params.at(0).enumValues.contains(QStringLiteral("set_object_label")));
        QCOMPARE(edit.params.at(1).name, QStringLiteral("category"));
        QCOMPARE(edit.params.at(2).type, QStringLiteral("string_array"));
        QCOMPARE(edit.params.at(3).name, QStringLiteral("label"));
    }

    void testActiveSamPromptToolsDeclarePrivacyAndConfirmationBoundaries() {
        const ToolDefinition status =
            ToolSchema::instance().findTool(QStringLiteral("get_active_annotation_prompt_status"));
        QVERIFY(!status.name.isEmpty());
        QVERIFY(!status.dangerous);
        QVERIFY(status.params.isEmpty());
        QVERIFY(status.description.contains(QStringLiteral("coordinates")));

        const ToolDefinition cancel =
            ToolSchema::instance().findTool(QStringLiteral("cancel_active_annotation_prompt"));
        QVERIFY(!cancel.name.isEmpty());
        QVERIFY(cancel.dangerous);
        QVERIFY(cancel.params.isEmpty());
        QVERIFY(cancel.description.contains(QStringLiteral("undo/redo")));

        const ToolDefinition save = ToolSchema::instance().findTool(QStringLiteral("save_active_annotation_session"));
        QVERIFY(!save.name.isEmpty());
        QVERIFY(save.dangerous);
        QCOMPARE(save.params.size(), 1);
        QCOMPARE(save.params.first().name, QStringLiteral("outputPath"));
        QVERIFY(save.params.first().required);
        QVERIFY(save.description.contains(QStringLiteral("never replaced")));

        const ToolDefinition open = ToolSchema::instance().findTool(QStringLiteral("open_active_annotation_session"));
        QVERIFY(!open.name.isEmpty());
        QVERIFY(open.dangerous);
        QCOMPARE(open.params.size(), 1);
        QCOMPARE(open.params.first().name, QStringLiteral("sessionPath"));
        QVERIFY(open.params.first().required);
        QVERIFY(open.description.contains(QStringLiteral("Requires confirmation")));
    }

    void testExportAnnotationSessionRequiresConfirmationAndNewTarget() {
        const ToolDefinition tool = ToolSchema::instance().findTool(QStringLiteral("export_annotation_session"));
        QCOMPARE(tool.name, QStringLiteral("export_annotation_session"));
        QVERIFY(tool.dangerous);
        QCOMPARE(tool.params.size(), 4);
        QVERIFY(tool.params.at(0).required);
        QVERIFY(tool.params.at(1).enumValues.contains(QStringLiteral("labelme")));
        QVERIFY(tool.params.at(1).enumValues.contains(QStringLiteral("yolo_seg")));
        QVERIFY(tool.params.at(1).enumValues.contains(QStringLiteral("coco")));
        QVERIFY(tool.params.at(2).required);
        QCOMPARE(tool.params.at(3).name, QStringLiteral("categories"));
        QCOMPARE(tool.params.at(3).type, QStringLiteral("string_array"));
        QVERIFY(!tool.params.at(3).required);

        const QJsonObject function = tool.toOpenAIFunction().value(QStringLiteral("function")).toObject();
        const QJsonObject properties =
            function.value(QStringLiteral("parameters")).toObject().value(QStringLiteral("properties")).toObject();
        QCOMPARE(properties.value(QStringLiteral("format")).toObject().value(QStringLiteral("enum")).toArray().size(),
                 3);
        QVERIFY(properties.value(QStringLiteral("categories")).toObject().contains(QStringLiteral("default")));
    }

    void testTransformAnnotationSessionRequiresConfirmationAndExplicitObjectIds() {
        const ToolDefinition tool = ToolSchema::instance().findTool(QStringLiteral("transform_annotation_session"));
        QCOMPARE(tool.name, QStringLiteral("transform_annotation_session"));
        QVERIFY(tool.dangerous);
        QCOMPARE(tool.params.size(), 5);
        QVERIFY(tool.params.at(0).required);
        QVERIFY(tool.params.at(1).required);
        QVERIFY(tool.params.at(2).required);
        QVERIFY(tool.params.at(2).enumValues.contains(QStringLiteral("set_label")));
        QVERIFY(tool.params.at(2).enumValues.contains(QStringLiteral("delete_objects")));
        QCOMPARE(tool.params.at(3).name, QStringLiteral("objectIds"));
        QCOMPARE(tool.params.at(3).type, QStringLiteral("string_array"));
        QVERIFY(tool.params.at(3).required);
        QCOMPARE(tool.params.at(4).name, QStringLiteral("label"));
        QVERIFY(!tool.params.at(4).required);
    }

    void testSelectFlowModuleIsReadOnlyNavigation() {
        const ToolDefinition tool = ToolSchema::instance().findTool(QStringLiteral("select_flow_module"));
        QCOMPARE(tool.name, QStringLiteral("select_flow_module"));
        QVERIFY(!tool.dangerous);
        QCOMPARE(tool.params.size(), 1);
        QCOMPARE(tool.params.first().name, QStringLiteral("instanceId"));
        QCOMPARE(tool.params.first().type, QStringLiteral("string"));
        QVERIFY(tool.params.first().required);
    }

    void testWorkspacePanelVisibilityIsGuiOnlyAndNotDangerous() {
        const ToolDefinition tool = ToolSchema::instance().findTool(QStringLiteral("set_workspace_panel_visibility"));
        QCOMPARE(tool.name, QStringLiteral("set_workspace_panel_visibility"));
        QVERIFY(!tool.dangerous);
        QCOMPARE(tool.params.size(), 2);
        QCOMPARE(tool.params.at(0).name, QStringLiteral("panel"));
        QCOMPARE(tool.params.at(0).type, QStringLiteral("string"));
        QVERIFY(tool.params.at(0).required);
        QCOMPARE(tool.params.at(0).enumValues,
                 QStringList({QStringLiteral("tools"), QStringLiteral("process"), QStringLiteral("bottom")}));
        QCOMPARE(tool.params.at(1).name, QStringLiteral("visible"));
        QCOMPARE(tool.params.at(1).type, QStringLiteral("boolean"));
        QVERIFY(tool.params.at(1).required);
    }

    void testLogLevelFilterUsesFiveCanonicalLevels() {
        const ToolDefinition tool = ToolSchema::instance().findTool(QStringLiteral("set_log_level_filter"));
        QCOMPARE(tool.name, QStringLiteral("set_log_level_filter"));
        QVERIFY(!tool.dangerous);
        QCOMPARE(tool.params.size(), 1);
        QCOMPARE(tool.params.first().name, QStringLiteral("minimumLevel"));
        QCOMPARE(tool.params.first().type, QStringLiteral("string"));
        QVERIFY(tool.params.first().required);
        QCOMPARE(tool.params.first().enumValues,
                 QStringList({QStringLiteral("all"), QStringLiteral("debug"), QStringLiteral("info"),
                              QStringLiteral("warning"), QStringLiteral("error")}));
    }

    void testLogPanelTabSelectionExcludesConversationPage() {
        const ToolDefinition tool = ToolSchema::instance().findTool(QStringLiteral("select_log_panel_tab"));
        QCOMPARE(tool.name, QStringLiteral("select_log_panel_tab"));
        QVERIFY(!tool.dangerous);
        QCOMPARE(tool.params.size(), 1);
        QCOMPARE(tool.params.first().name, QStringLiteral("tab"));
        QCOMPARE(tool.params.first().type, QStringLiteral("string"));
        QVERIFY(tool.params.first().required);
        QCOMPARE(tool.params.first().enumValues,
                 QStringList({QStringLiteral("runtime"), QStringLiteral("agent"), QStringLiteral("terminal")}));
    }

    void testScrollAgentChatToLatestIsNoArgumentReadOnlyTool() {
        const ToolDefinition tool = ToolSchema::instance().findTool(QStringLiteral("scroll_agent_chat_to_latest"));
        QCOMPARE(tool.name, QStringLiteral("scroll_agent_chat_to_latest"));
        QVERIFY(!tool.dangerous);
        QVERIFY(tool.params.isEmpty());
    }

    void testClearRuntimeLogsIsDangerousNoArgumentTool() {
        const ToolDefinition tool = ToolSchema::instance().findTool(QStringLiteral("clear_runtime_logs"));
        QCOMPARE(tool.name, QStringLiteral("clear_runtime_logs"));
        QVERIFY(tool.dangerous);
        QVERIFY(tool.params.isEmpty());
        QVERIFY(tool.description.contains(QStringLiteral("Does not truncate")));
        QVERIFY(tool.description.contains(QStringLiteral("Agent action history")));
    }

    void testListRecentProjectsIsNoArgumentReadOnlyTool() {
        const ToolDefinition tool = ToolSchema::instance().findTool(QStringLiteral("list_recent_projects"));
        QCOMPARE(tool.name, QStringLiteral("list_recent_projects"));
        QVERIFY(!tool.dangerous);
        QVERIFY(tool.params.isEmpty());
        QVERIFY(tool.description.contains(QStringLiteral("does not load or open projects")));
    }

    void testOpenAIFunctionFormat() {
        ToolDefinition t = ToolSchema::instance().findTool("run_flow");
        QJsonObject func = t.toOpenAIFunction();
        QVERIFY(func.contains("function"));
        QJsonObject f = func["function"].toObject();
        QCOMPARE(f["name"].toString(), QString("run_flow"));
        QVERIFY(f.contains("parameters"));
    }

    void testDangerousFlag() {
        ToolDefinition remove = ToolSchema::instance().findTool("remove_module");
        QVERIFY(remove.dangerous);

        ToolDefinition add = ToolSchema::instance().findTool("add_module");
        QVERIFY(!add.dangerous);
        QVERIFY(ToolSchema::instance().findTool("step_once").dangerous);
        QVERIFY(!ToolSchema::instance().findTool("reset_step").dangerous);
        QVERIFY(!ToolSchema::instance().findTool("validate_flow").dangerous);
        QVERIFY(!ToolSchema::instance().findTool("list_knowledge_documents").dangerous);
        QVERIFY(!ToolSchema::instance().findTool("preview_knowledge_document").dangerous);
        QVERIFY(ToolSchema::instance().findTool("add_knowledge_document").dangerous);
        QVERIFY(ToolSchema::instance().findTool("remove_knowledge_document").dangerous);
        QVERIFY(!ToolSchema::instance().findTool("list_global_variables").dangerous);
        QVERIFY(!ToolSchema::instance().findTool("get_global_variable").dangerous);
        QVERIFY(ToolSchema::instance().findTool("create_global_variable").dangerous);
        QVERIFY(ToolSchema::instance().findTool("set_global_variable").dangerous);
        QVERIFY(ToolSchema::instance().findTool("remove_global_variable").dangerous);
        QVERIFY(!ToolSchema::instance().findTool("list_data_sources").dangerous);
        QVERIFY(ToolSchema::instance().findTool("remove_data_source_reference").dangerous);
        QVERIFY(!ToolSchema::instance().findTool("get_data_source_view_state").dangerous);
        QVERIFY(ToolSchema::instance().findTool("display_data_source").dangerous);
        QVERIFY(ToolSchema::instance().findTool("set_data_source_visibility").dangerous);
        QCOMPARE(ToolSchema::instance().findTool("get_data_source_view_state").params.size(), 0);
        QCOMPARE(ToolSchema::instance().findTool("display_data_source").params.size(), 1);
        QCOMPARE(ToolSchema::instance().findTool("set_data_source_visibility").params.size(), 2);
        const ToolDefinition docs = ToolSchema::instance().findTool("read_documentation");
        QCOMPARE(docs.params.size(), 3);
        QCOMPARE(docs.params.at(1).type, QStringLiteral("integer"));
        QCOMPARE(docs.params.at(2).type, QStringLiteral("integer"));
    }

    void testGlobalVariableToolSchemas() {
        const ToolDefinition list = ToolSchema::instance().findTool(QStringLiteral("list_global_variables"));
        QCOMPARE(list.params.size(), 1);
        QVERIFY(list.params.first().enumValues.contains(QStringLiteral("int[]")));

        const ToolDefinition get = ToolSchema::instance().findTool(QStringLiteral("get_global_variable"));
        QCOMPARE(get.params.size(), 1);
        QCOMPARE(get.params.first().name, QStringLiteral("name"));
        QVERIFY(!get.dangerous);

        const ToolDefinition create = ToolSchema::instance().findTool(QStringLiteral("create_global_variable"));
        QVERIFY(create.dangerous);
        QCOMPARE(create.params.size(), 3);
        QCOMPARE(create.params.at(2).type, QStringLiteral("json"));
        const QJsonObject createSchema = create.params.at(2).toJson();
        QCOMPARE(createSchema.value(QStringLiteral("anyOf")).toArray().size(), 6);

        const ToolDefinition set = ToolSchema::instance().findTool(QStringLiteral("set_global_variable"));
        QVERIFY(set.dangerous);
        QCOMPARE(set.params.at(1).type, QStringLiteral("json"));
        QVERIFY(ToolSchema::instance().findTool(QStringLiteral("remove_global_variable")).dangerous);
    }

    void testDataSourceToolSchemas() {
        const ToolDefinition list = ToolSchema::instance().findTool(QStringLiteral("list_data_sources"));
        QVERIFY(!list.dangerous);
        QCOMPARE(list.params.size(), 1);
        QCOMPARE(list.params.first().name, QStringLiteral("type"));
        QVERIFY(list.params.first().enumValues.contains(QStringLiteral("image")));
        QVERIFY(list.params.first().enumValues.contains(QStringLiteral("pointcloud")));

        const ToolDefinition remove = ToolSchema::instance().findTool(QStringLiteral("remove_data_source_reference"));
        QVERIFY(remove.dangerous);
        QCOMPARE(remove.params.size(), 1);
        QCOMPARE(remove.params.first().name, QStringLiteral("dataSourceId"));
        QVERIFY(remove.params.first().required);

        const ToolDefinition import = ToolSchema::instance().findTool(QStringLiteral("import_data_source"));
        QVERIFY(import.dangerous);
        QCOMPARE(import.params.size(), 1);
        QCOMPARE(import.params.first().name, QStringLiteral("path"));
        QVERIFY(import.params.first().required);
        QVERIFY(import.description.contains(QStringLiteral("register only its file reference")));
        QVERIFY(import.description.contains(QStringLiteral("display_data_source separately")));
    }

    void testAllTools() {
        auto tools = ToolSchema::instance().allTools();
        QVERIFY(tools.size() >= 10);
        QVERIFY(ToolSchema::instance().hasTool(QStringLiteral("list_cameras")));
        QVERIFY(!ToolSchema::instance().findTool(QStringLiteral("list_cameras")).dangerous);
        QVERIFY(ToolSchema::instance().hasTool(QStringLiteral("list_communications")));
        QVERIFY(!ToolSchema::instance().findTool(QStringLiteral("list_communications")).dangerous);
        const ToolDefinition cameraDetails = ToolSchema::instance().findTool(QStringLiteral("get_camera_details"));
        QVERIFY(!cameraDetails.dangerous);
        QCOMPARE(cameraDetails.params.size(), 1);
        QCOMPARE(cameraDetails.params.first().name, QStringLiteral("deviceId"));
        QVERIFY(cameraDetails.params.first().required);
        const ToolDefinition cameraOperation = ToolSchema::instance().findTool(QStringLiteral("get_camera_operation"));
        QVERIFY(!cameraOperation.dangerous);
        QCOMPARE(cameraOperation.params.size(), 1);
        QCOMPARE(cameraOperation.params.first().name, QStringLiteral("requestId"));
        QCOMPARE(cameraOperation.params.first().type, QStringLiteral("string"));
        QVERIFY(cameraOperation.params.first().required);
        for (const QString& tool : {QStringLiteral("load_module_plugin"), QStringLiteral("unload_module_plugin")}) {
            QVERIFY(ToolSchema::instance().hasTool(tool));
            QVERIFY(ToolSchema::instance().findTool(tool).dangerous);
            const ToolDefinition definition = ToolSchema::instance().findTool(tool);
            QCOMPARE(definition.params.size(), 1);
            QCOMPARE(definition.params.first().name, QStringLiteral("plugin"));
            QVERIFY(definition.params.first().required);
        }
        for (const QString& tool : {QStringLiteral("connect_camera"), QStringLiteral("disconnect_camera")}) {
            QVERIFY(ToolSchema::instance().hasTool(tool));
            QVERIFY(ToolSchema::instance().findTool(tool).dangerous);
            const ToolDefinition definition = ToolSchema::instance().findTool(tool);
            QCOMPARE(definition.params.size(), 1);
            QCOMPARE(definition.params.first().name, QStringLiteral("deviceId"));
            QVERIFY(definition.params.first().required);
        }
    }

    void testCameraDiscoveryToolsDeclareSnapshotAndDangerousStart() {
        const ToolDefinition targets = ToolSchema::instance().findTool(QStringLiteral("get_camera_discovery_targets"));
        QVERIFY(!targets.name.isEmpty());
        QVERIFY(!targets.dangerous);
        QVERIFY(targets.params.isEmpty());

        const ToolDefinition discovery = ToolSchema::instance().findTool(QStringLiteral("discover_cameras"));
        QVERIFY(discovery.dangerous);
        QCOMPARE(discovery.params.size(), 3);
        QCOMPARE(discovery.params.at(0).name, QStringLiteral("registryGeneration"));
        QCOMPARE(discovery.params.at(0).type, QStringLiteral("string"));
        for (int i = 1; i < 3; ++i) {
            QCOMPARE(discovery.params.at(i).type, QStringLiteral("string_array"));
            QVERIFY(discovery.params.at(i).required);
        }
        const QJsonObject openAi = discovery.toOpenAIFunction().value(QStringLiteral("function")).toObject();
        const QJsonObject properties =
            openAi.value(QStringLiteral("parameters")).toObject().value(QStringLiteral("properties")).toObject();
        QCOMPARE(properties.value(QStringLiteral("pluginIds"))
                     .toObject()
                     .value(QStringLiteral("items"))
                     .toObject()
                     .value(QStringLiteral("type"))
                     .toString(),
                 QStringLiteral("string"));

        const ToolDefinition result = ToolSchema::instance().findTool(QStringLiteral("get_camera_discovery_operation"));
        QVERIFY(!result.name.isEmpty());
        QVERIFY(!result.dangerous);
        QCOMPARE(result.params.size(), 1);
        QCOMPARE(result.params.first().name, QStringLiteral("requestId"));
    }

    void testTypedParameterToolsSchema() {
        const auto& registry = ToolSchema::instance();
        for (const QString& tool :
             {QStringLiteral("set_param"), QStringLiteral("set_module_params"), QStringLiteral("reset_module_params")})
            QVERIFY2(registry.hasTool(tool), qPrintable(tool));
        const QJsonObject legacy = registry.findTool(QStringLiteral("set_param"))
                                       .toOpenAIFunction()
                                       .value(QStringLiteral("function"))
                                       .toObject();
        const QJsonObject value = legacy.value(QStringLiteral("parameters"))
                                      .toObject()
                                      .value(QStringLiteral("properties"))
                                      .toObject()
                                      .value(QStringLiteral("value"))
                                      .toObject();
        QVERIFY(value.value(QStringLiteral("anyOf")).isArray());
        const QJsonObject batch = registry.findTool(QStringLiteral("set_module_params"))
                                      .toOpenAIFunction()
                                      .value(QStringLiteral("function"))
                                      .toObject();
        QCOMPARE(batch.value(QStringLiteral("parameters"))
                     .toObject()
                     .value(QStringLiteral("properties"))
                     .toObject()
                     .value(QStringLiteral("values"))
                     .toObject()
                     .value(QStringLiteral("type"))
                     .toString(),
                 QStringLiteral("object"));
    }

    void testMoveModuleIndexIsIntegerInPublishedSchema() {
        const QJsonObject function = ToolSchema::instance()
                                         .findTool(QStringLiteral("move_module"))
                                         .toOpenAIFunction()
                                         .value("function")
                                         .toObject();
        const QJsonObject index =
            function.value("parameters").toObject().value("properties").toObject().value("index").toObject();
        QCOMPARE(index.value("type").toString(), QStringLiteral("integer"));
        QVERIFY(function.value("parameters").toObject().value("required").toArray().contains(QJsonValue("index")));
    }

    void testSetModulePositionPublishesIntegerCoordinates() {
        const QJsonObject function = ToolSchema::instance()
                                         .findTool(QStringLiteral("set_module_position"))
                                         .toOpenAIFunction()
                                         .value("function")
                                         .toObject();
        const QJsonObject parameters = function.value("parameters").toObject();
        const QJsonObject properties = parameters.value("properties").toObject();
        QCOMPARE(properties.value(QStringLiteral("x")).toObject().value(QStringLiteral("type")).toString(),
                 QStringLiteral("integer"));
        QCOMPARE(properties.value(QStringLiteral("y")).toObject().value(QStringLiteral("type")).toString(),
                 QStringLiteral("integer"));
        for (const QString& required : {QStringLiteral("instanceId"), QStringLiteral("x"), QStringLiteral("y")})
            QVERIFY(parameters.value(QStringLiteral("required")).toArray().contains(required));
    }

    void testFlowSelectorIsPublishedAsOptionalStringOnAllFlowTools() {
        const QStringList names{QStringLiteral("run_flow"), QStringLiteral("validate_flow"),
                                QStringLiteral("get_flow_state"), QStringLiteral("step_once"),
                                QStringLiteral("reset_step")};
        for (const QString& name : names) {
            const ToolDefinition definition = ToolSchema::instance().findTool(name);
            const QJsonObject openAi = definition.toOpenAIFunction().value(QStringLiteral("function")).toObject();
            const QJsonObject openAiParameters = openAi.value(QStringLiteral("parameters")).toObject();
            const QJsonObject openAiFlowId = openAiParameters.value(QStringLiteral("properties"))
                                                 .toObject()
                                                 .value(QStringLiteral("flowId"))
                                                 .toObject();
            QCOMPARE(openAiFlowId.value(QStringLiteral("type")).toString(), QStringLiteral("string"));
            QVERIFY(!openAiParameters.value(QStringLiteral("required")).toArray().contains("flowId"));
            QVERIFY(openAiFlowId.value(QStringLiteral("description")).toString().contains("unavailable"));

            const QJsonObject claude = definition.toClaudeTool().value(QStringLiteral("function")).toObject();
            const QJsonObject claudeParameters = claude.value(QStringLiteral("parameters")).toObject();
            const QJsonObject claudeFlowId = claudeParameters.value(QStringLiteral("properties"))
                                                 .toObject()
                                                 .value(QStringLiteral("flowId"))
                                                 .toObject();
            QCOMPARE(claudeFlowId.value(QStringLiteral("type")).toString(), QStringLiteral("string"));
            QVERIFY(!claudeParameters.value(QStringLiteral("required")).toArray().contains("flowId"));
        }
    }

    void testRenameModulePublishedSchemaRequiresStringName() {
        const QJsonObject parameters = ToolSchema::instance()
                                           .findTool(QStringLiteral("rename_module"))
                                           .toOpenAIFunction()
                                           .value("function")
                                           .toObject()
                                           .value("parameters")
                                           .toObject();
        QCOMPARE(parameters.value("properties").toObject().value("name").toObject().value("type").toString(),
                 QStringLiteral("string"));
        QVERIFY(parameters.value("required").toArray().contains(QJsonValue("instanceId")));
        QVERIFY(parameters.value("required").toArray().contains(QJsonValue("name")));
    }

    void testSetModuleEnabledPublishedSchemaRequiresBoolean() {
        const QJsonObject parameters = ToolSchema::instance()
                                           .findTool(QStringLiteral("set_module_enabled"))
                                           .toOpenAIFunction()
                                           .value("function")
                                           .toObject()
                                           .value("parameters")
                                           .toObject();
        QCOMPARE(parameters.value("properties").toObject().value("enabled").toObject().value("type").toString(),
                 QStringLiteral("boolean"));
        QVERIFY(parameters.value("required").toArray().contains(QJsonValue("instanceId")));
        QVERIFY(parameters.value("required").toArray().contains(QJsonValue("enabled")));
    }

    void testSetBreakpointPublishedSchemaRequiresBoolean() {
        const QJsonObject parameters = ToolSchema::instance()
                                           .findTool(QStringLiteral("set_breakpoint"))
                                           .toOpenAIFunction()
                                           .value("function")
                                           .toObject()
                                           .value("parameters")
                                           .toObject();
        QCOMPARE(parameters.value("properties").toObject().value("enabled").toObject().value("type").toString(),
                 QStringLiteral("boolean"));
        QVERIFY(parameters.value("required").toArray().contains(QJsonValue("instanceId")));
        QVERIFY(parameters.value("required").toArray().contains(QJsonValue("enabled")));
    }

    void testSetModuleNotePublishedSchemaRequiresString() {
        const QJsonObject parameters = ToolSchema::instance()
                                           .findTool(QStringLiteral("set_module_note"))
                                           .toOpenAIFunction()
                                           .value("function")
                                           .toObject()
                                           .value("parameters")
                                           .toObject();
        QCOMPARE(parameters.value("properties").toObject().value("note").toObject().value("type").toString(),
                 QStringLiteral("string"));
        QVERIFY(parameters.value("required").toArray().contains(QJsonValue("instanceId")));
        QVERIFY(parameters.value("required").toArray().contains(QJsonValue("note")));
    }

    // SAM 批3：sam_predict / sam_annotate schema 形状与危险级口径
    void testSamToolsSchema() {
        QVERIFY(ToolSchema::instance().hasTool("sam_predict"));
        QVERIFY(ToolSchema::instance().hasTool("sam_annotate"));

        const ToolDefinition predict = ToolSchema::instance().findTool("sam_predict");
        QVERIFY(predict.dangerous); // 外部推理会发送图像/提示到 SAM 服务，Autopilot 必须确认
        QVERIFY(!predict.description.isEmpty());
        bool imagePathRequired = false;
        bool pointsOptional = false;
        bool boxOptional = false;
        bool candidateIndexOptional = false; // 批4（C）：多掩膜候选选择
        for (const ToolParam& p : predict.params) {
            if (p.name == QLatin1String("imagePath"))
                imagePathRequired = p.required;
            if (p.name == QLatin1String("points"))
                pointsOptional = !p.required;
            if (p.name == QLatin1String("box"))
                boxOptional = !p.required;
            if (p.name == QLatin1String("candidateIndex"))
                candidateIndexOptional = !p.required && p.type == QLatin1String("integer");
        }
        QVERIFY(imagePathRequired);
        QVERIFY(pointsOptional);
        QVERIFY(boxOptional);
        QVERIFY(candidateIndexOptional);
        // OpenAI function 形状可序列化且 required 列表正确
        const QJsonObject f = predict.toOpenAIFunction()["function"].toObject();
        QCOMPARE(f["name"].toString(), QString("sam_predict"));
        QVERIFY(f["parameters"].toObject()["properties"].toObject().contains("imagePath"));
        QVERIFY(f["parameters"].toObject()["properties"].toObject().contains("candidateIndex"));
        QVERIFY(f["parameters"].toObject()["required"].toArray().contains(QJsonValue("imagePath")));
        QVERIFY(!f["parameters"].toObject()["required"].toArray().contains(QJsonValue("candidateIndex")));

        const ToolDefinition annotate = ToolSchema::instance().findTool("sam_annotate");
        QVERIFY(annotate.dangerous); // 写标注会话文件 → 与 save_project 同口径需确认
        bool sessionPathRequired = false;
        bool itemsRequired = true;
        bool labelMeOptional = false;
        for (const ToolParam& p : annotate.params) {
            if (p.name == QLatin1String("sessionPath"))
                sessionPathRequired = p.required;
            if (p.name == QLatin1String("items"))
                itemsRequired = p.required;
            if (p.name == QLatin1String("labelMePath"))
                labelMeOptional = !p.required;
        }
        QVERIFY(sessionPathRequired);
        QVERIFY(!itemsRequired); // 批4：items 与 labelMePath 互斥二选一（handler 强制 XOR）
        QVERIFY(labelMeOptional);
    }
};

QTEST_MAIN(TestToolSchema)
#include "test_toolschema.moc"
