#pragma once

#include <QVector3D>
#include <QMatrix4x4>

namespace DeepLux {

/**
 * @brief 相机控制器（纯数学类）
 *
 * 不继承 QObject，无 Qt 依赖，可单元测试
 * 负责：
 * - 轨道相机模型（orbit）
 * - 视图矩阵计算
 * - 投影矩阵计算
 */
class CameraController {
public:
    CameraController();

    /**
     * @brief 轨道旋转
     * @param deltaAzimuth 水平角度变化（弧度）
     * @param deltaElevation 垂直角度变化（弧度）
     */
    void orbit(float deltaAzimuth, float deltaElevation);

    /**
     * @brief 平移
     * @param deltaX X 方向变化
     * @param deltaY Y 方向变化
     */
    void pan(float deltaX, float deltaY);

    /**
     * @brief 缩放
     * @param delta 缩放增量（正值放大，负值缩小）
     */
    void zoom(float delta);

    /**
     * @brief 重置相机到初始位置
     */
    void reset();

    /**
     * @brief 设置目标中心点
     */
    void setTarget(const QVector3D& center);

    /**
     * @brief 获取视点位置
     */
    QVector3D eye() const { return m_eye; }

    /**
     * @brief 获取目标中心
     */
    QVector3D center() const { return m_center; }

    /**
     * @brief 获取上向量
     */
    QVector3D up() const { return m_up; }

    /**
     * @brief 获取相机到目标距离
     */
    float distance() const;

    /**
     * @brief 获取视图矩阵
     */
    QMatrix4x4 viewMatrix() const;

    /**
     * @brief 获取投影矩阵
     * @param aspectRatio 宽高比
     */
    QMatrix4x4 projectionMatrix(float aspectRatio) const;

    /**
     * @brief 设置视场角
     */
    void setFOV(float fov) { m_fov = fov; }

    /**
     * @brief 获取视场角
     */
    float fov() const { return m_fov; }

    /**
     * @brief 设置近平面
     */
    void setNearPlane(float near) { m_near = near; }

    /**
     * @brief 设置远平面
     */
    void setFarPlane(float far) { m_far = far; }

private:
    void updateEyeFromAngles();

    QVector3D m_center{0, 0, 0};     // 观察目标中心
    QVector3D m_eye{0, 0, 5};        // 相机位置
    QVector3D m_up{0, 1, 0};         // 上向量

    float m_fov = 45.0f;             // 视场角（度）
    float m_near = 0.1f;             // 近平面
    float m_far = 1000.0f;           // 远平面

    float m_azimuth = 0.0f;          // 水平角度（弧度）
    float m_elevation = 0.0f;        // 垂直角度（弧度）
    float m_distance = 5.0f;          // 到目标距离
};

} // namespace DeepLux
