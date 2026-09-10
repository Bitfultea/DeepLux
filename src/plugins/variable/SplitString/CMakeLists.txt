# SplitString Plugin

set(PLUGIN_SOURCES
    SplitStringPlugin.cpp
)

set(PLUGIN_HEADERS
    SplitStringPlugin.h
)

add_library(SplitStringPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(SplitStringPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
