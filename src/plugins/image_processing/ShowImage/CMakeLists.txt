# ShowImage Plugin

set(PLUGIN_SOURCES
    ShowImagePlugin.cpp
)

set(PLUGIN_HEADERS
    ShowImagePlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(ShowImagePlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(ShowImagePlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(ShowImagePlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(ShowImagePlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(ShowImagePlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()
