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
    deeplux/DataContract.cpp
    platform/PathUtils.cpp
    model/Project.cpp
    model/ProjectMigrator.cpp
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
    io/CocoExporter.cpp
    io/LabelMeImporter.cpp
    geometry/MeasurementData.cpp
    model/Annotation.cpp
    model/AnnotationCoordinateMapping.cpp
    model/AnnotationMaskOps.cpp
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
    agent/SamPredictor.cpp
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
    agent/SamPredictor.h
    io/PlyLoader.h
    io/TiffLoader.h
    io/LabelMeExporter.h
    io/YoloSegExporter.h
    io/CocoExporter.h
    io/LabelMeImporter.h
    geometry/MeasurementData.h
    model/Annotation.h
    model/AnnotationCoordinateMapping.h
    model/AnnotationMaskOps.h
)

add_library(deeplux_agent_resources STATIC agent/agent_resources.qrc)
set_target_properties(deeplux_agent_resources PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(deeplux_core SHARED ${CORE_SOURCES} ${CORE_HEADERS})

target_compile_definitions(deeplux_core PRIVATE DEEPLUX_CORE_EXPORT)

# ========== 阶段2：原生 SAM 推理（onnxruntime，可选）==========
option(DEEPLUX_NATIVE_SAM "Enable native SAM ONNX inference (requires onnxruntime)" OFF)
if(DEEPLUX_NATIVE_SAM)
    if(NOT ONNXRUNTIME_DIR AND DEFINED ENV{ONNXRUNTIME_DIR})
        set(ONNXRUNTIME_DIR "$ENV{ONNXRUNTIME_DIR}" CACHE PATH "onnxruntime install dir")
    endif()
    if(NOT EXISTS "${ONNXRUNTIME_DIR}/include/onnxruntime_c_api.h")
        message(FATAL_ERROR "DEEPLUX_NATIVE_SAM=ON 需要 -DONNXRUNTIME_DIR=<SDK 目录>（缺少 include/onnxruntime_c_api.h）")
    endif()
    target_sources(deeplux_core PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/inference/SamOnnxInference.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/inference/SamOnnxInference.h)
    target_include_directories(deeplux_core PRIVATE ${ONNXRUNTIME_DIR}/include)
    if(WIN32)
        set(_ort_dll_dir "${ONNXRUNTIME_DIR}/lib")
        if(NOT EXISTS "${_ort_dll_dir}/onnxruntime.dll")
            set(_ort_dll_dir "${ONNXRUNTIME_DIR}/bin")
        endif()
        if(NOT EXISTS "${ONNXRUNTIME_DIR}/lib/onnxruntime.lib" OR
           NOT EXISTS "${_ort_dll_dir}/onnxruntime.dll")
            message(FATAL_ERROR "ONNXRUNTIME_DIR 缺少 Windows onnxruntime.lib 或 onnxruntime.dll")
        endif()
        target_link_libraries(deeplux_core PRIVATE "${ONNXRUNTIME_DIR}/lib/onnxruntime.lib")
        file(GLOB _ort_runtime_dlls "${_ort_dll_dir}/onnxruntime*.dll")
        install(FILES ${_ort_runtime_dlls} DESTINATION ${CMAKE_INSTALL_BINDIR})
    elseif(UNIX AND NOT APPLE)
        if(NOT EXISTS "${ONNXRUNTIME_DIR}/lib/libonnxruntime.so")
            message(FATAL_ERROR "ONNXRUNTIME_DIR 缺少 lib/libonnxruntime.so")
        endif()
        target_link_libraries(deeplux_core PRIVATE "${ONNXRUNTIME_DIR}/lib/libonnxruntime.so")
        file(GLOB _ort_runtime_files "${ONNXRUNTIME_DIR}/lib/libonnxruntime.so*"
                                     "${ONNXRUNTIME_DIR}/lib/libonnxruntime_providers_*.so")
        set(_ort_real_files)
        set(_ort_links)
        foreach(_ort_file IN LISTS _ort_runtime_files)
            get_filename_component(_ort_real "${_ort_file}" REALPATH)
            get_filename_component(_ort_name "${_ort_file}" NAME)
            get_filename_component(_ort_real_name "${_ort_real}" NAME)
            list(APPEND _ort_real_files "${_ort_real}")
            if(NOT _ort_name STREQUAL _ort_real_name)
                list(APPEND _ort_links "${_ort_name}|${_ort_real_name}")
            endif()
        endforeach()
        list(REMOVE_DUPLICATES _ort_real_files)
        install(FILES ${_ort_real_files} DESTINATION ${CMAKE_INSTALL_LIBDIR})
        foreach(_ort_link IN LISTS _ort_links)
            string(REPLACE "|" ";" _ort_pair "${_ort_link}")
            list(GET _ort_pair 0 _ort_name)
            list(GET _ort_pair 1 _ort_real_name)
            install(CODE "file(CREATE_LINK \"${_ort_real_name}\" \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/${_ort_name}\" SYMBOLIC)\nlist(APPEND CMAKE_INSTALL_MANIFEST_FILES \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/${_ort_name}\")")
        endforeach()
        set_target_properties(deeplux_core PROPERTIES INSTALL_RPATH "$ORIGIN")
    else()
        message(FATAL_ERROR "原生 SAM 的 ONNX Runtime 安装规则尚不支持此平台")
    endif()
    target_compile_definitions(deeplux_core PUBLIC DEEPLUX_HAS_ONNXRUNTIME)
    message(STATUS "Native SAM inference: enabled (onnxruntime at ${ONNXRUNTIME_DIR})")
endif()

target_include_directories(deeplux_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# Platform define propagated to all downstream targets
target_compile_definitions(deeplux_core PUBLIC ${DEEPLUX_PLATFORM_DEFINE})

if(Qt6_FOUND)
    target_link_libraries(deeplux_agent_resources PRIVATE Qt6::Core)
    target_link_libraries(deeplux_core PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::SerialPort Qt6::Concurrent)
else()
    find_package(Qt5Widgets REQUIRED)
    find_package(Qt5Network REQUIRED)
    target_link_libraries(deeplux_agent_resources PRIVATE Qt5::Core)
    target_link_libraries(deeplux_core PUBLIC Qt5::Core Qt5::Gui Qt5::Widgets Qt5::Network Qt5::SerialPort Qt5::Concurrent)
endif()
target_link_libraries(deeplux_core PRIVATE deeplux_agent_resources)

# 阶段 3：OpenCV 为必需依赖（根 CMakeLists 中 find_package 失败即中止），此处恒成立。
target_compile_definitions(deeplux_core PUBLIC DEEPLUX_HAS_OPENCV)
if(TARGET OpenCV::OpenCV)
    target_link_libraries(deeplux_core PUBLIC OpenCV::OpenCV)
else()
    target_include_directories(deeplux_core PUBLIC ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(deeplux_core PUBLIC ${OpenCV_LIBS})
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
