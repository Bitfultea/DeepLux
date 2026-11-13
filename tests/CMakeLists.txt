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
    test_globalvarview.cpp
    test_mainwindow.cpp
    test_fileexit_handoff.cpp
    test_agent_settings_handoff.cpp
    test_agent_about_handoff.cpp
    test_agent_knowledgebase_handoff.cpp
    test_agent_sam_annotator_handoff.cpp
    test_agent_viewport_snapshot_handoff.cpp
    test_agent_datasource_folder_handoff.cpp
    test_agent_knowledge_folder_handoff.cpp
    test_agent_globalvars_handoff.cpp
    test_project.cpp
    test_configmanager.cpp
    test_communicationmanager.cpp
    test_communicationplugins.cpp
    test_pluginmanager.cpp
    test_plugincontracts.cpp
    test_logger.cpp
    test_imagedata.cpp
    test_runengine.cpp
    test_flowgraphanalyzer.cpp
    test_executionaffinity.cpp
    test_runenginefinalizer.cpp
    test_runenginejoblease.cpp
    test_runengineuserpause.cpp
    test_flowexecutionservice.cpp
    test_fitline.cpp
    test_findcircle.cpp
    test_measureline.cpp
    test_fitcircle.cpp
    test_fitellipse.cpp
    test_fitellipse_flow.cpp
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
    test_agent_camera_lifecycle_g5.cpp
    test_camera_shutdown.cpp
    test_camera_discovery_shutdown.cpp
    test_plyloader.cpp
    test_tiffloader.cpp
    test_himagewidget.cpp
    test_imagerefreshcoalescer.cpp
    test_flowidentitycontract.cpp
    test_flowcanvas.cpp
    test_flowlayoutcoordinator.cpp
    test_processtreeorder.cpp
    test_propertypanel.cpp
    test_toolschema.cpp
    test_agentobserver.cpp
    test_agentactor.cpp
    test_agentpermissions.cpp
    test_agenttoolcontract.cpp
    test_agentretry.cpp
    test_agentundo.cpp
    test_agentcontroller.cpp
    test_openai_llm_client.cpp
    test_agent_recent_projects.cpp
    test_agent_set_theme.cpp
    test_agent_inspector_pin.cpp
    test_agent_viewport_zoom.cpp
    test_agent_viewport_point_size.cpp
    test_agent_viewport_display_mode.cpp
    test_agent_viewport_render_mode.cpp
    test_agent_datasource_dedicated_viewport.cpp
    test_agent_folder_execution_g5.cpp
    test_agent_plcwrite_execution_g5.cpp
    test_agent_tcpclient_execution_g5.cpp
    test_agent_tcpserver_execution_g5.cpp
    test_agent_plcread_execution_g5.cpp
    test_agent_serialport_execution_g5.cpp
    test_agent_plccommunicate_execution_g5.cpp
    test_agentbridge.cpp
    test_agente2e.cpp
    test_agent_draft_restore.cpp
    test_agentclosedloop.cpp
    test_agentstream.cpp
    test_agentstreamui.cpp
    test_agentaudit.cpp
    test_mcpbridge.cpp
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
    test_agentphase0_repro.cpp
    test_moduleinspectorpanel.cpp
    test_pluginparametercontracts.cpp
    test_workerpluginconsistency.cpp
    test_acceptance_flows.cpp
    test_projectmigration.cpp
    test_timeslice.cpp
    test_queueplugins.cpp
    test_systemplugins.cpp
    test_imageplugins.cpp
    test_remainingplugins.cpp
    test_finalplugins.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TEST_SOURCES test_v4l2discovery.cpp)
endif()

set(_deeplux_registered_test_sources
    ${TEST_SOURCES}
    test_agent_core_plugins_g8.cpp
    test_agent_visual_plugins_g8.cpp
    test_agent_variable_plugins_g8.cpp
    test_agent_plugin_g5.cpp
)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND _deeplux_registered_test_sources test_v4l2discovery.cpp)
endif()
if(DEEPLUX_NATIVE_SAM)
    list(APPEND _deeplux_registered_test_sources test_sam_onnx_inference.cpp)
endif()
file(GLOB _deeplux_top_level_test_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp")
foreach(_source_path IN LISTS _deeplux_top_level_test_sources)
    get_filename_component(_source_name "${_source_path}" NAME)
    if(_source_name STREQUAL "test_sam_onnx_inference.cpp" AND NOT DEEPLUX_NATIVE_SAM)
        continue()
    endif()
    list(FIND _deeplux_registered_test_sources "${_source_name}" _registered_index)
    if(_registered_index EQUAL -1)
        message(FATAL_ERROR "Top-level test source is not registered with CMake: ${_source_name}")
    endif()
endforeach()

set(DEEPLUX_ABI_FIXTURE_DIR "${CMAKE_CURRENT_BINARY_DIR}/abi-fixtures")
add_library(AbiDependency SHARED plugins/abi/AbiDependencyExports.cpp)
add_library(AbiValidPlugin SHARED
    plugins/abi/AbiFixturePlugin.cpp
    plugins/abi/AbiFixtureNativeLoadMarker.cpp
    plugins/abi/AbiFixturePlugin.h
    plugins/abi/metadata.json
    "${CMAKE_SOURCE_DIR}/src/core/interface/PluginAbiExports.cpp")
add_library(AbiWrongVersionPlugin SHARED
    plugins/abi/AbiFixturePlugin.cpp
    plugins/abi/AbiFixturePlugin.h
    plugins/abi/AbiWrongVersionExports.cpp
    plugins/abi/metadata.json)
add_library(AbiWrongFingerprintPlugin SHARED
    plugins/abi/AbiFixturePlugin.cpp
    plugins/abi/AbiFixturePlugin.h
    plugins/abi/AbiWrongFingerprintExports.cpp
    plugins/abi/metadata.json)
add_library(AbiLegacyPlugin SHARED
    plugins/abi/AbiFixturePlugin.cpp
    plugins/abi/AbiFixturePlugin.h
    plugins/abi/metadata.json)
add_library(AbiDependencyPlugin SHARED
    plugins/abi/AbiFixturePlugin.cpp
    plugins/abi/AbiDependencyAnchor.cpp
    plugins/abi/AbiFixturePlugin.h
    plugins/abi/metadata.json)

foreach(_target AbiDependency AbiValidPlugin AbiWrongVersionPlugin AbiWrongFingerprintPlugin AbiLegacyPlugin
        AbiDependencyPlugin)
    target_include_directories(${_target} PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src/core")
    set_target_properties(${_target} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${DEEPLUX_ABI_FIXTURE_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${DEEPLUX_ABI_FIXTURE_DIR}")
endforeach()
target_compile_definitions(AbiWrongVersionPlugin PRIVATE DEEPLUX_PLUGIN_EXPORTS)
target_compile_definitions(AbiValidPlugin PRIVATE DEEPLUX_PLUGIN_EXPORTS)
target_compile_definitions(AbiWrongFingerprintPlugin PRIVATE DEEPLUX_PLUGIN_EXPORTS)
target_compile_definitions(AbiLegacyPlugin PRIVATE DEEPLUX_PLUGIN_EXPORTS)
target_compile_definitions(AbiDependencyPlugin PRIVATE DEEPLUX_PLUGIN_EXPORTS)
target_link_libraries(AbiDependency PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core)
target_link_libraries(AbiValidPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Widgets)
target_link_libraries(AbiWrongVersionPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Widgets)
target_link_libraries(AbiWrongFingerprintPlugin PRIVATE
    deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Widgets)
target_link_libraries(AbiLegacyPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Widgets)
target_link_libraries(AbiDependencyPlugin PRIVATE
    AbiDependency deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Widgets)
if(WIN32)
    target_sources(AbiDependencyPlugin PRIVATE plugins/abi/AbiDependencyForwarders.def)
endif()

# G5 Slices 27-28 keep plugin lifecycle/cache fixtures in a process-isolated test.
add_library(AgentAbiMismatchFixture MODULE
    fixtures/agent_g5/AgentAbiMismatchFixture.cpp
    fixtures/agent_g5/AgentAbiMismatchFixture.h
    fixtures/agent_g5/abi_metadata.json
)
add_library(AgentSchemaCacheFixture MODULE
    fixtures/agent_g5/AgentSchemaCacheFixture.cpp
    fixtures/agent_g5/AgentSchemaCacheFixture.h
    ${CMAKE_SOURCE_DIR}/src/core/interface/PluginAbiExports.cpp
    fixtures/agent_g5/schema_metadata.json
)
add_library(AgentInterfaceVersionMismatchFixture MODULE
    fixtures/agent_g5/AgentInterfaceVersionMismatchFixture.cpp
    fixtures/agent_g5/AgentInterfaceVersionMismatchFixture.h
    ${CMAKE_SOURCE_DIR}/src/core/interface/PluginAbiExports.cpp
    fixtures/agent_g5/version_mismatch_metadata.json
)
add_library(AgentVariableArraySourceFixture MODULE
    fixtures/agent_g5/VariableArraySourceFixture.cpp
    fixtures/agent_g5/VariableArraySourceFixture.h
    ${CMAKE_SOURCE_DIR}/src/core/interface/PluginAbiExports.cpp
    fixtures/agent_g5/variable_array_source_metadata.json
)
target_link_libraries(AgentAbiMismatchFixture PRIVATE Qt${QT_VERSION_MAJOR}::Core)
target_link_libraries(AgentSchemaCacheFixture PRIVATE deeplux_core)
target_link_libraries(AgentInterfaceVersionMismatchFixture PRIVATE deeplux_core)
target_link_libraries(AgentVariableArraySourceFixture PRIVATE deeplux_core)
foreach(_target AgentSchemaCacheFixture AgentInterfaceVersionMismatchFixture AgentVariableArraySourceFixture)
    target_compile_definitions(${_target} PRIVATE DEEPLUX_PLUGIN_EXPORTS)
endforeach()
set_target_properties(AgentAbiMismatchFixture AgentSchemaCacheFixture AgentInterfaceVersionMismatchFixture
    AgentVariableArraySourceFixture PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test-fixtures"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test-fixtures"
)

add_executable(test_agent_plugin_g5 test_agent_plugin_g5.cpp)
target_link_libraries(test_agent_plugin_g5 PRIVATE deeplux_core deeplux_ui Qt${QT_VERSION_MAJOR}::Test)
target_compile_definitions(test_agent_plugin_g5 PRIVATE
    AGENT_G5_ABI_FIXTURE="$<TARGET_FILE:AgentAbiMismatchFixture>"
    AGENT_G5_SCHEMA_FIXTURE="$<TARGET_FILE:AgentSchemaCacheFixture>"
    AGENT_G5_VERSION_FIXTURE="$<TARGET_FILE:AgentInterfaceVersionMismatchFixture>"
    AGENT_G5_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/agent_g5"
)
add_dependencies(test_agent_plugin_g5 AgentAbiMismatchFixture AgentSchemaCacheFixture AgentInterfaceVersionMismatchFixture)
add_test(NAME test_agent_plugin_g5 COMMAND test_agent_plugin_g5)
set_tests_properties(test_agent_plugin_g5 PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=${DEEPLUX_TEST_QPA_PLATFORM}"
    TIMEOUT 30
)

add_executable(test_agent_variable_plugins_g8 test_agent_variable_plugins_g8.cpp)
target_link_libraries(test_agent_variable_plugins_g8 PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Test)
target_compile_definitions(test_agent_variable_plugins_g8 PRIVATE
    G8_VARDEFINE_PLUGIN="$<TARGET_FILE:VarDefinePlugin>"
    G8_VARSET_PLUGIN="$<TARGET_FILE:VarSetPlugin>"
    G8_VARDEFINE_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/variable/VarDefinePlugin/metadata.json"
    G8_VARSET_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/variable/VarSetPlugin/metadata.json"
)
add_dependencies(test_agent_variable_plugins_g8 VarDefinePlugin VarSetPlugin)
add_test(NAME test_agent_variable_plugins_g8 COMMAND test_agent_variable_plugins_g8)
set_tests_properties(test_agent_variable_plugins_g8 PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=${DEEPLUX_TEST_QPA_PLATFORM}"
    TIMEOUT 30
)

add_executable(test_agent_visual_plugins_g8 test_agent_visual_plugins_g8.cpp)
target_link_libraries(test_agent_visual_plugins_g8 PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Test ${OpenCV_LIBS})
target_include_directories(test_agent_visual_plugins_g8 PRIVATE ${OpenCV_INCLUDE_DIRS})
target_compile_definitions(test_agent_visual_plugins_g8 PRIVATE
    G8_QRCODE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/g8_qrcode.png"
    G8_GRABIMAGE_PLUGIN="$<TARGET_FILE:GrabImagePlugin>"
    G8_QRCODE_PLUGIN="$<TARGET_FILE:QRCodePlugin>"
    G8_SHOWIMAGE_PLUGIN="$<TARGET_FILE:ShowImagePlugin>"
    G8_SAVEIMAGE_PLUGIN="$<TARGET_FILE:SaveImagePlugin>"
    G8_DISPLAYDATA_PLUGIN="$<TARGET_FILE:DisplayDataPlugin>"
    G8_COLORRECOGNITION_PLUGIN="$<TARGET_FILE:ColorRecognitionPlugin>"
    G8_BLOB_PLUGIN="$<TARGET_FILE:BlobPlugin>"
    G8_FINDCIRCLE_PLUGIN="$<TARGET_FILE:FindCirclePlugin>"
    G8_CROPIMAGE_PLUGIN="$<TARGET_FILE:CropImagePlugin>"
    G8_MEASURECIRCLE_PLUGIN="$<TARGET_FILE:MeasureCirclePlugin>"
    G8_MEASUREMENTINPUT_PLUGIN="$<TARGET_FILE:MeasurementInputPlugin>"
    G8_DISTANCEPP_PLUGIN="$<TARGET_FILE:DistancePPPlugin>"
    G8_DISTANCEPL_PLUGIN="$<TARGET_FILE:DistancePLPlugin>"
    G8_FITCIRCLE_PLUGIN="$<TARGET_FILE:FitCirclePlugin>"
    G8_FITLINE_PLUGIN="$<TARGET_FILE:FitLinePlugin>"
    G8_FITELLIPSE_PLUGIN="$<TARGET_FILE:FitEllipsePlugin>"
    G8_CALCULATEOFFSET_PLUGIN="$<TARGET_FILE:CalculateOffsetPlugin>"
    G8_FITPLANE_PLUGIN="$<TARGET_FILE:FitPlanePlugin>"
    G8_MEASURELINE_PLUGIN="$<TARGET_FILE:MeasureLinePlugin>"
    G8_MEASURERECT_PLUGIN="$<TARGET_FILE:MeasureRectPlugin>"
    G8_LINESDISTANCE_PLUGIN="$<TARGET_FILE:LinesDistancePlugin>"
    G8_MEASUREGAP_PLUGIN="$<TARGET_FILE:MeasureGapPlugin>"
    G8_POINTSURFACEDISTANCE_PLUGIN="$<TARGET_FILE:PointSurfaceDistancePlugin>"
    G8_IMAGESCRIPT_PLUGIN="$<TARGET_FILE:ImageScriptPlugin>"
    G8_LOADPOINTCLOUD_PLUGIN="$<TARGET_FILE:LoadPointCloudPlugin>"
    G8_FREEFORMSURFACE_PLUGIN="$<TARGET_FILE:FreeformSurfacePlugin>"
    G8_JIGSAWPUZZLE_PLUGIN="$<TARGET_FILE:JigsawPuzzlePlugin>"
    G8_QUEUEOUT_PLUGIN="$<TARGET_FILE:QueueOutPlugin>"
    G8_QUEUEIN_PLUGIN="$<TARGET_FILE:QueueInPlugin>"
    G8_CREATESTRING_PLUGIN="$<TARGET_FILE:CreateStringPlugin>"
    G8_GAPMEASURE3D_PLUGIN="$<TARGET_FILE:GapMeasure3DPlugin>"
    G8_PERPROCESSING_PLUGIN="$<TARGET_FILE:PerProcessingPlugin>"
    G8_PREPROCESSING3D_PLUGIN="$<TARGET_FILE:PreProcessing3DPlugin>"
    G8_WHILE_PLUGIN="$<TARGET_FILE:WhilePlugin>"
    G8_STOPWHILE_PLUGIN="$<TARGET_FILE:StopWhilePlugin>"
    G8_DELAY_PLUGIN="$<TARGET_FILE:DelayPlugin>"
    G8_GRABIMAGE_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/GrabImage/metadata.json"
    G8_QRCODE_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/detection/QRCode/metadata.json"
    G8_SHOWIMAGE_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/ShowImage/metadata.json"
    G8_SAVEIMAGE_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/SaveImage/metadata.json"
    G8_DISPLAYDATA_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/DisplayData/metadata.json"
    G8_COLORRECOGNITION_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/detection/ColorRecognition/metadata.json"
    G8_BLOB_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/Blob/metadata.json"
    G8_FINDCIRCLE_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/detection/FindCircle/metadata.json"
    G8_CROPIMAGE_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/CropImage/metadata.json"
    G8_MEASURECIRCLE_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/detection/MeasureCircle/metadata.json"
    G8_MEASUREMENTINPUT_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/geometry/MeasurementInput/metadata.json"
    G8_DISTANCEPP_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/geometry/DistancePP/metadata.json"
    G8_DISTANCEPL_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/geometry/DistancePL/metadata.json"
    G8_FITCIRCLE_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/geometry/FitCircle/metadata.json"
    G8_FITLINE_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/geometry/FitLine/metadata.json"
    G8_FITELLIPSE_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/geometry/FitEllipse/metadata.json"
    G8_CALCULATEOFFSET_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/calibration/CalculateOffset/metadata.json"
    G8_FITPLANE_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/geometry/FitPlane/metadata.json"
    G8_MEASURELINE_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/detection/MeasureLine/metadata.json"
    G8_MEASURERECT_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/detection/MeasureRect/metadata.json"
    G8_LINESDISTANCE_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/geometry/LinesDistance/metadata.json"
    G8_MEASUREGAP_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/geometry/MeasureGap/metadata.json"
    G8_POINTSURFACEDISTANCE_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/geometry/PointSurfaceDistance/metadata.json"
    G8_IMAGESCRIPT_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/ImageScript/metadata.json"
    G8_LOADPOINTCLOUD_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/LoadPointCloud/metadata.json"
    G8_FREEFORMSURFACE_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/geometry/FreeformSurface/metadata.json"
    G8_JIGSAWPUZZLE_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/JigsawPuzzle/metadata.json"
    G8_QUEUEOUT_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/logic/QueueOut/metadata.json"
    G8_QUEUEIN_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/logic/QueueIn/metadata.json"
    G8_CREATESTRING_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/variable/CreateString/metadata.json"
    G8_GAPMEASURE3D_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/geometry/GapMeasure3D/metadata.json"
    G8_PERPROCESSING_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/PerProcessing/metadata.json"
    G8_PREPROCESSING3D_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/PreProcessing3D/metadata.json"
    G8_WHILE_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/logic/While/metadata.json"
    G8_STOPWHILE_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/logic/StopWhile/metadata.json"
    G8_DELAY_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/logic/Delay/metadata.json"
)
add_dependencies(test_agent_visual_plugins_g8 GrabImagePlugin QRCodePlugin ShowImagePlugin SaveImagePlugin
                 DisplayDataPlugin ColorRecognitionPlugin BlobPlugin FindCirclePlugin CropImagePlugin
                 MeasureCirclePlugin MeasurementInputPlugin DistancePPPlugin DistancePLPlugin FitCirclePlugin
                 FitLinePlugin FitEllipsePlugin CalculateOffsetPlugin FitPlanePlugin MeasureLinePlugin MeasureRectPlugin
                 LinesDistancePlugin MeasureGapPlugin PointSurfaceDistancePlugin ImageScriptPlugin LoadPointCloudPlugin
                 FreeformSurfacePlugin JigsawPuzzlePlugin QueueOutPlugin QueueInPlugin CreateStringPlugin
                 GapMeasure3DPlugin PerProcessingPlugin PreProcessing3DPlugin WhilePlugin StopWhilePlugin DelayPlugin)
add_test(NAME test_agent_visual_plugins_g8 COMMAND test_agent_visual_plugins_g8)
set_tests_properties(test_agent_visual_plugins_g8 PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=${DEEPLUX_TEST_QPA_PLATFORM}"
    TIMEOUT 30
)

add_executable(test_agent_core_plugins_g8 test_agent_core_plugins_g8.cpp)
target_link_libraries(test_agent_core_plugins_g8 PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Test)
target_compile_definitions(test_agent_core_plugins_g8 PRIVATE
    G8_PLCREAD_PLUGIN="$<TARGET_FILE:PLCReadPlugin>"
    G8_PLCREAD_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/communication/PLCReadPlugin/metadata.json"
    G8_MATH_PLUGIN="$<TARGET_FILE:MathPlugin>"
    G8_MATH_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/variable/MathPlugin/metadata.json"
    G8_CREATESTRING_PLUGIN="$<TARGET_FILE:CreateStringPlugin>"
    G8_CREATESTRING_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/variable/CreateString/metadata.json"
    G8_SPLITSTRING_PLUGIN="$<TARGET_FILE:SplitStringPlugin>"
    G8_SPLITSTRING_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/variable/SplitString/metadata.json"
    G8_STRFORMAT_PLUGIN="$<TARGET_FILE:StrFormatPlugin>"
    G8_STRFORMAT_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/variable/StrFormatPlugin/metadata.json"
    G8_SYSTEMTIME_PLUGIN="$<TARGET_FILE:SystemTimePlugin>"
    G8_SYSTEMTIME_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/system/SystemTime/metadata.json"
    G8_WRITETEXT_PLUGIN="$<TARGET_FILE:WriteTextPlugin>"
    G8_WRITETEXT_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/system/WriteText/metadata.json"
    G8_TIMESLICE_PLUGIN="$<TARGET_FILE:TimeSlicePlugin>"
    G8_TIMESLICE_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/system/TimeSlice/metadata.json"
    G8_CONDITION_PLUGIN="$<TARGET_FILE:ConditionPlugin>"
    G8_CONDITION_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/logic/Condition/metadata.json"
    G8_DELAY_PLUGIN="$<TARGET_FILE:DelayPlugin>"
    G8_DELAY_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/logic/Delay/metadata.json"
    G8_DATACHECK_PLUGIN="$<TARGET_FILE:DataCheckPlugin>"
    G8_DATACHECK_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/system/DataCheck/metadata.json"
    G8_TABLEOUTPUT_PLUGIN="$<TARGET_FILE:TableOutPutPlugin>"
    G8_TABLEOUTPUT_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/system/TableOutPut/metadata.json"
    G8_SAVEDATA_PLUGIN="$<TARGET_FILE:SaveDataPlugin>"
    G8_SAVEDATA_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/system/SaveData/metadata.json"
    G8_DEFECTDETECTION_PLUGIN="$<TARGET_FILE:DefectDetectionPlugin>"
    G8_DEFECTDETECTION_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/hymson3d/DefectDetection/metadata.json"
    G8_LOADPOINTCLOUD_PLUGIN="$<TARGET_FILE:LoadPointCloudPlugin>"
    G8_LOADPOINTCLOUD_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/LoadPointCloud/metadata.json"
)
add_dependencies(test_agent_core_plugins_g8 PLCReadPlugin MathPlugin CreateStringPlugin SplitStringPlugin StrFormatPlugin
                 SystemTimePlugin WriteTextPlugin TimeSlicePlugin ConditionPlugin DelayPlugin DataCheckPlugin
                 TableOutPutPlugin SaveDataPlugin DefectDetectionPlugin LoadPointCloudPlugin)
add_test(NAME test_agent_core_plugins_g8 COMMAND test_agent_core_plugins_g8)
set_tests_properties(test_agent_core_plugins_g8 PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=${DEEPLUX_TEST_QPA_PLATFORM}"
    TIMEOUT 30
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
            deeplux_core_test_support
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

    if(TEST_NAME MATCHES "^(test_acceptance_flows|test_mainwindow|test_plugincontracts|test_pluginparametercontracts|test_workerpluginconsistency|test_agentpermissions)$")
        deeplux_add_test_plugin_defs(${TEST_NAME})
        if(TEST_NAME STREQUAL "test_agentpermissions")
            add_dependencies(${TEST_NAME} GrabImagePlugin)
        endif()
        if(TEST_NAME STREQUAL "test_pluginparametercontracts" OR TEST_NAME STREQUAL "test_workerpluginconsistency")
            # 这些测试会安装并校验全部/候选插件，必须确保插件库先于测试构建完成。
            foreach(_plugin_entry IN LISTS DEEPLUX_PLUGIN_TARGET_ENTRIES)
                string(REPLACE "|" ";" _plugin_parts "${_plugin_entry}")
                list(GET _plugin_parts 0 _plugin_target)
                if(TARGET ${_plugin_target})
                    add_dependencies(${TEST_NAME} ${_plugin_target})
                endif()
            endforeach()
        endif()
    endif()
    if(TEST_NAME STREQUAL "test_plugincontracts")
        foreach(_plugin_entry IN LISTS DEEPLUX_PLUGIN_TARGET_ENTRIES)
            string(REPLACE "|" ";" _plugin_parts "${_plugin_entry}")
            list(GET _plugin_parts 0 _plugin_target)
            if(TARGET ${_plugin_target})
                add_dependencies(${TEST_NAME} ${_plugin_target})
            endif()
        endforeach()
    endif()

    if(TEST_NAME STREQUAL "test_fitline")
        target_link_libraries(${TEST_NAME} PRIVATE FitLinePlugin)
    endif()
    if(TEST_NAME STREQUAL "test_v4l2discovery")
        target_link_libraries(${TEST_NAME} PRIVATE V4L2CameraPlugin)
        target_include_directories(${TEST_NAME} PRIVATE
            ${CMAKE_SOURCE_DIR}/src/plugins/camera/V4L2Camera)
    endif()
    if(TEST_NAME STREQUAL "test_acceptance_flows")
        target_link_libraries(${TEST_NAME} PRIVATE BlobPlugin)
    endif()
    if(TEST_NAME STREQUAL "test_pluginmanager")
        add_dependencies(${TEST_NAME} MeasurementInputPlugin)
        add_dependencies(${TEST_NAME} AbiValidPlugin AbiWrongVersionPlugin AbiWrongFingerprintPlugin AbiLegacyPlugin
            AbiDependencyPlugin)
        target_compile_definitions(${TEST_NAME} PRIVATE
            TEST_PLUGINMANAGER_LIB="$<TARGET_FILE:MeasurementInputPlugin>"
            TEST_PLUGINMANAGER_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/geometry/MeasurementInput/metadata.json"
            TEST_ABI_WRONG_VERSION_PLUGIN="$<TARGET_FILE:AbiWrongVersionPlugin>"
            TEST_ABI_VALID_PLUGIN="$<TARGET_FILE:AbiValidPlugin>"
            TEST_ABI_WRONG_FINGERPRINT_PLUGIN="$<TARGET_FILE:AbiWrongFingerprintPlugin>"
            TEST_ABI_LEGACY_PLUGIN="$<TARGET_FILE:AbiLegacyPlugin>"
            TEST_ABI_DEPENDENCY_PLUGIN="$<TARGET_FILE:AbiDependencyPlugin>"
            TEST_ABI_DEPENDENCY_LIB="$<TARGET_FILE:AbiDependency>")
        if(UNIX AND NOT APPLE)
            add_dependencies(${TEST_NAME} V4L2CameraPlugin)
            target_compile_definitions(${TEST_NAME} PRIVATE
                TEST_PLUGINMANAGER_CAMERA_LIB="$<TARGET_FILE:V4L2CameraPlugin>"
                TEST_PLUGINMANAGER_CAMERA_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/camera/V4L2Camera/metadata.json")
        endif()
    endif()
    if(TEST_NAME STREQUAL "test_findcircle")
        target_link_libraries(${TEST_NAME} PRIVATE FindCirclePlugin)
    endif()
    if(TEST_NAME STREQUAL "test_measureline")
        target_link_libraries(${TEST_NAME} PRIVATE MeasureLinePlugin)
    endif()
    if(TEST_NAME STREQUAL "test_fitcircle")
        target_link_libraries(${TEST_NAME} PRIVATE FitCirclePlugin)
    endif()
    # 阶7 批1：FitEllipse 行为+流程验收
    if(TEST_NAME STREQUAL "test_fitellipse")
        target_link_libraries(${TEST_NAME} PRIVATE FitEllipsePlugin)
    endif()
    if(TEST_NAME STREQUAL "test_fitellipse_flow")
        add_dependencies(${TEST_NAME} FitEllipsePlugin MeasurementInputPlugin)
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
                JigsawPuzzlePlugin
                NPointCalibrationPlugin
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
    if(TEST_NAME STREQUAL "test_measurerect")
        target_link_libraries(${TEST_NAME} PRIVATE MeasureRectPlugin)
    endif()
    if(TEST_NAME STREQUAL "test_distancepl")
        target_link_libraries(${TEST_NAME} PRIVATE DistancePLPlugin)
    endif()
    if(TEST_NAME STREQUAL "test_distancepp")
        target_link_libraries(${TEST_NAME} PRIVATE DistancePPPlugin)
    endif()
    if(TEST_NAME STREQUAL "test_linesdistance")
        target_link_libraries(${TEST_NAME} PRIVATE LinesDistancePlugin)
    endif()
    if(TEST_NAME STREQUAL "test_measuregap")
        target_link_libraries(${TEST_NAME} PRIVATE MeasureGapPlugin)
    endif()
    if(TEST_NAME STREQUAL "test_pointsurfacedistance")
        target_link_libraries(${TEST_NAME} PRIVATE PointSurfaceDistancePlugin)
    endif()
    if(TEST_NAME STREQUAL "test_freeformsurface")
        target_link_libraries(${TEST_NAME} PRIVATE FreeformSurfacePlugin)
    endif()
    if(TEST_NAME STREQUAL "test_measurementinput")
        target_link_libraries(${TEST_NAME} PRIVATE MeasurementInputPlugin)
    endif()
    if(TEST_NAME STREQUAL "test_grabimage")
        target_link_libraries(${TEST_NAME} PRIVATE GrabImagePlugin)
    endif()
    if(TEST_NAME STREQUAL "test_agent_sam_tools")
        add_dependencies(${TEST_NAME} GrabImagePlugin CropImagePlugin SamPredictPlugin DelayPlugin)
        target_compile_definitions(${TEST_NAME} PRIVATE
            AGENT_SAM_G5_GRABIMAGE_LIBRARY="$<TARGET_FILE:GrabImagePlugin>"
            AGENT_SAM_G5_GRABIMAGE_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/GrabImage/metadata.json"
            AGENT_SAM_G5_CROPIMAGE_LIBRARY="$<TARGET_FILE:CropImagePlugin>"
            AGENT_SAM_G5_CROPIMAGE_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/CropImage/metadata.json"
            AGENT_SAM_G5_DELAY_LIBRARY="$<TARGET_FILE:DelayPlugin>"
            AGENT_SAM_G5_DELAY_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/logic/Delay/metadata.json"
            AGENT_SAM_G5_SAMPREDICT_LIBRARY="$<TARGET_FILE:SamPredictPlugin>"
            AGENT_SAM_G5_SAMPREDICT_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/detection/SamPredict/metadata.json")
    endif()
    if(TEST_NAME STREQUAL "test_agent_camera_lifecycle_g5")
        target_link_libraries(${TEST_NAME} PRIVATE GrabImagePlugin)
        add_dependencies(${TEST_NAME} GrabImagePlugin DelayPlugin)
        target_compile_definitions(${TEST_NAME} PRIVATE DEEPLUX_ENABLE_TEST_HOOKS)
        target_compile_definitions(${TEST_NAME} PRIVATE
            AGENT_CAMERA_G5_GRABIMAGE_LIBRARY="$<TARGET_FILE:GrabImagePlugin>"
            AGENT_CAMERA_G5_GRABIMAGE_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/GrabImage/metadata.json"
            AGENT_CAMERA_G5_DELAY_LIBRARY="$<TARGET_FILE:DelayPlugin>"
            AGENT_CAMERA_G5_DELAY_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/logic/Delay/metadata.json")
    endif()
    if(TEST_NAME STREQUAL "test_cameramanager")
        target_compile_definitions(${TEST_NAME} PRIVATE DEEPLUX_ENABLE_TEST_HOOKS)
    endif()
    if(TEST_NAME STREQUAL "test_project" OR TEST_NAME STREQUAL "test_projectmigration" OR
       TEST_NAME STREQUAL "test_agentcontroller")
        target_compile_definitions(${TEST_NAME} PRIVATE DEEPLUX_ENABLE_TEST_HOOKS)
    endif()
    if(TEST_NAME STREQUAL "test_agent_folder_execution_g5")
        add_dependencies(${TEST_NAME} FolderPlugin GrabImagePlugin SaveDataPlugin SaveImagePlugin WriteTextPlugin)
        target_compile_definitions(${TEST_NAME} PRIVATE
            AGENT_FOLDER_G5_LIBRARY="$<TARGET_FILE:FolderPlugin>"
            AGENT_FOLDER_G5_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/system/Folder/metadata.json"
            AGENT_GRABIMAGE_G5_LIBRARY="$<TARGET_FILE:GrabImagePlugin>"
            AGENT_GRABIMAGE_G5_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/GrabImage/metadata.json"
            AGENT_SAVEDATA_G5_LIBRARY="$<TARGET_FILE:SaveDataPlugin>"
            AGENT_SAVEDATA_G5_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/system/SaveData/metadata.json"
            AGENT_SAVEIMAGE_G5_LIBRARY="$<TARGET_FILE:SaveImagePlugin>"
            AGENT_SAVEIMAGE_G5_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/SaveImage/metadata.json"
            AGENT_WRITETEXT_G5_LIBRARY="$<TARGET_FILE:WriteTextPlugin>"
            AGENT_WRITETEXT_G5_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/system/WriteText/metadata.json")
    endif()
    if(TEST_NAME STREQUAL "test_agent_plcwrite_execution_g5")
        add_dependencies(${TEST_NAME} PLCWritePlugin PLCReadPlugin AgentVariableArraySourceFixture)
        target_compile_definitions(${TEST_NAME} PRIVATE
            AGENT_PLCWRITE_G5_LIBRARY="$<TARGET_FILE:PLCWritePlugin>"
            AGENT_PLCWRITE_G5_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/communication/PLCWritePlugin/metadata.json"
            AGENT_PLCREAD_G5_LIBRARY="$<TARGET_FILE:PLCReadPlugin>"
            AGENT_PLCREAD_G5_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/communication/PLCReadPlugin/metadata.json"
            AGENT_VARIABLE_ARRAY_SOURCE_G5_LIBRARY="$<TARGET_FILE:AgentVariableArraySourceFixture>"
            AGENT_VARIABLE_ARRAY_SOURCE_G5_METADATA="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/agent_g5/variable_array_source_metadata.json")
    endif()
    if(TEST_NAME STREQUAL "test_agent_tcpclient_execution_g5")
        add_dependencies(${TEST_NAME} TCPClientPlugin)
        target_compile_definitions(${TEST_NAME} PRIVATE
            AGENT_TCPCLIENT_G5_LIBRARY="$<TARGET_FILE:TCPClientPlugin>"
            AGENT_TCPCLIENT_G5_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/communication/TCPClient/metadata.json")
    endif()
    if(TEST_NAME STREQUAL "test_agent_tcpserver_execution_g5")
        target_link_libraries(${TEST_NAME} PRIVATE TCPServerPlugin)
        add_dependencies(${TEST_NAME} TCPServerPlugin)
        target_include_directories(${TEST_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/src/plugins/communication/TCPServer")
        target_compile_definitions(${TEST_NAME} PRIVATE DEEPLUX_ENABLE_TEST_HOOKS)
        target_compile_definitions(TCPServerPlugin PRIVATE DEEPLUX_ENABLE_TEST_HOOKS)
        target_compile_definitions(${TEST_NAME} PRIVATE
            AGENT_TCPSERVER_G5_LIBRARY="$<TARGET_FILE:TCPServerPlugin>"
            AGENT_TCPSERVER_G5_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/communication/TCPServer/metadata.json")
    endif()
    if(TEST_NAME STREQUAL "test_agent_plcread_execution_g5")
        add_dependencies(${TEST_NAME} PLCReadPlugin)
        target_compile_definitions(${TEST_NAME} PRIVATE
            AGENT_PLCREAD_G5_LIBRARY="$<TARGET_FILE:PLCReadPlugin>"
            AGENT_PLCREAD_G5_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/communication/PLCReadPlugin/metadata.json")
    endif()
    if(TEST_NAME STREQUAL "test_agent_serialport_execution_g5")
        add_dependencies(${TEST_NAME} SerialPortPlugin)
        target_compile_definitions(${TEST_NAME} PRIVATE
            AGENT_SERIALPORT_G5_LIBRARY="$<TARGET_FILE:SerialPortPlugin>"
            AGENT_SERIALPORT_G5_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/communication/SerialPort/metadata.json")
    endif()
    if(TEST_NAME STREQUAL "test_agent_plccommunicate_execution_g5")
        add_dependencies(${TEST_NAME} PLCCommunicatePlugin)
        target_compile_definitions(${TEST_NAME} PRIVATE
            AGENT_PLCCOMMUNICATE_G5_LIBRARY="$<TARGET_FILE:PLCCommunicatePlugin>"
            AGENT_PLCCOMMUNICATE_G5_METADATA="${CMAKE_SOURCE_DIR}/src/plugins/communication/PLCCommunicatePlugin/metadata.json")
    endif()
    if(TEST_NAME STREQUAL "test_communicationplugins")
        target_link_libraries(${TEST_NAME} PRIVATE PLCReadPlugin PLCWritePlugin)
    endif()
    # SAM 批1：Region2D 契约 + SamPredict 生产者 + RegionStats 消费者行为+流程验收
    if(TEST_NAME STREQUAL "test_sam_region_flow")
        target_link_libraries(${TEST_NAME}
            PRIVATE
                SamPredictPlugin
                RegionStatsPlugin
                GrabImagePlugin
                CropImagePlugin
        )
        target_compile_definitions(${TEST_NAME} PRIVATE
            TEST_SAMREGION_LIB_GrabImage="$<TARGET_FILE:GrabImagePlugin>"
            TEST_SAMREGION_LIB_CropImage="$<TARGET_FILE:CropImagePlugin>"
            TEST_SAMREGION_LIB_SamPredict="$<TARGET_FILE:SamPredictPlugin>"
            TEST_SAMREGION_LIB_RegionStats="$<TARGET_FILE:RegionStatsPlugin>"
            TEST_SAMREGION_META_GrabImage="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/GrabImage/metadata.json"
            TEST_SAMREGION_META_CropImage="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/CropImage/metadata.json"
            TEST_SAMREGION_META_SamPredict="${CMAKE_SOURCE_DIR}/src/plugins/detection/SamPredict/metadata.json"
            TEST_SAMREGION_META_RegionStats="${CMAKE_SOURCE_DIR}/src/plugins/geometry/RegionStats/metadata.json"
            TEST_SAMREGION_DATA_Circle="${CMAKE_SOURCE_DIR}/tests/acceptance/data/circle_640x480.png")
    endif()
    # 阶7 Agent 评审修复：set_param 转换/undo 保真回归需要真实可 createModule 的插件
    if(TEST_NAME STREQUAL "test_agentactor")
        target_link_libraries(${TEST_NAME} PRIVATE GrabImagePlugin MeasureCirclePlugin MeasurementInputPlugin FitCirclePlugin IfPlugin ParallelPlugin SamPredictPlugin)
        target_compile_definitions(${TEST_NAME} PRIVATE
            TEST_AGENTACTOR_LIB_GrabImage="$<TARGET_FILE:GrabImagePlugin>"
            TEST_AGENTACTOR_META_GrabImage="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/GrabImage/metadata.json"
            TEST_AGENTACTOR_LIB_MeasureCircle="$<TARGET_FILE:MeasureCirclePlugin>"
            TEST_AGENTACTOR_META_MeasureCircle="${CMAKE_SOURCE_DIR}/src/plugins/detection/MeasureCircle/metadata.json"
            TEST_AGENTACTOR_LIB_MeasurementInput="$<TARGET_FILE:MeasurementInputPlugin>"
            TEST_AGENTACTOR_META_MeasurementInput="${CMAKE_SOURCE_DIR}/src/plugins/geometry/MeasurementInput/metadata.json"
            TEST_AGENTACTOR_LIB_FitCircle="$<TARGET_FILE:FitCirclePlugin>"
            TEST_AGENTACTOR_META_FitCircle="${CMAKE_SOURCE_DIR}/src/plugins/geometry/FitCircle/metadata.json"
            TEST_AGENTACTOR_LIB_If="$<TARGET_FILE:IfPlugin>"
            TEST_AGENTACTOR_META_If="${CMAKE_SOURCE_DIR}/src/plugins/logic/If/metadata.json"
            TEST_AGENTACTOR_LIB_Parallel="$<TARGET_FILE:ParallelPlugin>"
            TEST_AGENTACTOR_META_Parallel="${CMAKE_SOURCE_DIR}/src/plugins/logic/Parallel/metadata.json"
            TEST_AGENTACTOR_LIB_SamPredict="$<TARGET_FILE:SamPredictPlugin>"
            TEST_AGENTACTOR_META_SamPredict="${CMAKE_SOURCE_DIR}/src/plugins/detection/SamPredict/metadata.json")
    endif()
    # 第一迭代闭环测试：安装真实 GrabImage 插件（Path 源）后跑 建流程→校验→运行→读输出→调参→截图
    if(TEST_NAME STREQUAL "test_agentclosedloop")
        target_link_libraries(${TEST_NAME} PRIVATE GrabImagePlugin ShowImagePlugin)
        target_compile_definitions(${TEST_NAME} PRIVATE
            TEST_CLOSEDLOOP_LIB_GrabImage="$<TARGET_FILE:GrabImagePlugin>"
            TEST_CLOSEDLOOP_META_GrabImage="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/GrabImage/metadata.json"
            TEST_CLOSEDLOOP_LIB_ShowImage="$<TARGET_FILE:ShowImagePlugin>"
            TEST_CLOSEDLOOP_META_ShowImage="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/ShowImage/metadata.json")
    endif()
    if(TEST_NAME STREQUAL "test_loadpointcloud")
        target_link_libraries(${TEST_NAME} PRIVATE LoadPointCloudPlugin)
    endif()
    # 阶段 0：F01–F13 基线特征化复现（docs/agent-capability-improvement-plan.md §5.5）
    # 阶段 1：追加 PLCWrite（G1 副作用流程确认前零执行证据,仅构建流程不连接设备）
    if(TEST_NAME STREQUAL "test_agentphase0_repro")
        target_link_libraries(${TEST_NAME}
            PRIVATE
                GrabImagePlugin
                DelayPlugin
                MeasurementInputPlugin
                FitCirclePlugin
                NPointCalibrationPlugin
                FolderPlugin
                PLCWritePlugin
        )
        target_compile_definitions(${TEST_NAME} PRIVATE
            TEST_REPRO_LIB_GrabImage="$<TARGET_FILE:GrabImagePlugin>"
            TEST_REPRO_META_GrabImage="${CMAKE_SOURCE_DIR}/src/plugins/image_processing/GrabImage/metadata.json"
            TEST_REPRO_LIB_Delay="$<TARGET_FILE:DelayPlugin>"
            TEST_REPRO_META_Delay="${CMAKE_SOURCE_DIR}/src/plugins/logic/Delay/metadata.json"
            TEST_REPRO_LIB_MeasurementInput="$<TARGET_FILE:MeasurementInputPlugin>"
            TEST_REPRO_META_MeasurementInput="${CMAKE_SOURCE_DIR}/src/plugins/geometry/MeasurementInput/metadata.json"
            TEST_REPRO_LIB_FitCircle="$<TARGET_FILE:FitCirclePlugin>"
            TEST_REPRO_META_FitCircle="${CMAKE_SOURCE_DIR}/src/plugins/geometry/FitCircle/metadata.json"
            TEST_REPRO_LIB_NPointCalibration="$<TARGET_FILE:NPointCalibrationPlugin>"
            TEST_REPRO_META_NPointCalibration="${CMAKE_SOURCE_DIR}/src/plugins/calibration/NPointCalibration/metadata.json"
            TEST_REPRO_LIB_Folder="$<TARGET_FILE:FolderPlugin>"
            TEST_REPRO_META_Folder="${CMAKE_SOURCE_DIR}/src/plugins/system/Folder/metadata.json"
            TEST_REPRO_LIB_PLCWrite="$<TARGET_FILE:PLCWritePlugin>"
            TEST_REPRO_META_PLCWrite="${CMAKE_SOURCE_DIR}/src/plugins/communication/PLCWritePlugin/metadata.json")
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
        target_link_libraries(${TEST_NAME} PRIVATE BlobPlugin PerProcessingPlugin MatchingPlugin)
    endif()
    if(TEST_NAME STREQUAL "test_remainingplugins")
        target_link_libraries(${TEST_NAME} PRIVATE TableOutPutPlugin ColorRecognitionPlugin DisplayDataPlugin
                                                   JiErHanDefectsDetPlugin)
    endif()
    if(TEST_NAME STREQUAL "test_finalplugins")
        target_link_libraries(${TEST_NAME} PRIVATE ImageScriptPlugin ShowPointPlugin FreeformSurfacePlugin)
    endif()
    if(TEST_NAME STREQUAL "test_clihandler")
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
            ShowPointPlugin
            GrabImagePlugin
            EdgeDefectDetectionPlugin
            MatchingPlugin
            FindCirclePlugin
            MeasurementInputPlugin
            ColorRecognitionPlugin
            JiErHanDefectsDetPlugin
            CropImagePlugin
            MeasureRectPlugin
            DistancePPPlugin
            MeasureGapPlugin
            DistancePLPlugin
            LoadPointCloudPlugin
            FreeformSurfacePlugin
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
            NPointCalibrationPlugin
            IfPlugin
            DelayPlugin
        )
        # 阶7 批4 复核四轮（P2）：CropImage 高级配置弹窗完整回归需直接实例化插件
        target_link_libraries(${TEST_NAME} PRIVATE CropImagePlugin NPointCalibrationPlugin)
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

    # 第二迭代下半：MCP 桥直接复用 src/app 的静态库 deeplux_mcp（协议/权限在同一实现上测试）
    # 集成收口 B4：黑盒测试启动真实 deeplux-mcp-bridge 子进程
    if(TEST_NAME STREQUAL "test_mcpbridge")
        target_link_libraries(${TEST_NAME} PRIVATE deeplux_mcp)
        target_compile_definitions(${TEST_NAME} PRIVATE
            DEEPLUX_MCP_BRIDGE_PATH="$<TARGET_FILE:deeplux-mcp-bridge>")
    endif()

    # GUI integration suites can emit many Qt warnings during plugin scans and theme changes.
    set(GUI_TESTS
        test_3d_interaction
        test_annotationoverlaywidget
        test_flowcanvas
        test_himagewidget
        test_knowledgebasedialog
        test_mainwindow
        test_moduleinspectorpanel
        test_propertypanel
        test_samannotatordialog
    )
    if((TEST_NAME STREQUAL "test_mainwindow" OR TEST_NAME STREQUAL "test_agent_viewport_snapshot_handoff") AND
       CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME ${TEST_NAME}
            COMMAND bash ${CMAKE_SOURCE_DIR}/scripts/run-mainwindow-test-isolated.sh
                ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR} $<TARGET_FILE:${TEST_NAME}>)
    elseif(TEST_NAME IN_LIST GUI_TESTS)
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME} -maxwarnings 100000)
    else()
        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    endif()
    set_tests_properties(${TEST_NAME} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=${DEEPLUX_TEST_QPA_PLATFORM}")
    if(TEST_NAME STREQUAL "test_pluginmanager")
        set_tests_properties(${TEST_NAME} PROPERTIES RUN_SERIAL TRUE)
    endif()
    # B10 Step3：test_agentbridge 与 test_mainwindow 会探测全局插件目录/窗口状态，
    # 并行运行时与同进程组其他用例互相抢占会造成管道/全局状态隔离缺陷（历史 -j4 失败）。
    # 对两者设 RUN_SERIAL，使 ctest -j4 全量可稳定通过（串行于其他测试执行）。
    if(TEST_NAME STREQUAL "test_agentbridge" OR TEST_NAME STREQUAL "test_mainwindow")
        set_tests_properties(${TEST_NAME} PROPERTIES RUN_SERIAL TRUE)
    endif()
    # B3：该用例会安装并校验全部插件参数契约，插件库/全局插件目录不能与其它用例并行抢占。
    if(TEST_NAME STREQUAL "test_pluginparametercontracts")
        set_tests_properties(${TEST_NAME} PROPERTIES RUN_SERIAL TRUE)
    endif()
    if(TEST_NAME STREQUAL "test_acceptance_flows")
        set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 120)
    endif()
    if(TEST_NAME STREQUAL "test_cameramanager")
        set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 30)
    endif()
    # 并发/服务用例在 -j4 与慢机上有正常长尾，给足显式下限，避免全局 --timeout 误杀。
    if(TEST_NAME STREQUAL "test_flowexecutionservice" OR TEST_NAME STREQUAL "test_executionaffinity")
        set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 60)
    endif()
    if(TEST_NAME STREQUAL "test_mainwindow")
        set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 240)
    elseif(TEST_NAME STREQUAL "test_agent_viewport_snapshot_handoff")
        set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 120)
    endif()
    if(TEST_NAME STREQUAL "test_acceptance_flows")
        set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 60)
    endif()
    if(TEST_NAME STREQUAL "test_cameramanager")
        set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 60)
    endif()
    if(TEST_NAME STREQUAL "test_agent_folder_execution_g5")
        set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 180)
    endif()
    if(TEST_NAME STREQUAL "test_agent_plcwrite_execution_g5")
        set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 180)
    endif()
    if(TEST_NAME STREQUAL "test_agent_tcpclient_execution_g5")
        set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 180)
    endif()
    if(TEST_NAME STREQUAL "test_agent_tcpserver_execution_g5")
        set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 180)
    endif()
    if(TEST_NAME STREQUAL "test_agent_plcread_execution_g5")
        set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 180)
    endif()
    if(TEST_NAME STREQUAL "test_agent_serialport_execution_g5")
        set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 180)
    endif()
    if(TEST_NAME STREQUAL "test_agent_plccommunicate_execution_g5")
        set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 180)
    endif()
    # 阶7 批1 复核四轮：拟合含 RANSAC 循环，设 TIMEOUT 防无限循环回归挂死 CI
    if(TEST_NAME STREQUAL "test_fitellipse")
        set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 180)
    endif()
    # 阶7 批4 复核三轮（P0-1）：角度归一含超大有限数用例，设 TIMEOUT 防循环不终止挂死 CI
    if(TEST_NAME STREQUAL "test_batch4")
        set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 180)
    endif()
    # SAM 批1：测试内 HTTP 服务 + 嵌套事件循环收口，设 TIMEOUT 防挂死 CI
    if(TEST_NAME STREQUAL "test_sam_region_flow")
        set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 120)
    endif()
    # 阶段 0 基线复现：含异步运行等待轮询，设 TIMEOUT 防挂死 CI
    if(TEST_NAME STREQUAL "test_agentphase0_repro")
        set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 120)
    endif()
    # SAM 批3：Agent SAM 工具 × 测试内 HTTP 服务，同设 TIMEOUT
    if(TEST_NAME STREQUAL "test_agent_sam_tools")
        set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 120)
    endif()
    if(TEST_NAME STREQUAL "test_agent_camera_lifecycle_g5")
        set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 30)
    endif()
    # 第一迭代：LLM 重试测试包含真实指数退避等待（最长 4s）与本地假端点，设 TIMEOUT 防挂死
    if(TEST_NAME STREQUAL "test_agentretry")
        set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 120)
    endif()
    # 第二迭代下半：MCP 桥测试启动线程化假桥（QLocalServer），设 TIMEOUT 防挂死 CI
    if(TEST_NAME STREQUAL "test_mcpbridge")
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

# ========== 统一超时兜底 ==========
# 未显式设置 TIMEOUT 的用例给宽松默认，避免 CI 去掉全局 --timeout 后，
# 挂起回归只能等外部超时。显式 TIMEOUT（60/120/300 等）保持不变。
get_property(_deeplux_all_tests DIRECTORY PROPERTY TESTS)
foreach(_t IN LISTS _deeplux_all_tests)
    get_test_property(${_t} TIMEOUT _deeplux_existing_timeout)
    if(_deeplux_existing_timeout STREQUAL "NOTFOUND")
        set_tests_properties(${_t} PROPERTIES TIMEOUT 600)
    endif()
endforeach()
