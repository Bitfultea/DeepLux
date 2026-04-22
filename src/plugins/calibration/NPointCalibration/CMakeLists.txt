# NPointCalibration Plugin

set(PLUGIN_SOURCES
    NPointCalibrationPlugin.cpp
)

set(PLUGIN_HEADERS
    NPointCalibrationPlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(NPointCalibrationPlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(NPointCalibrationPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(NPointCalibrationPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(NPointCalibrationPlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(NPointCalibrationPlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()
