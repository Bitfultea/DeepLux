# Phase 1: 3D 渲染框架基础设计（修订版）

## 1. 目标

在现有 DisplayManager 架构基础上，添加 3D 点云显示支持，实现：
- Viewport3DWidget (基于 QOpenGLWidget)
- PointCloudRenderer 点云渲染器
- 2D/3D 显示模式切换

## 2. 架构设计（修订后）

### 2.1 类图

```
QFrame (ViewportWidget)
├── 标题栏 + 通用工具栏
└── IViewportContent* m_content     ← 惰性创建，按需切换
      ├── Viewport2DContent (包装 HImageWidget + 2D 工具栏) [新建简化]
      └── Viewport3DContent (QOpenGLWidget + 3D 工具栏) [新建]
            ├── PointCloudGPUBuffer  ← 数据转换层 (double→float)
            ├── IPointCloudRenderer  ← 渲染接口
            └── CameraController     ← 纯数学类，无 QObject

DisplayManager
└── displayData(DisplayData&)
      └── target->displayData(data)  ← 路由下放到 ViewportWidget
```

### 2.2 核心接口

```cpp
// ============================================================================
// IViewportContent - 视口内容接口（新建）
// ============================================================================
class IViewportContent {
public:
    virtual ~IViewportContent() = default;
    virtual void displayData(const DisplayData& data) = 0;
    virtual void clearDisplay() = 0;
    virtual QWidget* toolbarExtension() { return nullptr; }  // 可选的工具栏
};

// ============================================================================
// PointCloudGPUBuffer - GPU 友好格式（新建）
// ============================================================================
struct PointCloudGPUBuffer {
    std::vector<float> positions;  // x,y,z 交错，float 精度
    std::vector<float> colors;     // r,g,b 交错
    std::vector<float> normals;    // nx,ny,nz 交错
    bool dirty = true;             // 标记是否需要重新上传 VBO

    // 从 PointCloudData 转换（一次性开销，数据变更时执行）
    void fromPointCloudData(const PointCloudData& data);
    size_t pointCount() const { return positions.size() / 3; }
};

// ============================================================================
// IPointCloudRenderer - 点云渲染器接口（新建）
// ============================================================================
class IPointCloudRenderer {
public:
    virtual ~IPointCloudRenderer() = default;
    virtual void setPointCloud(const PointCloudGPUBuffer& buffer) = 0;
    virtual void clear() = 0;
    virtual void setBackgroundColor(const QColor& color) = 0;
    virtual void scheduleRedraw() = 0;                    // 替换 update() 避免命名冲突
    virtual void setPointSize(float size) = 0;
    virtual void setColorMode(int mode) = 0;              // 0=uniform, 1=rgb, 2=labels
};

// ============================================================================
// CameraController - 纯数学相机控制（新建）
// ============================================================================
class CameraController {
public:
    CameraController();

    // 输入增量（由 Viewport3DWidget 将 Qt 事件转换后传入）
    void orbit(float deltaAzimuth, float deltaElevation);  // 旋转
    void pan(float deltaX, float deltaY);                  // 平移
    void zoom(float delta);                                // 缩放
    void reset();

    // 矩阵输出
    QMatrix4x4 viewMatrix() const;
    QMatrix4x4 projectionMatrix(float aspectRatio) const;

    // 状态查询
    QVector3D eye() const { return m_eye; }
    float distance() const;

private:
    QVector3D m_eye{0, 0, 5};
    QVector3D m_center{0, 0, 0};
    QVector3D m_up{0, 1, 0};
    float m_fov = 45.0f;
    float m_azimuth = 0.0f;
    float m_elevation = 0.0f;
};

// ============================================================================
// Viewport3DContent - 3D 视口内容（新建）
// ============================================================================
class Viewport3DContent : public QOpenGLWidget, public IViewportContent {
    Q_OBJECT
public:
    explicit Viewport3DContent(QWidget* parent = nullptr);
    ~Viewport3DContent() override;

    // IViewportContent
    void displayData(const DisplayData& data) override;
    void clearDisplay() override;
    QWidget* toolbarExtension() override { return nullptr; }

public slots:
    void resetCamera();

signals:
    void pointClicked(int index, const QVector3D& point);

protected:
    // QOpenGLWidget
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void updateCameraMatrices();
    QPointF mapToSphere(const QPoint& point) const;

    std::unique_ptr<IPointCloudRenderer> m_renderer;
    PointCloudGPUBuffer m_gpuBuffer;
    CameraController m_camera;
    QColor m_backgroundColor = Qt::darkGray;
    bool mNeedsUpdate = true;
    QPoint m_lastMousePos;
};

// ============================================================================
// ViewportWidget - 修改现有类
// ============================================================================
class ViewportWidget : public QFrame {
    Q_OBJECT
public:
    explicit ViewportWidget(QWidget* parent = nullptr);
    ~ViewportWidget() override;

    // 对外统一的显示接口（DisplayManager 调用此方法）
    void displayData(const DisplayData& data);

    // 工具栏动作
    QAction* fitWindowAction() { return m_fitWindowAction; }
    QAction* actualSizeAction() { return m_actualSizeAction; }
    QAction* zoomInAction() { return m_zoomInAction; }
    QAction* zoomOutAction() { return m_zoomOutAction; }

public slots:
    void closeViewport();

signals:
    void viewportClosed(const QString& id);
    void imageDisplayed();

private:
    // 惰性创建内容 widget
    template<typename T>
    void ensureContent();

    // 模板实现放在 cpp
    void setupToolbar();

    QString m_viewportId;
    QVBoxLayout* m_layout;
    QWidget* m_titleBar;
    QToolBar* m_toolbar;

    // 通用工具栏动作
    QAction* m_fitWindowAction;
    QAction* m_actualSizeAction;
    QAction* m_zoomInAction;
    QAction* m_zoomOutAction;
    QAction* m_closeAction;

    // 内容区域 - 使用 IViewportContent 接口
    IViewportContent* m_content = nullptr;
    Viewport2DContent* m_2dContent = nullptr;  // 仅用于类型转换
    Viewport3DContent* m_3dContent = nullptr;  // 仅用于类型转换
};
```

### 2.3 DisplayManager 修改

```cpp
// DisplayManager.cpp - 路由下放到 ViewportWidget
void DisplayManager::displayData(const DisplayData& data) {
    ViewportWidget* target = findAvailableViewport(data.viewportId);
    if (target) {
        target->displayData(data);  // ViewportWidget 自己决定怎么渲染
        emit dataDisplayed(target->viewportId());
    }
}

// ViewportWidget.cpp - 内部路由逻辑
void ViewportWidget::displayData(const DisplayData& data) {
    if (data.pointCloudData() && !data.pointCloudData()->isEmpty()) {
        ensureContent<Viewport3DContent>();  // 懒加载 3D 内容
        if (m_content) m_content->displayData(data);
    } else if (data.imageData()) {
        ensureContent<Viewport2DContent>();  // 原有 HImageWidget 包装
        if (m_content) m_content->displayData(data);
    }
}
```

### 2.4 PointCloudGPUBuffer 转换

```cpp
// PointCloudGPUBuffer.cpp
void PointCloudGPUBuffer::fromPointCloudData(const PointCloudData& data) {
    positions.clear();
    colors.clear();
    normals.clear();

    // double → float 转换
    positions.reserve(data.points.size() * 3);
    for (const auto& p : data.points) {
        positions.push_back(static_cast<float>(p.x()));
        positions.push_back(static_cast<float>(p.y()));
        positions.push_back(static_cast<float>(p.z()));
    }

    // 颜色转换
    if (data.hasColors()) {
        colors.reserve(data.colors.size() * 3);
        for (const auto& c : data.colors) {
            colors.push_back(static_cast<float>(c.x()));
            colors.push_back(static_cast<float>(c.y()));
            colors.push_back(static_cast<float>(c.z()));
        }
    }

    // 法向量转换
    if (data.hasNormals()) {
        normals.reserve(data.normals.size() * 3);
        for (const auto& n : data.normals) {
            normals.push_back(static_cast<float>(n.x()));
            normals.push_back(static_cast<float>(n.y()));
            normals.push_back(static_cast<float>(n.z()));
        }
    }

    dirty = true;
}
```

## 3. 文件结构

```
src/ui/display/
├── CMakeLists.txt                    (修改)
├── DisplayManager.h/cpp               (修改)
├── ViewportWidget.h/cpp               (修改)
├── IViewportContent.h                (新建)
├── Viewport3DContent.h/cpp            (新建)
├── PointCloudGPUBuffer.h/cpp          (新建)
├── IPointCloudRenderer.h              (新建)
├── PointCloudRendererOpenGL.h/cpp      (新建)
└── CameraController.h/cpp             (新建)
```

## 4. 依赖

- Qt5::OpenGL (Qt内置)
- Eigen3 (已有)
- OpenGL (系统库)

## 5. 实施计划

**Step 1:** 创建基础文件（接口、GPUBuffer、CameraController）
**Step 2:** 实现 Viewport3DContent + PointCloudRendererOpenGL
**Step 3:** 修改 ViewportWidget 支持惰性加载
**Step 4:** 修改 DisplayManager 路由
**Step 5:** 编译测试

## 6. 验收标准

1. ViewportWidget 可以显示 2D 图像（向后兼容）
2. 可以创建 3D 视口并显示点云数据
3. 2D/3D 切换通过 setDisplayMode 或自动路由实现
4. 相机控制（旋转/缩放/平移）正常工作
5. 所有代码通过编译，无 warning
