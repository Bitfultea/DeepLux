cmake_minimum_required(VERSION 3.14)

if(UNIX AND NOT APPLE)
    add_subdirectory(V4L2Camera)
    if(ENABLE_CAMERA_HIKVISION)
        add_subdirectory(HikvisionCamera)
    endif()
elseif(WIN32)
    add_subdirectory(DirectShowCamera)
    if(ENABLE_CAMERA_HIKVISION)
        # Windows 版 MVS SDK 的目录结构（MVS/Development/Libraries/win64）与
        # 插件的 Linux 构建脚本尚未整合，明确提示而不是静默跳过。
        message(STATUS
            "HikvisionCamera: ENABLE_CAMERA_HIKVISION=ON is currently Linux-only; "
            "using DirectShowCamera on Windows. Windows MVS SDK integration is pending.")
    endif()
endif()
