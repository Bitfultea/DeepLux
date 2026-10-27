# DeepLux Unit Tests

set(DEEPLUX_TEST_QPA_PLATFORM "offscreen" CACHE STRING "Qt platform plugin used by GUI tests")

# 原生 SAM 模型包默认位置：Linux/macOS 用 $HOME，Windows 用 %USERPROFILE%。
if(DEFINED ENV{HOME})
    set(DEEPLUX_TEST_HOME "$ENV{HOME}")
elseif(DEFINED ENV{USERPROFILE})
    file(TO_CMAKE_PATH "$ENV{USERPROFILE}" DEEPLUX_TEST_HOME)
else()
    set(DEEPLUX_TEST_HOME "")
endif()

set(TEST_SOURCES
    test_pathtutils.cpp
    test_modulebase.cpp
    test_mainviewmodel.cpp
    test_mainwindow.cpp
    test_project.cpp
    test_configmanager.cpp
    test_communicationmanager.cpp
    test_pluginmanager.cpp
    test_plugincontracts.cpp
    test_logger.cpp
    test_imagedata.cpp
    test_runengine.cpp
    test_fitline.cpp
    test_findcircle.cpp
    test_measureline.cpp
    test_fitcircle.cpp
    test_fitellipse.cpp
    test_detection_batch2.cpp
    test_3d_batch3.cpp
    test_batch4.cpp
    test_measurerect.cpp
    test_distancepl.cpp
    test_distancepp.cpp
    test_linesdistance.cpp
    test_terminalscreen.cpp
    test_ansiparser.cpp
    test_clihandler.cpp
    test_cameramanager.cpp
    test_plyloader.cpp
    test_tiffloader.cpp
    test_himagewidget.cpp
    test_flowcanvas.cpp
    test_propertypanel.cpp
    test_toolschema.cpp
    test_agentobserver.cpp
    test_agentactor.cpp
    test_agentpermissions.cpp
    test_agenttoolcontract.cpp
    test_agentretry.cpp
    test_agentundo.cpp
    test_agentcontroller.cpp
    test_agentbridge.cpp
    test_agente2e.cpp
    test_agent_draft_restore.cpp
    test_agentclosedloop.cpp
    test_variable_system_plugins.cpp
    test_measurementdata.cpp
    test_measuregap.cpp
    test_pointsurfacedistance.cpp
    test_freeformsurface.cpp
    test_measurementinput.cpp
    test_grabimage.cpp
    test_loadpointcloud.cpp
    test_3d_interaction.cpp
    test_logicplugins.cpp
    test_annotation_session.cpp
    test_annotation_coordinate_mapping.cpp
    test_annotationmaskops.cpp
    test_labelme_exporter.cpp
    test_yoloseg_exporter.cpp
    test_cocoexporter.cpp
    test_labelmeimporter.cpp
    test_annotationoverlaywidget.cpp
    test_expressionengine.cpp
    test_samannotatordialog.cpp
    test_sambackendclient.cpp
    test_sam_region_flow.cpp
    test_agent_sam_tools.cpp
    test_knowledgebasedialog.cpp
    test_moduleinspectorpanel.cpp
    test_pluginparametercontracts.cpp
    test_acceptance_flows.cpp
    test_projectmigration.cpp
    test_timeslice.cpp
    test_queueplugins.cpp
    test_systemplugins.cpp
    test_imageplugins.cpp
    test_remainingplugins.cpp
    test_finalplugins.cpp
)

# ========== 插件路径清单 ==========
# 顶层 CMake 在测试之前收集了所有插件 target 及其 metadata.json。
# 这里展开为一条编译期清单：<target>|<dll-or-so>|<metadata>@@...
# 测试通过 tests/TestPluginRegistry.h 解析，避免在测试代码里硬编码
# lib*.so/*.dll 名称，并天然支持 MSVC 多配置输出目录（bin/Release 等）。
get_property(DEEPLUX_PLUGIN_TARGET_ENTRIES GLOBAL PROPERTY DEEPLUX_PLUGIN_TARGET_ENTRIES)
set(DEEPLUX_TEST_PLUGIN_MANIFEST "")
foreach(_plugin_entry IN LISTS DEEPLUX_PLUGIN_TARGET_ENTRIES)
    string(REPLACE "|" ";" _plugin_parts "${_plugin_entry}")
    list(LENGTH _plugin_parts _plugin_part_count)
    if(_plugin_part_count LESS 2)
        continue()
    endif()
    list(GET _plugin_parts 0 _plugin_target)
    list(GET _plugin_parts 1 _plugin_metadata)
    if(TARGET ${_plugin_target})
        string(APPEND DEEPLUX_TEST_PLUGIN_MANIFEST
            "${_plugin_target}|$<TARGET_FILE:${_plugin_target}>|${_plugin_metadata}@@")
    endif()
endforeach()

function(deeplux_add_test_plugin_defs test_target)
    if(NOT DEEPLUX_TEST_PLUGIN_MANIFEST)
        return()
    endif()
    target_compile_definitions(${test_target} PRIVATE
        DEEPLUX_TEST_PLUGIN_MANIFEST="${DEEPLUX_TEST_PLUGIN_MANIFEST}")
endfunction()

foreach(TEST_FILE ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_FILE} NAME_WE)

    add_executable(${TEST_NAME} ${TEST_FILE})

    target_compile_definitions(${TEST_NAME} PRIVATE
        DEEPLUX_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

    target_link_libraries(${TEST_NAME}
        PRIVATE
            deeplux_core
            deeplux_ui
    )

    if(Qt6_FOUND)
        target_link_libraries(${TEST_NAME} PRIVATE Qt6::Core Qt6::Test)
    else()
        target_link_libraries(${TEST_NAME} PRIVATE Qt5::Core Qt5::Test)
    endif()

    if(OpenCV_FOUND)
        target_link_libraries(${TEST_NAME} PRIVATE ${OpenCV_LIBS})
        target_include_directories(${TEST_NAME} PRIVATE ${OpenCV_INCLUDE_DIRS})
    endif()

    if(TEST_NAME MATCHES "^(test_acceptance_flows|test_mainwindow|test_plugincontracts|test_pluginparametercontracts)$")
        deeplux_add_test_plugin_defs(${TEST_NAME})
        if(TEST_NAME STREQUAL "test_pluginparametercontracts")
            # 该测试会安装并校验全部插件，必须确保所有插件库先于测试构建完成。
            foreach(_plugin_entry IN LISTS DEEPLUX_PLUGIN_TARGET_ENTRIES)
                string(REPLACE "|" ";" _plugin_parts "${_plugin_entry}")
                list(GET _plugin_parts 0 _plugin_target)
                if(TARGET ${_plugin_target})
                    add_dependencies(${TEST_NAME} ${_plugin_target})
                endif()
            endforeach()
        endif()
    endif()

    if(TEST_NAME STREQUAL "test_fitline" OR TEST_NAME STREQUAL "test_plugincontracts")
        target_link_libraries(${TEST_NAME} PRIVATE FitLinePlugin)
    endif()
    if(TEST_NAME STREQUAL "test_pluginmanager")
        add_dependencies(${TEST_NAME} MeasurementInputPlugin)
        target_compile_definitions(${TEST_NAME} PRIVATE
            TEST_PLUGINMANAGER_LIB="$<TARGET_FILE:MeasurementInputPlugin>"
            TEST_PLUGINMANAGER_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/geometry/MeasurementInput/metadata.json")
        if(UNIX AND NOT APPLE)
            add_dependencies(${TEST_NAME} V4L2CameraPlugin)
            target_compile_definitions(${TEST_NAME} PRIVATE
                TEST_PLUGINMANAGER_CAMERA_LIB="$<TARGET_FILE:V4L2CameraPlugin>"
                TEST_PLUGINMANAGER_CAMERA_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/camera/V4L2Camera/metadata.json")
        endif()
    endif()
    if(TEST_NAME STREQUAL "test_findcircle" OR TEST_NAME STREQUAL "test_plugincontracts")
        target_link_libraries(${TEST_NAME} PRIVATE FindCirclePlugin)
    endif()
    if(TEST_NAME STREQUAL "test_measureline" OR TEST_NAME STREQUAL "test_plugincontracts")
        target_link_libraries(${TEST_NAME} PRIVATE MeasureLinePlugin)
    endif()
    if(TEST_NAME STREQUAL "test_fitcircle" OR TEST_NAME STREQUAL "test_plugincontracts")
        target_link_libraries(${TEST_NAME} PRIVATE FitCirclePlugin)
    endif()
    # 阶7 批1：FitEllipse 行为+流程验收
    if(TEST_NAME STREQUAL "test_fitellipse")
        target_link_libraries(${TEST_NAME} PRIVATE FitEllipsePlugin)
        target_compile_definitions(${TEST_NAME} PRIVATE
            TEST_FITELLIPSE_LIB_FitEllipse="$<TARGET_FILE:FitEllipsePlugin>"
            TEST_FITELLIPSE_LIB_MeasurementInput="$<TARGET_FILE:MeasurementInputPlugin>"
            TEST_FITELLIPSE_META_FitEllipse="${CMAKE_SOURCE_DIR}/src/plugins/geometry/FitEllipse/metadata.json"
            TEST_FITELLIPSE_META_MeasurementInput="${CMAKE_SOURCE_DIR}/src/plugins/geometry/MeasurementInput/metadata.json")
    endif()
    # 阶7 批2：检测识别批（MeasureCircle/EdgeDefectDetection）行为+流程验收
    # 阶7 批2 复核二轮（P2-1）：metadata/库/数据路径全部由 CMake 注入（跨平台/多配置）
    if(TEST_NAME STREQUAL "test_detection_batch2")
        target_link_libraries(${TEST_NAME}
            PRIVATE
                MeasureCirclePlugin
                EdgeDefectDetectionPlugin
                GrabImagePlugin
                MeasurementInputPlugin
        )
        target_compile_definitions(${TEST_NAME} PRIVATE
            TEST_BATCH2_LIB_GrabImage="$<TARGET_FILE:GrabImagePlugin>"
            TEST_BATCH2_LIB_MeasureCircle="$<TARGET_FILE:MeasureCirclePlugin>"
            TEST_BATCH2_LIB_EdgeDefectDetection="$<TARGET_FILE:EdgeDefectDetectionPlugin>"
            TEST_BATCH2_LIB_MeasurementInput="$<TARGET_FILE:MeasurementInputPlugin>"
            TEST_BATCH2_META_GrabImage="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/GrabImage/metadata.json"
            TEST_BATCH2_META_MeasureCircle="${CMAKE_SOURCE_DIR}/src/plugins/detection/MeasureCircle/metadata.json"
            TEST_BATCH2_META_EdgeDefectDetection="${CMAKE_SOURCE_DIR}/src/plugins/detection/EdgeDefectDetection/metadata.json"
            TEST_BATCH2_META_MeasurementInput="${CMAKE_SOURCE_DIR}/src/plugins/geometry/MeasurementInput/metadata.json"
            TEST_BATCH2_DATA_Circle="${CMAKE_SOURCE_DIR}/tests/acceptance/data/circle_640x480.png")
    endif()
    # 阶7 批3：0143D 批（3DPreProcessing/FitPlane/GapMeasure3D）行为+流程验收
    # metadata/库路径由 CMake 注入；32F TIFF 高度图由测试运行期生成（无静态资产）
    if(TEST_NAME STREQUAL "test_3d_batch3")
        target_link_libraries(${TEST_NAME}
            PRIVATE
                PreProcessing3DPlugin
                FitPlanePlugin
                GapMeasure3DPlugin
                GrabImagePlugin
        )
        target_compile_definitions(${TEST_NAME} PRIVATE
            TEST_BATCH3_LIB_GrabImage="$<TARGET_FILE:GrabImagePlugin>"
            TEST_BATCH3_LIB_PreProcessing3D="$<TARGET_FILE:PreProcessing3DPlugin>"
            TEST_BATCH3_LIB_FitPlane="$<TARGET_FILE:FitPlanePlugin>"
            TEST_BATCH3_LIB_GapMeasure3D="$<TARGET_FILE:GapMeasure3DPlugin>"
            TEST_BATCH3_META_GrabImage="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/GrabImage/metadata.json"
            TEST_BATCH3_META_PreProcessing3D="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/PreProcessing3D/metadata.json"
            TEST_BATCH3_META_FitPlane="${CMAKE_SOURCE_DIR}/src/plugins/geometry/FitPlane/metadata.json"
            TEST_BATCH3_META_GapMeasure3D="${CMAKE_SOURCE_DIR}/src/plugins/geometry/GapMeasure3D/metadata.json")
    endif()
    # 阶7 批4：CropImage（001图像处理）+ CalculateOffset（005坐标标定）行为+流程验收
    if(TEST_NAME STREQUAL "test_batch4")
        target_link_libraries(${TEST_NAME}
            PRIVATE
                CropImagePlugin
                CalculateOffsetPlugin
                GrabImagePlugin
                MeasurementInputPlugin
                MeasureCirclePlugin
        )
        target_compile_definitions(${TEST_NAME} PRIVATE
            TEST_BATCH4_LIB_GrabImage="$<TARGET_FILE:GrabImagePlugin>"
            TEST_BATCH4_LIB_CropImage="$<TARGET_FILE:CropImagePlugin>"
            TEST_BATCH4_LIB_MeasureCircle="$<TARGET_FILE:MeasureCirclePlugin>"
            TEST_BATCH4_LIB_MeasurementInput="$<TARGET_FILE:MeasurementInputPlugin>"
            TEST_BATCH4_LIB_CalculateOffset="$<TARGET_FILE:CalculateOffsetPlugin>"
            TEST_BATCH4_META_GrabImage="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/GrabImage/metadata.json"
            TEST_BATCH4_META_CropImage="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/CropImage/metadata.json"
            TEST_BATCH4_META_MeasureCircle="${CMAKE_SOURCE_DIR}/src/plugins/detection/MeasureCircle/metadata.json"
            TEST_BATCH4_META_MeasurementInput="${CMAKE_SOURCE_DIR}/src/plugins/geometry/MeasurementInput/metadata.json"
            TEST_BATCH4_META_CalculateOffset="${CMAKE_SOURCE_DIR}/src/plugins/calibration/CalculateOffset/metadata.json"
            TEST_BATCH4_DATA_Circle="${CMAKE_SOURCE_DIR}/tests/acceptance/data/circle_640x480.png")
    endif()
    if(TEST_NAME STREQUAL "test_measurerect" OR TEST_NAME STREQUAL "test_plugincontracts")
        target_link_libraries(${TEST_NAME} PRIVATE MeasureRectPlugin)
    endif()
    if(TEST_NAME STREQUAL "test_distancepl" OR TEST_NAME STREQUAL "test_plugincontracts")
        target_link_libraries(${TEST_NAME} PRIVATE DistancePLPlugin)
    endif()
    if(TEST_NAME STREQUAL "test_distancepp" OR TEST_NAME STREQUAL "test_plugincontracts")
        target_link_libraries(${TEST_NAME} PRIVATE DistancePPPlugin)
    endif()
    if(TEST_NAME STREQUAL "test_linesdistance" OR TEST_NAME STREQUAL "test_plugincontracts")
        target_link_libraries(${TEST_NAME} PRIVATE LinesDistancePlugin)
    endif()
    if(TEST_NAME STREQUAL "test_measuregap" OR TEST_NAME STREQUAL "test_plugincontracts")
        target_link_libraries(${TEST_NAME} PRIVATE MeasureGapPlugin)
    endif()
    if(TEST_NAME STREQUAL "test_pointsurfacedistance" OR TEST_NAME STREQUAL "test_plugincontracts")
        target_link_libraries(${TEST_NAME} PRIVATE PointSurfaceDistancePlugin)
    endif()
    if(TEST_NAME STREQUAL "test_freeformsurface" OR TEST_NAME STREQUAL "test_plugincontracts")
        target_link_libraries(${TEST_NAME} PRIVATE FreeformSurfacePlugin)
    endif()
    if(TEST_NAME STREQUAL "test_measurementinput" OR TEST_NAME STREQUAL "test_plugincontracts")
        target_link_libraries(${TEST_NAME} PRIVATE MeasurementInputPlugin)
    endif()
    if(TEST_NAME STREQUAL "test_grabimage")
        target_link_libraries(${TEST_NAME} PRIVATE GrabImagePlugin)
    endif()
    # SAM 批1：Region2D 契约 + SamPredict 生产者 + RegionStats 消费者行为+流程验收
    if(TEST_NAME STREQUAL "test_sam_region_flow")
        target_link_libraries(${TEST_NAME}
            PRIVATE
                SamPredictPlugin
                RegionStatsPlugin
                GrabImagePlugin
        )
        target_compile_definitions(${TEST_NAME} PRIVATE
            TEST_SAMREGION_LIB_GrabImage="$<TARGET_FILE:GrabImagePlugin>"
            TEST_SAMREGION_LIB_SamPredict="$<TARGET_FILE:SamPredictPlugin>"
            TEST_SAMREGION_LIB_RegionStats="$<TARGET_FILE:RegionStatsPlugin>"
            TEST_SAMREGION_META_GrabImage="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/GrabImage/metadata.json"
            TEST_SAMREGION_META_SamPredict="${CMAKE_SOURCE_DIR}/src/plugins/detection/SamPredict/metadata.json"
            TEST_SAMREGION_META_RegionStats="${CMAKE_SOURCE_DIR}/src/plugins/geometry/RegionStats/metadata.json"
            TEST_SAMREGION_DATA_Circle="${CMAKE_SOURCE_DIR}/tests/acceptance/data/circle_640x480.png")
    endif()
    # 阶7 Agent 评审修复：set_param 转换/undo 保真回归需要真实可 createModule 的插件
    if(TEST_NAME STREQUAL "test_agentactor")
        target_link_libraries(${TEST_NAME} PRIVATE GrabImagePlugin MeasureCirclePlugin MeasurementInputPlugin FitCirclePlugin)
        target_compile_definitions(${TEST_NAME} PRIVATE
            TEST_AGENTACTOR_LIB_GrabImage="$<TARGET_FILE:GrabImagePlugin>"
            TEST_AGENTACTOR_META_GrabImage="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/GrabImage/metadata.json"
            TEST_AGENTACTOR_LIB_MeasureCircle="$<TARGET_FILE:MeasureCirclePlugin>"
            TEST_AGENTACTOR_META_MeasureCircle="${CMAKE_SOURCE_DIR}/src/plugins/detection/MeasureCircle/metadata.json"
            TEST_AGENTACTOR_LIB_MeasurementInput="$<TARGET_FILE:MeasurementInputPlugin>"
            TEST_AGENTACTOR_META_MeasurementInput="${CMAKE_SOURCE_DIR}/src/plugins/geometry/MeasurementInput/metadata.json"
            TEST_AGENTACTOR_LIB_FitCircle="$<TARGET_FILE:FitCirclePlugin>"
            TEST_AGENTACTOR_META_FitCircle="${CMAKE_SOURCE_DIR}/src/plugins/geometry/FitCircle/metadata.json")
    endif()
    # 第一迭代闭环测试：安装真实 GrabImage 插件（Path 源）后跑 建流程→校验→运行→读输出→调参→截图
    if(TEST_NAME STREQUAL "test_agentclosedloop")
        target_link_libraries(${TEST_NAME} PRIVATE GrabImagePlugin)
        target_compile_definitions(${TEST_NAME} PRIVATE
            TEST_CLOSEDLOOP_LIB_GrabImage="$<TARGET_FILE:GrabImagePlugin>"
            TEST_CLOSEDLOOP_META_GrabImage="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/GrabImage/metadata.json")
    endif()
    if(TEST_NAME STREQUAL "test_loadpointcloud" OR TEST_NAME STREQUAL "test_plugincontracts")
        target_link_libraries(${TEST_NAME} PRIVATE LoadPointCloudPlugin)
    endif()
    if(TEST_NAME STREQUAL "test_timeslice")
        target_link_libraries(${TEST_NAME} PRIVATE TimeSlicePlugin)
    endif()
    if(TEST_NAME STREQUAL "test_queueplugins")
        target_link_libraries(${TEST_NAME} PRIVATE QueueInPlugin QueueOutPlugin)
    endif()
    if(TEST_NAME STREQUAL "test_systemplugins")
        target_link_libraries(${TEST_NAME} PRIVATE DataCheckPlugin SaveDataPlugin)
    endif()
    if(TEST_NAME STREQUAL "test_imageplugins")
        target_link_libraries(${TEST_NAME} PRIVATE PerProcessingPlugin MatchingPlugin)
    endif()
    if(TEST_NAME STREQUAL "test_remainingplugins")
        target_link_libraries(${TEST_NAME} PRIVATE TableOutPutPlugin ColorRecognitionPlugin DisplayDataPlugin
                                                   JiErHanDefectsDetPlugin)
    endif()
    if(TEST_NAME STREQUAL "test_finalplugins")
        target_link_libraries(${TEST_NAME} PRIVATE ImageScriptPlugin ShowPointPlugin FreeformSurfacePlugin)
    endif()
    if(TEST_NAME STREQUAL "test_clihandler" OR TEST_NAME STREQUAL "test_plugincontracts")
        target_link_libraries(${TEST_NAME} PRIVATE SystemTimePlugin)
    endif()
    if(TEST_NAME STREQUAL "test_clihandler")
        add_dependencies(${TEST_NAME} SystemTimePlugin)
        target_compile_definitions(${TEST_NAME} PRIVATE
            TEST_CLIHANDLER_PLUGIN_SystemTime="$<TARGET_FILE:SystemTimePlugin>"
            TEST_CLIHANDLER_META_SystemTime="${CMAKE_SOURCE_DIR}/src/plugins/system/SystemTime/metadata.json")
    endif()
    if(TEST_NAME STREQUAL "test_logicplugins")
        target_link_libraries(${TEST_NAME}
            PRIVATE
                IfPlugin
                LoopPlugin
                WhilePlugin
                DelayPlugin
        )
    endif()
    if(TEST_NAME STREQUAL "test_plugincontracts")
        target_link_libraries(${TEST_NAME}
            PRIVATE
                BlobPlugin
                PerProcessingPlugin
                MatchingPlugin
                LoopPlugin
                FolderPlugin
                SaveDataPlugin
                CreateStringPlugin
                SplitStringPlugin
                MathPlugin
        )
    endif()
    if(TEST_NAME STREQUAL "test_variable_system_plugins")
        target_link_libraries(${TEST_NAME}
            PRIVATE
                CreateStringPlugin
                SplitStringPlugin
                MathPlugin
                SystemTimePlugin
                SaveDataPlugin
                StrFormatPlugin
        )
    endif()
    # 验收工程测试：插件经 PluginManager 动态加载，仅需保证插件目标先于测试构建
    if(TEST_NAME STREQUAL "test_acceptance_flows")
        add_dependencies(${TEST_NAME}
            GrabImagePlugin
            FindCirclePlugin
            MeasurementInputPlugin
            DistancePPPlugin
            LoadPointCloudPlugin
            PointSurfaceDistancePlugin
            FitLinePlugin
            FitCirclePlugin
            IfPlugin
            DelayPlugin
            LoopPlugin
            WhilePlugin
            StopWhilePlugin
            ParallelPlugin
            MathPlugin
            SaveDataPlugin
        )
    endif()
    if(TEST_NAME STREQUAL "test_mainwindow")
        add_dependencies(${TEST_NAME}
            DistancePPPlugin
            FitCirclePlugin
            FitLinePlugin
            GrabImagePlugin
            LoadPointCloudPlugin
            MeasurementInputPlugin
            IfPlugin
            DelayPlugin
        )
        # 阶7 批4 复核四轮（P2）：CropImage 高级配置弹窗完整回归需直接实例化插件
        target_link_libraries(${TEST_NAME} PRIVATE CropImagePlugin)
    endif()
    if(TEST_NAME STREQUAL "test_agente2e")
        add_dependencies(${TEST_NAME}
            GrabImagePlugin
            FindCirclePlugin
        )
        # 阶段 5 复核：插件库路径以 $<TARGET_FILE:...> 注入，测试不再拼接 .so 库名（跨平台）
        target_compile_definitions(${TEST_NAME} PRIVATE
            AGENTE2E_PLUGIN_GrabImage="$<TARGET_FILE:GrabImagePlugin>"
            AGENTE2E_PLUGIN_FindCircle="$<TARGET_FILE:FindCirclePlugin>")
    endif()

    # GUI 测试在扫描插件/换肤时会输出大量 qDebug，Qt 6 的默认 -maxwarnings
    # 上限会把它当成失败；显式放宽，行为断言仍由 QVERIFY/QCOMPARE 负责。
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME} -maxwarnings 100000)
    set_tests_properties(${TEST_NAME} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${DEEPLUX_TEST_QPA_PLATFORM}")
    if(TEST_NAME STREQUAL "test_pluginmanager")
        set_tests_properties(${TEST_NAME} PROPERTIES RUN_SERIAL TRUE)
    endif()
    if(TEST_NAME STREQUAL "test_acceptance_flows")
        set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 10)
    endif()
    if(TEST_NAME STREQUAL "test_cameramanager")
        set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 2)
    endif()
    # 阶7 批1 复核四轮：拟合含 RANSAC 循环，设 TIMEOUT 防无限循环回归挂死 CI
    if(TEST_NAME STREQUAL "test_fitellipse")
        set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 60)
    endif()
    # 阶7 批4 复核三轮（P0-1）：角度归一含超大有限数用例，设 TIMEOUT 防循环不终止挂死 CI
    if(TEST_NAME STREQUAL "test_batch4")
        set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 60)
    endif()
    # SAM 批1：测试内 HTTP 服务 + 嵌套事件循环收口，设 TIMEOUT 防挂死 CI
    if(TEST_NAME STREQUAL "test_sam_region_flow")
        set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 120)
    endif()
    # SAM 批3：Agent SAM 工具 × 测试内 HTTP 服务，同设 TIMEOUT
    if(TEST_NAME STREQUAL "test_agent_sam_tools")
        set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 120)
    endif()
    # 第一迭代：LLM 重试测试包含真实指数退避等待（最长 4s）与本地假端点，设 TIMEOUT 防挂死
    if(TEST_NAME STREQUAL "test_agentretry")
        set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 120)
    endif()
endforeach()

add_executable(ui_capture_mainwindow ui_capture_mainwindow.cpp)
target_link_libraries(ui_capture_mainwindow
    PRIVATE
        deeplux_core
        deeplux_ui
)
add_dependencies(ui_capture_mainwindow
    GrabImagePlugin
    FindCirclePlugin
    LoadPointCloudPlugin
    MeasurementInputPlugin
    FitCirclePlugin
    IfPlugin
    DelayPlugin
    CropImagePlugin
)
if(Qt6_FOUND)
    target_link_libraries(ui_capture_mainwindow PRIVATE Qt6::Core Qt6::Widgets Qt6::Test)
else()
    target_link_libraries(ui_capture_mainwindow PRIVATE Qt5::Core Qt5::Widgets Qt5::Test)
endif()
if(OpenCV_FOUND)
    target_link_libraries(ui_capture_mainwindow PRIVATE ${OpenCV_LIBS})
    target_include_directories(ui_capture_mainwindow PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()
# 阶段 5 复核：插件库路径以 $<TARGET_FILE:...> 注入，测试不再拼接 .so 库名（跨平台）
target_compile_definitions(ui_capture_mainwindow PRIVATE
    DEEPLUX_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    UICAP_PLUGIN_GrabImage="$<TARGET_FILE:GrabImagePlugin>"
    UICAP_PLUGIN_FindCircle="$<TARGET_FILE:FindCirclePlugin>"
    UICAP_PLUGIN_LoadPointCloud="$<TARGET_FILE:LoadPointCloudPlugin>"
    UICAP_PLUGIN_MeasurementInput="$<TARGET_FILE:MeasurementInputPlugin>"
    UICAP_PLUGIN_FitCircle="$<TARGET_FILE:FitCirclePlugin>"
    UICAP_PLUGIN_If="$<TARGET_FILE:IfPlugin>"
    UICAP_PLUGIN_Delay="$<TARGET_FILE:DelayPlugin>"
    UICAP_PLUGIN_CropImage="$<TARGET_FILE:CropImagePlugin>")

# 阶段 5：GUI 截图验收注册为 CTest——
# 验收运行或任一截图自校验（存在/尺寸/非空白/关键界面状态）失败都会使测试失败。
add_test(NAME ui_capture_mainwindow COMMAND ui_capture_mainwindow "${CMAKE_CURRENT_BINARY_DIR}/ui_capture_output")
set_tests_properties(ui_capture_mainwindow PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=${DEEPLUX_TEST_QPA_PLATFORM}"
    TIMEOUT 300)

# ========== 阶段2：原生 SAM ONNX 推理测试（条件编译）==========
if(DEEPLUX_NATIVE_SAM)
    target_compile_definitions(test_samannotatordialog PRIVATE
        TEST_SAM_PACKAGE_DIR="${DEEPLUX_TEST_HOME}/Data/Models/sam1_vit_b_onnx")
    set_tests_properties(test_samannotatordialog PROPERTIES TIMEOUT 120)
    add_executable(test_sam_onnx_inference test_sam_onnx_inference.cpp)
    target_link_libraries(test_sam_onnx_inference PRIVATE deeplux_core SamPredictPlugin)
    if(Qt6_FOUND)
        target_link_libraries(test_sam_onnx_inference PRIVATE Qt6::Core Qt6::Test)
    else()
        target_link_libraries(test_sam_onnx_inference PRIVATE Qt5::Core Qt5::Test)
    endif()
    target_compile_definitions(test_sam_onnx_inference PRIVATE
        TEST_SAM_PACKAGE_DIR="${DEEPLUX_TEST_HOME}/Data/Models/sam1_vit_b_onnx"
        TEST_SAM_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/acceptance/data/sam_native_golden"
        TEST_SAM_REAL_REPORT="${CMAKE_SOURCE_DIR}/docs/sam_native/stage1/compare_real_report.json")
    add_test(NAME test_sam_onnx_inference COMMAND test_sam_onnx_inference)
    set_tests_properties(test_sam_onnx_inference PROPERTIES TIMEOUT 120)
    add_test(NAME test_sam_native_pool_exit COMMAND test_sam_onnx_inference persistentPoolSessionSurvivesShutdown)
    set_tests_properties(test_sam_native_pool_exit PROPERTIES TIMEOUT 60)
endif()
