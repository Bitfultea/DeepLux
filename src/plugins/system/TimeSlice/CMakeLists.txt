# TimeSlice Plugin

set(PLUGIN_SOURCES
    TimeSlicePlugin.cpp
)

set(PLUGIN_HEADERS
    TimeSlicePlugin.h
)

add_library(TimeSlicePlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(TimeSlicePlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(TimeSlicePlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(TimeSlicePlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()