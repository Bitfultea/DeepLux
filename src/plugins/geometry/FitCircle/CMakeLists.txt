# FitCircle Plugin

set(PLUGIN_SOURCES
    FitCirclePlugin.cpp
)

set(PLUGIN_HEADERS
    FitCirclePlugin.h
)

add_library(FitCirclePlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(FitCirclePlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(FitCirclePlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(FitCirclePlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()