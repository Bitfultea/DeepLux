# Blob Plugin

set(PLUGIN_SOURCES
    BlobPlugin.cpp
)

set(PLUGIN_HEADERS
    BlobPlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(BlobPlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(BlobPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(BlobPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(BlobPlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(BlobPlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()