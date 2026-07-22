# DeepLux Core Library

# Find Eigen3 for 3D math
find_package(Eigen3 QUIET)
if(Eigen3_FOUND)
    message(STATUS "Eigen3: Found (version: ${EIGEN3_VERSION})")
else()
    message(STATUS "Eigen3: Not found (3D features will be limited)")
endif()

set(CORE_SOURCES
    base/ModuleBase.cpp
    platform/PathUtils.cpp
    model/Project.cpp
    manager/ProjectManager.cpp
    manager/ConfigManager.cpp
    manager/PluginManager.cpp
    manager/GlobalVarManager.cpp
    model/ImageData.cpp
    model/DataSource.cpp
    common/Logger.cpp
    common/ModuleIconProvider.cpp
    common/UserManager.cpp
    common/ShortcutManager.cpp
    common/VarModel.cpp
    common/ExpressionEngine.cpp
    common/CancellationToken.cpp
    common/CLIHandler.cpp
    engine/RunEngine.cpp
    device/CameraManager.cpp
    communication/CommunicationManager.cpp
    config/SystemConfig.cpp
    display/DisplayData.cpp
    io/PlyLoader.cpp
    io/TiffLoader.cpp
    io/LabelMeExporter.cpp
    io/YoloSegExporter.cpp
    geometry/MeasurementData.cpp
    model/Annotation.cpp
    model/AnnotationCoordinateMapping.cpp
    agent/AgentBridge.cpp
    agent/AgentConnection.cpp
    agent/AgentController.cpp
    agent/AgentObserver.cpp
    agent/AgentActor.cpp
    agent/ToolSchema.cpp
    agent/ILLMClient.cpp
    agent/OpenAILLMClient.cpp
    agent/GuiEvent.cpp
    agent/SamBackendClient.cpp
    agent/agent_resources.qrc
)

set(CORE_HEADERS
    interface/IModule.h
    interface/ICamera.h
    interface/IMotion.h
    interface/ICameraPlugin.h
    base/ModuleBase.h
    platform/Platform.h
    platform/PathUtils.h
    common/Types.h
    common/Enums.h
    common/Logger.h
    common/ModuleIconProvider.h
    common/UserManager.h
    common/ShortcutManager.h
    common/VarModel.h
    common/ExpressionEngine.h
    common/CancellationToken.h
    common/CLIHandler.h
    model/Project.h
    manager/ProjectManager.h
    manager/ConfigManager.h
    manager/PluginManager.h
    manager/GlobalVarManager.h
    model/ImageData.h
    model/DataSource.h
    engine/RunEngine.h
    device/CameraManager.h
    communication/CommunicationManager.h
    config/SystemConfig.h
    display/DisplayData.h
    display/IDisplayPort.h
    agent/AgentBridge.h
    agent/AgentConnection.h
    agent/AgentController.h
    agent/AgentObserver.h
    agent/AgentActor.h
    agent/ToolSchema.h
    agent/ILLMClient.h
    agent/OpenAILLMClient.h
    agent/GuiEvent.h
    agent/SamBackendClient.h
    io/PlyLoader.h
    io/TiffLoader.h
    io/LabelMeExporter.h
    io/YoloSegExporter.h
    geometry/MeasurementData.h
    model/Annotation.h
    model/AnnotationCoordinateMapping.h
)

add_library(deeplux_core SHARED ${CORE_SOURCES} ${CORE_HEADERS})

target_include_directories(deeplux_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# Platform define propagated to all downstream targets
target_compile_definitions(deeplux_core PUBLIC ${DEEPLUX_PLATFORM_DEFINE})

if(Qt6_FOUND)
    target_link_libraries(deeplux_core PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::SerialPort Qt6::Concurrent)
else()
    find_package(Qt5Widgets REQUIRED)
    find_package(Qt5Network REQUIRED)
    target_link_libraries(deeplux_core PUBLIC Qt5::Core Qt5::Gui Qt5::Widgets Qt5::Network Qt5::SerialPort Qt5::Concurrent)
endif()

if(OpenCV_FOUND)
    target_compile_definitions(deeplux_core PUBLIC DEEPLUX_HAS_OPENCV)
    if(TARGET OpenCV::OpenCV)
        target_link_libraries(deeplux_core PUBLIC OpenCV::OpenCV)
    else()
        target_include_directories(deeplux_core PUBLIC ${OpenCV_INCLUDE_DIRS})
        target_link_libraries(deeplux_core PUBLIC ${OpenCV_LIBS})
    endif()
endif()

if(Eigen3_FOUND)
    if(TARGET Eigen3::Eigen)
        target_link_libraries(deeplux_core PUBLIC Eigen3::Eigen)
    else()
        target_include_directories(deeplux_core PUBLIC ${EIGEN3_INCLUDE_DIRS})
    endif()
endif()

set_target_properties(deeplux_core PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    OUTPUT_NAME deeplux_core
)

install(TARGETS deeplux_core
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES ${CORE_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/deeplux/core
)
