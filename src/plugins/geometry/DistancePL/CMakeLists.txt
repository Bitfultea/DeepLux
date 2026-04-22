# DistancePL Plugin

set(PLUGIN_SOURCES
    DistancePLPlugin.cpp
)

set(PLUGIN_HEADERS
    DistancePLPlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(DistancePLPlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(DistancePLPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(DistancePLPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(DistancePLPlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(DistancePLPlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()