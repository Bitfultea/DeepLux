# MeasurementInput Plugin

set(PLUGIN_SOURCES
    MeasurementInputPlugin.cpp
)

set(PLUGIN_HEADERS
    MeasurementInputPlugin.h
)

add_library(MeasurementInputPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(MeasurementInputPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
