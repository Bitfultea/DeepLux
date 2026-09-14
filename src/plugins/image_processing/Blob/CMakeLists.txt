# Blob Plugin

set(PLUGIN_SOURCES
    BlobPlugin.cpp
)

set(PLUGIN_HEADERS
    BlobPlugin.h
)

add_library(BlobPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(BlobPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(BlobPlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(BlobPlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()