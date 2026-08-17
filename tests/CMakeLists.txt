# DeepLux Unit Tests

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
    test_agentundo.cpp
    test_agentcontroller.cpp
    test_agentbridge.cpp
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
    test_labelme_exporter.cpp
    test_yoloseg_exporter.cpp
    test_annotationoverlaywidget.cpp
    test_expressionengine.cpp
    test_samannotatordialog.cpp
    test_sambackendclient.cpp
    test_moduleinspectorpanel.cpp
    test_pluginparametercontracts.cpp
    test_acceptance_flows.cpp
    test_projectmigration.cpp
)

foreach(TEST_FILE ${TEST_SOURCES})
    get_filename_component(TEST_NAME ${TEST_FILE} NAME_WE)

    add_executable(${TEST_NAME} ${TEST_FILE})

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

    if(TEST_NAME STREQUAL "test_fitline" OR TEST_NAME STREQUAL "test_plugincontracts")
        target_link_libraries(${TEST_NAME} PRIVATE FitLinePlugin)
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
    if(TEST_NAME STREQUAL "test_loadpointcloud" OR TEST_NAME STREQUAL "test_plugincontracts")
        target_link_libraries(${TEST_NAME} PRIVATE LoadPointCloudPlugin)
    endif()
    if(TEST_NAME STREQUAL "test_clihandler" OR TEST_NAME STREQUAL "test_plugincontracts")
        target_link_libraries(${TEST_NAME} PRIVATE SystemTimePlugin)
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
        )
    endif()

    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    set_tests_properties(${TEST_NAME} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(TEST_NAME STREQUAL "test_cameramanager")
        set_tests_properties(${TEST_NAME} PROPERTIES TIMEOUT 2)
    endif()
endforeach()

add_executable(ui_capture_mainwindow ui_capture_mainwindow.cpp)
target_link_libraries(ui_capture_mainwindow
    PRIVATE
        deeplux_core
        deeplux_ui
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
