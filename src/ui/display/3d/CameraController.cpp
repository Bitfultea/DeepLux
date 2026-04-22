#include "CameraController.h"
#include <cmath>

namespace DeepLux {

CameraController::CameraController()
    : m_center(0, 0, 0)
    , m_eye(0, 0, 5)
    , m_up(0, 1, 0)
    , m_fov(45.0f)
    , m_near(0.1f)
    , m_far(1000.0f)
    , m_azimuth(0.0f)
    , m_elevation(0.0f)
    , m_distance(5.0f)
{
}

void CameraController::orbit(float deltaAzimuth, float deltaElevation) {
    m_azimuth += deltaAzimuth;
    m_elevation += deltaElevation;

    // 限制仰角避免万向锁
    const float PI = static_cast<float>(M_PI);
    m_elevation = std::max(-PI / 2.0f + 0.01f, std::min(PI / 2.0f - 0.01f, m_elevation));

    updateEyeFromAngles();
}

void CameraController::pan(float deltaX, float deltaY) {
    // 计算相机的右向量和上向量
    QVector3D forward = (m_center - m_eye).normalized();
    QVector3D right = QVector3D::crossProduct(forward, m_up).normalized();
    QVector3D up = QVector3D::crossProduct(right, forward).normalized();

    // 在视平面内平移
    float scale = m_distance * 0.002f;  // 缩放因子
    QVector3D offset = right * deltaX * scale + up * deltaY * scale;
    m_center += offset;
    m_eye += offset;
}

void CameraController::zoom(float delta) {
    m_distance *= (1.0f - delta * 0.1f);
    m_distance = std::max(0.1f, std::min(1000.0f, m_distance));
    updateEyeFromAngles();
}

void CameraController::reset() {
    m_azimuth = 0.0f;
    m_elevation = 0.0f;
    m_distance = 5.0f;
    m_center = QVector3D(0, 0, 0);
    updateEyeFromAngles();
}

void CameraController::setTarget(const QVector3D& center) {
    m_center = center;
    updateEyeFromAngles();
}

float CameraController::distance() const {
    return m_distance;
}

QMatrix4x4 CameraController::viewMatrix() const {
    QMatrix4x4 view;
    view.lookAt(m_eye, m_center, m_up);
    return view;
}

QMatrix4x4 CameraController::projectionMatrix(float aspectRatio) const {
    QMatrix4x4 projection;
    projection.perspective(m_fov, aspectRatio, m_near, m_far);
    return projection;
}

void CameraController::updateEyeFromAngles() {
    float x = m_distance * std::cos(m_elevation) * std::sin(m_azimuth);
    float y = m_distance * std::sin(m_elevation);
    float z = m_distance * std::cos(m_elevation) * std::cos(m_azimuth);

    m_eye = QVector3D(x, y, z) + m_center;

    // 保持 up 向量大致向上
    if (std::abs(m_elevation) > M_PI / 2.0f - 0.1f) {
        m_up = QVector3D(0, 1, 0);
    } else {
        m_up = QVector3D(0, 1, 0);
    }
}

} // namespace DeepLux
