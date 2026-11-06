# SAM 性能基线工具（原生推理计划·阶段0）：
# 链接 deeplux_core 走 SamBackendClient 产品路径；内嵌 stub 复用
# tests/helpers/SamTestServer.h（header-only，仅 Qt Network 依赖）。
# 不注册进 CTest（性能工具需真机/真模型语境，由复核流程手动执行）。

add_executable(sam_bench main.cpp)

target_link_libraries(sam_bench PRIVATE deeplux_core)

if(Qt6_FOUND)
    target_link_libraries(sam_bench PRIVATE Qt6::Core Qt6::Gui Qt6::Network)
else()
    target_link_libraries(sam_bench PRIVATE Qt5::Core Qt5::Gui Qt5::Network)
endif()

target_include_directories(sam_bench PRIVATE ${CMAKE_SOURCE_DIR}/tests)
