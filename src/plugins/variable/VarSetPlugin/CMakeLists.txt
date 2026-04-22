# VarSetPlugin

set(PLUGIN_SOURCES
    VarSetPlugin.cpp
)

set(PLUGIN_HEADERS
    VarSetPlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(VarSetPlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(VarSetPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(VarSetPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
