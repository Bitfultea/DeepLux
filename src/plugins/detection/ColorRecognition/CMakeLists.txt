# ColorRecognition Plugin

set(PLUGIN_SOURCES
    ColorRecognitionPlugin.cpp
)

set(PLUGIN_HEADERS
    ColorRecognitionPlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(ColorRecognitionPlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(ColorRecognitionPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(ColorRecognitionPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(ColorRecognitionPlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(ColorRecognitionPlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()