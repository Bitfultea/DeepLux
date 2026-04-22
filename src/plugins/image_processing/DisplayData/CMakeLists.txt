# DisplayData Plugin

set(PLUGIN_SOURCES
    DisplayDataPlugin.cpp
)

set(PLUGIN_HEADERS
    DisplayDataPlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(DisplayDataPlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(DisplayDataPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(DisplayDataPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(DisplayDataPlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(DisplayDataPlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()