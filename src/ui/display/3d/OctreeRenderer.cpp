#include "OctreeRenderer.h"
#include "CameraController.h"
#include "PointCloudRendererOpenGL.h"
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QMatrix4x4>
#include <QDebug>
#include <cmath>

namespace DeepLux {

// 顶点着色器
const char* const OCTREE_VERTEX_SHADER = R"(
    #version 130
    attribute vec3 aPosition;
    attribute vec3 aColor;

    uniform mat4 uViewMatrix;
    uniform mat4 uProjectionMatrix;
    uniform float uPointSize;

    varying vec3 vColor;

    void main() {
        gl_Position = uProjectionMatrix * uViewMatrix * vec4(aPosition, 1.0);
        gl_PointSize = uPointSize;
        vColor = aColor;
    }
)";

// 片段着色器
const char* const OCTREE_FRAGMENT_SHADER = R"(
    #version 130
    varying vec3 vColor;

    void main() {
        gl_FragColor = vec4(vColor, 1.0);
    }
)";

OctreeRenderer::OctreeRenderer()
{
}

OctreeRenderer::~OctreeRenderer() {
    clear();
}

void OctreeRenderer::initializeGL() {
    if (m_initialized) return;

    QOpenGLFunctions* f = QOpenGLContext::currentContext()->functions();
    f->initializeOpenGLFunctions();

    // 创建着色器程序
    m_shaderProgram = new QOpenGLShaderProgram;
    m_shaderProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, OCTREE_VERTEX_SHADER);
    m_shaderProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, OCTREE_FRAGMENT_SHADER);
    m_shaderProgram->link();

    m_initialized = true;
}

void OctreeRenderer::setOctree(std::shared_ptr<PointCloudOctree> octree) {
    if (m_octree != octree) {
        clear();
        m_octree = octree;
    }
}

void OctreeRenderer::setCamera(const CameraController* camera) {
    if (!camera) return;

    m_cameraPosition = camera->eye();
}

void OctreeRenderer::setCameraMatrices(const QMatrix4x4& viewMatrix, const QMatrix4x4& projectionMatrix) {
    m_viewMatrix = viewMatrix;
    m_projectionMatrix = projectionMatrix;
    m_cameraPosition = -viewMatrix.inverted().column(3).toVector3D();

    extractFrustumPlanes(viewMatrix, projectionMatrix);
}

void OctreeRenderer::extractFrustumPlanes(const QMatrix4x4& viewMatrix, const QMatrix4x4& projectionMatrix) {
    // 从视投影矩阵提取视锥 6 个平面
    QMatrix4x4 vp = projectionMatrix * viewMatrix;

    // 列主序 OpenGL 矩阵: vp(i,j) = vp(row=i, col=j)
    // 左平面: vp(0,0) + vp(3,0)
    // 右平面: -vp(0,0) + vp(3,0)
    // 下平面: vp(1,1) + vp(3,1)
    // 上平面: -vp(1,1) + vp(3,1)
    // 近平面: vp(2,2) + vp(3,2)
    // 远平面: -vp(2,2) + vp(3,2)

    m_frustumPlanes.resize(6);

    // 左平面
    m_frustumPlanes[0].normal = QVector3D(vp(0, 0) + vp(3, 0), vp(1, 0) + vp(3, 0), vp(2, 0) + vp(3, 0));
    m_frustumPlanes[0].d = vp(3, 3);

    // 右平面
    m_frustumPlanes[1].normal = QVector3D(-vp(0, 0) + vp(3, 0), -vp(1, 0) + vp(3, 0), -vp(2, 0) + vp(3, 0));
    m_frustumPlanes[1].d = vp(3, 3);

    // 下平面
    m_frustumPlanes[2].normal = QVector3D(vp(0, 1) + vp(3, 1), vp(1, 1) + vp(3, 1), vp(2, 1) + vp(3, 1));
    m_frustumPlanes[2].d = vp(3, 3);

    // 上平面
    m_frustumPlanes[3].normal = QVector3D(-vp(0, 1) + vp(3, 1), -vp(1, 1) + vp(3, 1), -vp(2, 1) + vp(3, 1));
    m_frustumPlanes[3].d = vp(3, 3);

    // 近平面
    m_frustumPlanes[4].normal = QVector3D(vp(0, 2), vp(1, 2), vp(2, 2));
    m_frustumPlanes[4].d = vp(3, 2);

    // 远平面
    m_frustumPlanes[5].normal = QVector3D(-vp(0, 2) + vp(3, 2), -vp(1, 2) + vp(3, 2), -vp(2, 2) + vp(3, 2));
    m_frustumPlanes[5].d = -vp(3, 3);

    // 归一化平面
    for (auto& plane : m_frustumPlanes) {
        float len = plane.normal.length();
        if (len > 1e-6f) {
            plane.normal /= len;
            plane.d /= len;
        }
    }
}

void OctreeRenderer::clear() {
    // 删除所有 VBO
    QOpenGLFunctions* f = nullptr;
    if (QOpenGLContext::currentContext()) {
        f = QOpenGLContext::currentContext()->functions();
    }

    for (auto& entry : m_vboCache) {
        if (f) {
            if (entry.second.vboPositions) {
                f->glDeleteBuffers(1, &entry.second.vboPositions);
            }
            if (entry.second.vboColors) {
                f->glDeleteBuffers(1, &entry.second.vboColors);
            }
            if (entry.second.vboNormals) {
                f->glDeleteBuffers(1, &entry.second.vboNormals);
            }
        }
    }
    m_vboCache.clear();

    // 删除着色器
    if (m_shaderProgram) {
        delete m_shaderProgram;
        m_shaderProgram = nullptr;
    }

    m_initialized = false;
}

int OctreeRenderer::selectLODLevel() {
    if (!m_lodEnabled || !m_octree) {
        m_useLOD = false;
        return m_maxLODDepth;
    }

    // 检查 LOD 阈值
    if (!m_octree->shouldUseLOD()) {
        m_useLOD = false;
        return m_maxLODDepth;
    }

    m_useLOD = true;

    // 基于距离的 LOD 选择
    float distance = (m_cameraPosition - m_octree->center()).length();

    // 距离阈值分级
    // 级别 0: 完全不细分
    // 级别 1: distance > size * 4
    // 级别 2: distance > size * 2
    // 级别 3: distance > size
    // 级别 4+: 越来越近

    float size = m_octree->size();

    if (distance > size * 8) {
        m_currentLODLevel = 1;
    } else if (distance > size * 4) {
        m_currentLODLevel = 2;
    } else if (distance > size * 2) {
        m_currentLODLevel = 3;
    } else if (distance > size) {
        m_currentLODLevel = 4;
    } else {
        m_currentLODLevel = 5;
    }

    // 限制最大深度
    return std::min(m_currentLODLevel, m_maxLODDepth);
}

void OctreeRenderer::updateVisibleNodes() {
    m_visibleLeaves.clear();

    if (!m_octree || !m_octree->root()) {
        return;
    }

    int targetDepth = selectLODLevel();

    std::function<void(OctreeNode*, const OctreeNodeInfo&)> traverse;
    traverse = [&](OctreeNode* node, const OctreeNodeInfo& info) {
        if (!node) return;

        // 视锥裁剪测试
        if (!m_frustumPlanes.empty()) {
            if (!isInFrustum(info.center(), info.size)) {
                return;
            }
        }

        if (node->isLeaf()) {
            m_visibleLeaves.push_back(static_cast<OctreeLeafNode*>(node));
            return;
        }

        auto* internal = static_cast<OctreeInternalNode*>(node);

        // LOD 决策
        if (info.depth >= static_cast<size_t>(targetDepth)) {
            // 达到目标深度，收集所有叶子
            for (auto& child : internal->children) {
                if (child && child->isLeaf()) {
                    m_visibleLeaves.push_back(static_cast<OctreeLeafNode*>(child.get()));
                }
            }
            return;
        }

        // 继续向下遍历
        float childSize = info.size * 0.5f;

        for (int i = 0; i < 8; ++i) {
            if (!internal->children[i]) continue;

            OctreeNodeInfo childInfo;
            childInfo.depth = info.depth + 1;
            childInfo.child_index = i;
            childInfo.size = childSize;
            childInfo.origin = QVector3D(
                info.origin.x() + ((i & 1) ? childSize : 0),
                info.origin.y() + ((i & 2) ? childSize : 0),
                info.origin.z() + ((i & 4) ? childSize : 0)
            );

            traverse(internal->children[i].get(), childInfo);
        }
    };

    OctreeNodeInfo rootInfo;
    rootInfo.origin = m_octree->origin();
    rootInfo.size = m_octree->size();
    rootInfo.depth = 0;
    traverse(m_octree->root().get(), rootInfo);

    m_visibleNodeCount = m_visibleLeaves.size();
}

bool OctreeRenderer::isInFrustum(const QVector3D& center, float size) const {
    if (m_frustumPlanes.empty()) {
        return true;  // 没有视锥数据时默认可见
    }

    // 简单的球体-立方体相交测试
    float halfSize = size * 0.5f * std::sqrt(3.0f);  // 立方体内接球半径

    for (const auto& plane : m_frustumPlanes) {
        float dist = QVector3D::dotProduct(plane.normal, center) + plane.d;
        if (dist < -halfSize) {
            return false;  // 完全在平面外
        }
    }
    return true;
}

bool OctreeRenderer::isSphereInFrustum(const QVector3D& center, float radius) const {
    if (m_frustumPlanes.empty()) {
        return true;
    }

    for (const auto& plane : m_frustumPlanes) {
        float dist = QVector3D::dotProduct(plane.normal, center) + plane.d;
        if (dist < -radius) {
            return false;
        }
    }
    return true;
}

void OctreeRenderer::uploadDirtyBuffers() {
    if (!m_initialized) return;

    QOpenGLFunctions* f = QOpenGLContext::currentContext()->functions();

    for (auto* leaf : m_visibleLeaves) {
        if (!leaf || !leaf->gpuBuffer) continue;

        VBOEntry& entry = m_vboCache[leaf];

        if (!entry.dirty && !leaf->dirty) continue;

        // 上传位置数据
        if (leaf->gpuBuffer->dirty || entry.vboPositions == 0) {
            if (entry.vboPositions == 0) {
                f->glGenBuffers(1, &entry.vboPositions);
            }

            f->glBindBuffer(GL_ARRAY_BUFFER, entry.vboPositions);
            f->glBufferData(GL_ARRAY_BUFFER,
                          leaf->gpuBuffer->positions.size() * sizeof(float),
                          leaf->gpuBuffer->positions.data(),
                          GL_STATIC_DRAW);
        }

        // 上传颜色数据
        if (leaf->gpuBuffer->hasColors() && (leaf->gpuBuffer->dirty || entry.vboColors == 0)) {
            if (entry.vboColors == 0) {
                f->glGenBuffers(1, &entry.vboColors);
            }

            f->glBindBuffer(GL_ARRAY_BUFFER, entry.vboColors);
            f->glBufferData(GL_ARRAY_BUFFER,
                          leaf->gpuBuffer->colors.size() * sizeof(float),
                          leaf->gpuBuffer->colors.data(),
                          GL_STATIC_DRAW);
        }

        entry.pointCount = leaf->gpuBuffer->pointCount();
        entry.dirty = false;
        leaf->dirty = false;
    }
}

void OctreeRenderer::renderNodes() {
    if (!m_initialized || !m_shaderProgram) return;

    QOpenGLFunctions* f = QOpenGLContext::currentContext()->functions();

    m_renderedPointCount = 0;

    m_shaderProgram->bind();

    for (auto* leaf : m_visibleLeaves) {
        if (!leaf) continue;

        VBOEntry& entry = m_vboCache[leaf];
        if (entry.vboPositions == 0 || entry.pointCount == 0) continue;

        // 绑定位置
        f->glBindBuffer(GL_ARRAY_BUFFER, entry.vboPositions);
        GLint posLoc = m_shaderProgram->attributeLocation("aPosition");
        if (posLoc >= 0) {
            f->glEnableVertexAttribArray(posLoc);
            f->glVertexAttribPointer(posLoc, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        }

        // 绑定颜色
        if (entry.vboColors != 0) {
            f->glBindBuffer(GL_ARRAY_BUFFER, entry.vboColors);
            GLint colLoc = m_shaderProgram->attributeLocation("aColor");
            if (colLoc >= 0) {
                f->glEnableVertexAttribArray(colLoc);
                f->glVertexAttribPointer(colLoc, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
            }
        } else {
            // 使用统一颜色
            m_shaderProgram->setAttributeValue("aColor",
                QVector3D(m_uniformColor.redF(),
                         m_uniformColor.greenF(),
                         m_uniformColor.blueF()));
        }

        // 绘制
        f->glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(entry.pointCount));
        m_renderedPointCount += entry.pointCount;
    }

    m_shaderProgram->release();
}

void OctreeRenderer::render(const QMatrix4x4& viewMatrix, const QMatrix4x4& projectionMatrix) {
    if (!m_octree || !m_octree->isBuilt()) return;

    if (!m_initialized) {
        initializeGL();
    }

    // 更新可见节点
    updateVisibleNodes();

    // 上传脏的缓冲区
    uploadDirtyBuffers();

    // 设置 OpenGL 状态
    QOpenGLFunctions* f = QOpenGLContext::currentContext()->functions();

    f->glClearColor(
        m_backgroundColor.redF(),
        m_backgroundColor.greenF(),
        m_backgroundColor.blueF(),
        1.0f
    );
    f->glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    f->glEnable(GL_DEPTH_TEST);

    // 设置着色器 uniforms
    if (m_shaderProgram) {
        m_shaderProgram->bind();
        m_shaderProgram->setUniformValue("uViewMatrix", viewMatrix);
        m_shaderProgram->setUniformValue("uProjectionMatrix", projectionMatrix);
        m_shaderProgram->setUniformValue("uPointSize", m_pointSize);
    }

    // 渲染节点
    renderNodes();

    if (m_shaderProgram) {
        m_shaderProgram->release();
    }
}

} // namespace DeepLux
