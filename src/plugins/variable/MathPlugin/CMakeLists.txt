# Math Plugin

set(PLUGIN_SOURCES
    MathPlugin.cpp
)

set(PLUGIN_HEADERS
    MathPlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(MathPlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(MathPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(MathPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
