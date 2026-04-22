# Delay Plugin

set(PLUGIN_SOURCES
    DelayPlugin.cpp
)

set(PLUGIN_HEADERS
    DelayPlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(DelayPlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(DelayPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(DelayPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
