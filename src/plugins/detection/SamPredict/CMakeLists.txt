# SamPredict Plugin（SAM 批1：Region2D 生产者）

set(PLUGIN_SOURCES
    SamPredictPlugin.cpp
)

set(PLUGIN_HEADERS
    SamPredictPlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(SamPredictPlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(SamPredictPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(SamPredictPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network)

if(OpenCV_FOUND)
    target_link_libraries(SamPredictPlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(SamPredictPlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()
