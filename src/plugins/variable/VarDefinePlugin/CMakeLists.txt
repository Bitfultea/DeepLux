# VarDefinePlugin

set(PLUGIN_SOURCES
    VarDefinePlugin.cpp
)

set(PLUGIN_HEADERS
    VarDefinePlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(VarDefinePlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(VarDefinePlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(VarDefinePlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
