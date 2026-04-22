# GrabImage Plugin

set(PLUGIN_SOURCES
    GrabImagePlugin.cpp
)

set(PLUGIN_HEADERS
    GrabImagePlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(GrabImagePlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(GrabImagePlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(GrabImagePlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(GrabImagePlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(GrabImagePlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()
