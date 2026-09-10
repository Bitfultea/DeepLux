# WriteText Plugin

set(PLUGIN_SOURCES
    WriteTextPlugin.cpp
)

set(PLUGIN_HEADERS
    WriteTextPlugin.h
)

add_library(WriteTextPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(WriteTextPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(WriteTextPlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(WriteTextPlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()