# FitLine Plugin

set(PLUGIN_SOURCES
    FitLinePlugin.cpp
)

set(PLUGIN_HEADERS
    FitLinePlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(FitLinePlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(FitLinePlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(FitLinePlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(FitLinePlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(FitLinePlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()
