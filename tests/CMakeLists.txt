# DeepLux Unit Tests

set(TEST_SOURCES
    test_pathtutils.cpp
    test_modulebase.cpp
    test_mainviewmodel.cpp
    test_project.cpp
    test_configmanager.cpp
    test_pluginmanager.cpp
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
    test_toolschema.cpp
    test_agentobserver.cpp
    test_agentactor.cpp
    test_agentpermissions.cpp
    test_agentundo.cpp
    test_agentcontroller.cpp
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

    # Link FitLinePlugin for geometry tests
    target_link_libraries(${TEST_NAME} PRIVATE FitLinePlugin)
    # Link FindCirclePlugin for detection tests
    target_link_libraries(${TEST_NAME} PRIVATE FindCirclePlugin)
    # Link MeasureLinePlugin for detection tests
    target_link_libraries(${TEST_NAME} PRIVATE MeasureLinePlugin)
    # Link FitCirclePlugin for geometry tests
    target_link_libraries(${TEST_NAME} PRIVATE FitCirclePlugin)
    # Link MeasureRectPlugin for detection tests
    target_link_libraries(${TEST_NAME} PRIVATE MeasureRectPlugin)
    # Link DistancePLPlugin for geometry tests
    target_link_libraries(${TEST_NAME} PRIVATE DistancePLPlugin)
    # Link DistancePPPlugin for geometry tests
    target_link_libraries(${TEST_NAME} PRIVATE DistancePPPlugin)
    # Link LinesDistancePlugin for geometry tests
    target_link_libraries(${TEST_NAME} PRIVATE LinesDistancePlugin)

    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()
