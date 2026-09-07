# FitEllipse Plugin

set(PLUGIN_SOURCES
    FitEllipsePlugin.cpp
)

set(PLUGIN_HEADERS
    FitEllipsePlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(FitEllipsePlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(FitEllipsePlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(FitEllipsePlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(FitEllipsePlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(FitEllipsePlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()
