# LinesDistance Plugin

set(PLUGIN_SOURCES
    LinesDistancePlugin.cpp
)

set(PLUGIN_HEADERS
    LinesDistancePlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(LinesDistancePlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(LinesDistancePlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(LinesDistancePlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(LinesDistancePlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(LinesDistancePlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()