# SplitString Plugin

set(PLUGIN_SOURCES
    SplitStringPlugin.cpp
)

set(PLUGIN_HEADERS
    SplitStringPlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(SplitStringPlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(SplitStringPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(SplitStringPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
