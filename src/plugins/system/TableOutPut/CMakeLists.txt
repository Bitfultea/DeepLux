# TableOutPut Plugin

set(PLUGIN_SOURCES
    TableOutPutPlugin.cpp
)

set(PLUGIN_HEADERS
    TableOutPutPlugin.h
)

add_library(TableOutPutPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(TableOutPutPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(TableOutPutPlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(TableOutPutPlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()