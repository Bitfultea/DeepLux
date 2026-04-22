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
    common/Logger.cpp
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
    agent/AgentBridge.cpp
    agent/AgentConnection.cpp
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
    engine/RunEngine.h
    device/CameraManager.h
    communication/CommunicationManager.h
    config/SystemConfig.h
    display/DisplayData.h
    display/IDisplayPort.h
    agent/AgentBridge.h
    agent/AgentConnection.h
)

add_library(deeplux_core SHARED ${CORE_SOURCES} ${CORE_HEADERS})

target_include_directories(deeplux_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

if(Qt6_FOUND)
    target_link_libraries(deeplux_core PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::SerialPort)
else()
    find_package(Qt5Widgets REQUIRED)
    find_package(Qt5Network REQUIRED)
    find_package(Qt5SerialPort REQUIRED)
    target_link_libraries(deeplux_core PUBLIC Qt5::Core Qt5::Gui Qt5::Widgets Qt5::Network Qt5::SerialPort)
endif()

if(OpenCV_FOUND)
    target_include_directories(deeplux_core PUBLIC ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(deeplux_core PUBLIC ${OpenCV_LIBS})
endif()

if(Eigen3_FOUND)
    target_include_directories(deeplux_core PUBLIC ${EIGEN3_INCLUDE_DIRS})
endif()

set_target_properties(deeplux_core PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    OUTPUT_NAME deeplux_core
)

install(TARGETS deeplux_core
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES ${CORE_HEADERS}
    DESTINATION include/deeplux/core
)
