# CalculateOffset Plugin（阶7 批4：005坐标标定 重建）

set(PLUGIN_SOURCES
    CalculateOffsetPlugin.cpp
)

set(PLUGIN_HEADERS
    CalculateOffsetPlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(CalculateOffsetPlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(CalculateOffsetPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(CalculateOffsetPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(CalculateOffsetPlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(CalculateOffsetPlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()
