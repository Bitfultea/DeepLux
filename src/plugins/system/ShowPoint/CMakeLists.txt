# ShowPoint Plugin

set(PLUGIN_SOURCES
    ShowPointPlugin.cpp
)

set(PLUGIN_HEADERS
    ShowPointPlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(ShowPointPlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(ShowPointPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(ShowPointPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(ShowPointPlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(ShowPointPlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()