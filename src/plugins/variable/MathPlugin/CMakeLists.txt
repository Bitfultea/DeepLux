# Math Plugin

set(PLUGIN_SOURCES
    MathPlugin.cpp
)

set(PLUGIN_HEADERS
    MathPlugin.h
)

add_library(MathPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(MathPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
