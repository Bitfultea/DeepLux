# TimeSlice Plugin

set(PLUGIN_SOURCES
    TimeSlicePlugin.cpp
)

set(PLUGIN_HEADERS
    TimeSlicePlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(TimeSlicePlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(TimeSlicePlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(TimeSlicePlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(TimeSlicePlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(TimeSlicePlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()