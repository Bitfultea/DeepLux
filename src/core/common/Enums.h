#pragma once

#include <QObject>

namespace DeepLux {

/**
 * @brief 项目状态
 */
enum class ProjectState {
    Idle,           // 空闲
    Running,        // 运行中
    Paused,         // 暂停
    Error           // 错误
};

/**
 * @brief 图像格式
 */
enum class ImageFormat {
    Unknown,
    Grayscale8,     // 8位灰度
    Grayscale16,    // 16位灰度
    RGB24,          // RGB 24位
    RGBA32,         // RGBA 32位
    BGR24,          // BGR 24位
    BGRA32          // BGRA 32位
};

/**
 * @brief 相机状态
 */
enum class CameraState {
    Disconnected,   // 未连接
    Connected,      // 已连接
    Acquiring,      // 采集中
    Error           // 错误
};

/**
 * @brief 运动状态
 */
enum class MotionState {
    Idle,           // 空闲
    Moving,         // 运动中
    Homing,         // 回零中
    Stopped,        // 停止
    Error           // 错误
};

/**
 * @brief 通讯类型
 */
enum class CommunicationType {
    TCP,
    UDP,
    Serial,
    ModbusTCP,
    ModbusRTU
};

} // namespace DeepLux
