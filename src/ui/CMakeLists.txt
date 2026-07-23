# DeepLux UI Library

set(UI_SOURCES
    ThemeManager.cpp
    controllers/ProcessTreeController.cpp
    views/MainWindow.cpp
    views/SplashScreen.cpp
    viewmodels/MainViewModel.cpp
    widgets/HImageWidget.cpp
    widgets/FlowCanvas.cpp
    widgets/PropertyPanel.cpp
    widgets/TerminalWidget.cpp
    widgets/AgentActionLogWidget.cpp
    widgets/AnnotationOverlayWidget.cpp
    widgets/AgentChatPanel.cpp
    widgets/AgentMessageBubble.cpp
    widgets/AgentToolPreviewCard.cpp
    widgets/AppIconProvider.cpp
    widgets/ToolBox.cpp
    dialogs/AgentSettingsDialog.cpp
    widgets/ViewportWidget.cpp
    widgets/ModuleInspectorPanel.cpp
    panels/DataSourcePanel.cpp
    bridge/TerminalBridge.cpp
    process/BashProcess.cpp
    display/DisplayManager.cpp
    display/IViewportContent.cpp
    display/Viewport2DContent.cpp
    display/3d/Viewport3DContent.cpp
    display/3d/PointCloudGPUBuffer.cpp
    display/3d/CameraController.cpp
    display/3d/PointCloudRendererOpenGL.cpp
    display/3d/LODController.cpp
    display/3d/PointCloudLODBuffer.cpp
    display/3d/OctreeNode.cpp
    display/3d/PointCloudOctree.cpp
    display/3d/OctreeRenderer.cpp
    dialogs/MessageDialog.cpp
    dialogs/LoginDialog.cpp
    dialogs/AboutDialog.cpp
    dialogs/PluginTestDialog.cpp
    dialogs/SamAnnotatorDialog.cpp
    views/CameraSetView.cpp
    views/CommunicationSetView.cpp
    views/GlobalVarView.cpp
    views/SystemParamView.cpp
    resources/icons.qrc
)

# Platform-specific PTY implementation
if(LINUX)
    list(APPEND UI_SOURCES process/LinuxPtyImpl.cpp)
elseif(WIN32)
    list(APPEND UI_SOURCES process/WindowsPtyImpl.cpp)
endif()

# Terminal components (ANSI parser, renderer)
add_subdirectory(terminal)

set(UI_HEADERS
    controllers/ProcessTreeController.h
    views/MainWindow.h
    views/SplashScreen.h
    viewmodels/MainViewModel.h
    widgets/HImageWidget.h
    widgets/FlowCanvas.h
    widgets/PropertyPanel.h
    widgets/TerminalWidget.h
    widgets/AgentActionLogWidget.h
    widgets/AgentChatPanel.h
    widgets/AgentMessageBubble.h
    widgets/AgentToolPreviewCard.h
    widgets/AppIconProvider.h
    widgets/ToolBox.h
    dialogs/AgentSettingsDialog.h
    widgets/ViewportWidget.h
    widgets/ModuleInspectorPanel.h
    bridge/TerminalBridge.h
    process/BashProcess.h
    process/PtyImpl.h
    display/DisplayManager.h
    display/IViewportContent.h
    display/Viewport2DContent.h
    display/3d/Viewport3DContent.h
    display/3d/PointCloudGPUBuffer.h
    display/3d/CameraController.h
    display/3d/IPointCloudRenderer.h
    display/3d/PointCloudRendererOpenGL.h
    display/3d/LODController.h
    display/3d/PointCloudLODBuffer.h
    display/3d/OctreeNode.h
    display/3d/PointCloudOctree.h
    display/3d/OctreeRenderer.h
    dialogs/MessageDialog.h
    dialogs/LoginDialog.h
    dialogs/AboutDialog.h
    dialogs/PluginTestDialog.h
    dialogs/SamAnnotatorDialog.h
    panels/DataSourcePanel.h
    views/CameraSetView.h
    views/CommunicationSetView.h
    views/GlobalVarView.h
    views/SystemParamView.h
)

add_library(deeplux_ui SHARED ${UI_SOURCES} ${UI_HEADERS})

target_include_directories(deeplux_ui
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(deeplux_ui
    PUBLIC
        deeplux_core
        deeplux_terminal
)

if(OpenCV_FOUND)
    if(TARGET OpenCV::OpenCV)
        target_link_libraries(deeplux_ui PUBLIC OpenCV::OpenCV)
    else()
        target_link_libraries(deeplux_ui PUBLIC ${OpenCV_LIBS})
        target_include_directories(deeplux_ui PUBLIC ${OpenCV_INCLUDE_DIRS})
    endif()
endif()

if(Qt6_FOUND)
    target_link_libraries(deeplux_ui PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL)
else()
    target_link_libraries(deeplux_ui PUBLIC Qt5::Core Qt5::Gui Qt5::Widgets)
    find_package(OpenGL QUIET)
    if(OpenGL_FOUND)
        target_link_libraries(deeplux_ui PUBLIC ${OPENGL_LIBRARIES})
    endif()
endif()

set_target_properties(deeplux_ui PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    OUTPUT_NAME deeplux_ui
)

install(TARGETS deeplux_ui
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
