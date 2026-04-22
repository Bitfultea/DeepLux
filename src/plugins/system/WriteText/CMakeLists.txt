# WriteText Plugin

set(PLUGIN_SOURCES
    WriteTextPlugin.cpp
)

set(PLUGIN_HEADERS
    WriteTextPlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(WriteTextPlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(WriteTextPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(WriteTextPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(WriteTextPlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(WriteTextPlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()