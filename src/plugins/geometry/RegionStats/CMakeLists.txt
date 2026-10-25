# RegionStats Plugin（SAM 批1：Region2D 消费者）

set(PLUGIN_SOURCES
    RegionStatsPlugin.cpp
)

set(PLUGIN_HEADERS
    RegionStatsPlugin.h
)

add_library(RegionStatsPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(RegionStatsPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(RegionStatsPlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(RegionStatsPlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()
