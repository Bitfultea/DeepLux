# FitCircle Plugin

set(PLUGIN_SOURCES
    FitCirclePlugin.cpp
)

set(PLUGIN_HEADERS
    FitCirclePlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(FitCirclePlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(FitCirclePlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(FitCirclePlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(FitCirclePlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(FitCirclePlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()