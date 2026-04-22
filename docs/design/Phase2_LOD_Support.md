# Phase 2: LOD (Level of Detail) 支持设计

## 1. 目标

为大规模点云渲染提供多细节层次支持，实现：
- 基于距离的自动 LOD 切换
- 多分辨率点云缓冲区
- 平滑的 LOD 过渡
- 保持与 Phase 1 架构的兼容性

## 2. 架构设计

### 2.1 类图

```
PointCloudLODBuffer
├── m_originalBuffer      ← 原始全分辨率
├── m_lodLevels[]        ← 降采样层级
└── m_currentLOD        ← 当前激活层级

LODController
├── m_distanceThresholds[]  ← 每级切换距离
├── m_pointBudget          ← 点数预算
└── calculateLODLevel(distance) → int

PointCloudRendererOpenGL (修改)
├── m_lodBuffer           ← LOD 缓冲区
├── m_lodController       ← LOD 控制器
└── updateLODIfNeeded()   ← 按需更新 LOD

Viewport3DContent (修改)
├── m_lodEnabled          ← LOD 开关
└── m_cameraDistance       ← 当前相机距离
```

### 2.2 核心接口

```cpp
// ============================================================================
// PointCloudLODBuffer - 多分辨率点云缓冲区（新建）
// ============================================================================
class PointCloudLODBuffer {
public:
    // LOD 级别数量
    static constexpr int MAX_LOD_LEVELS = 4;

    PointCloudLODBuffer();

    // 设置原始数据，自动生成各级 LOD
    void setData(const PointCloudGPUBuffer& original);

    // 获取指定 LOD 级别的缓冲区
    const PointCloudGPUBuffer* getLevel(int level) const;
    int currentLevel() const { return m_currentLevel; }

    // 切换到指定级别
    void setCurrentLevel(int level);

    // 清空
    void clear();

    // 是否有效
    bool isValid() const { return m_originalBuffer.isValid(); }

private:
    // 生成降采样数据
    void generateLODLevels();

    // 随机采样降采样
    PointCloudGPUBuffer downsample(const PointCloudGPUBuffer& input, size_t targetCount) const;

    PointCloudGPUBuffer m_originalBuffer;           // 原始数据
    std::array<PointCloudGPUBuffer, MAX_LOD_LEVELS> m_lodLevels;  // LOD 层级
    int m_currentLevel = 0;                         // 当前级别
    bool m_dirty = true;                            // 标记是否需要重新上传
};

// ============================================================================
// LODController - LOD 控制器（新建）
// ============================================================================
class LODController {
public:
    // 距离阈值配置 [近, 中, 远, 超远]
    static constexpr float DEFAULT_DISTANCES[] = {1.0f, 5.0f, 20.0f, 100.0f};

    LODController();

    // 设置距离阈值
    void setDistanceThresholds(const std::array<float, 4>& thresholds);

    // 根据相机距离计算 LOD 级别
    int calculateLODLevel(float cameraDistance) const;

    // 设置/获取 LOD 是否启用
    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool isEnabled() const { return m_enabled; }

    // 点数预算（用于采样计算）
    void setPointBudget(size_t budget) { m_pointBudget = budget; }
    size_t pointBudget() const { return m_pointBudget; }

private:
    std::array<float, 4> m_distanceThresholds;
    bool m_enabled = true;
    size_t m_pointBudget = 100000;  // 默认 10 万点
};

// ============================================================================
// IPointCloudRenderer - 修改，新增 LOD 支持
// ============================================================================
class IPointCloudRenderer {
public:
    // ... 现有接口 ...

    // 新增 LOD 接口
    virtual void setLODEnabled(bool enabled) = 0;
    virtual bool isLODEnabled() const = 0;
    virtual void updateLODForDistance(float distance) = 0;
};
```

### 2.3 LOD 算法

#### 降采样策略

采用**均匀采样**算法，保证每个点都有相等的被选中概率：

```cpp
PointCloudGPUBuffer PointCloudLODBuffer::downsample(
    const PointCloudGPUBuffer& input,
    size_t targetCount) const
{
    PointCloudGPUBuffer output;
    size_t originalCount = input.pointCount();

    if (originalCount <= targetCount) {
        return input;  // 不需要降采样
    }

    // 计算采样步长 (等间距采样保证均匀性)
    float step = static_cast<float>(originalCount) / static_cast<float>(targetCount);

    // 复制采样点 - positions 是交错格式 (x,y,z,x,y,z,...)
    output.positions.reserve(targetCount * 3);
    for (size_t i = 0; i < targetCount; ++i) {
        size_t idx = static_cast<size_t>(i * step);
        size_t base = idx * 3;
        output.positions.push_back(input.positions[base]);
        output.positions.push_back(input.positions[base + 1]);
        output.positions.push_back(input.positions[base + 2]);
    }

    // 颜色同步采样
    if (input.hasColors()) {
        output.colors.reserve(targetCount * 3);
        for (size_t i = 0; i < targetCount; ++i) {
            size_t idx = static_cast<size_t>(i * step);
            size_t base = idx * 3;
            output.colors.push_back(input.colors[base]);
            output.colors.push_back(input.colors[base + 1]);
            output.colors.push_back(input.colors[base + 2]);
        }
    }

    // 法向量同步采样
    if (input.hasNormals()) {
        output.normals.reserve(targetCount * 3);
        for (size_t i = 0; i < targetCount; ++i) {
            size_t idx = static_cast<size_t>(i * step);
            size_t base = idx * 3;
            output.normals.push_back(input.normals[base]);
            output.normals.push_back(input.normals[base + 1]);
            output.normals.push_back(input.normals[base + 2]);
        }
    }

    // 标签同步采样
    if (input.hasLabels()) {
        output.labels.reserve(targetCount);
        for (size_t i = 0; i < targetCount; ++i) {
            size_t idx = static_cast<size_t>(i * step);
            output.labels.push_back(input.labels[idx]);
        }
    }

    output.dirty = true;
    return output;
}
```

#### 距离阈值

| LOD 级别 | 距离范围 | 采样率 | 目标点数 |
|---------|---------|--------|---------|
| 0 (近)  | 0-1m    | 100%   | 原始全部 |
| 1 (中)  | 1-5m    | 50%    | 50%     |
| 2 (远)  | 5-20m   | 20%    | 20%     |
| 3 (超远)| 20m+    | 10%    | 10%     |

### 2.4 数据流

```
PointCloudData (算法层)
        │
        ▼
PointCloudGPUBuffer::fromPointCloudData()
        │
        ▼
PointCloudLODBuffer::setData()
        │
        ├─► Level 0: 原始数据 (100%)
        ├─► Level 1: 50% 采样
        ├─► Level 2: 20% 采样
        └─► Level 3: 10% 采样
        │
        ▼
LODController::calculateLODLevel(distance)
        │
        ▼
PointCloudRendererOpenGL::render()
        │
        ▼
glDrawArrays(GL_POINTS, ...)
```

## 3. 文件结构

```
src/ui/display/3d/
├── PointCloudLODBuffer.h/cpp     (新建)
├── LODController.h/cpp          (新建)
├── PointCloudRendererOpenGL.h/cpp (修改)
└── Viewport3DContent.h/cpp      (修改)
```

## 4. 依赖

- Phase 1 所有依赖
- `<array>` (C++11)
- `<cstdlib>` (rand)

## 5. 实施计划

**Step 1:** 创建 LODController 类
**Step 2:** 创建 PointCloudLODBuffer 类
**Step 3:** 修改 PointCloudRendererOpenGL 支持 LOD
**Step 4:** 修改 Viewport3DContent 集成 LOD
**Step 5:** 编译测试

## 6. 验收标准

1. 大规模点云（100万+点）渲染流畅
2. 相机远离时自动降低细节，靠近时恢复
3. LOD 切换无明显视觉跳跃
4. 可通过配置开关 LOD
5. 向后兼容：LOD 关闭时行为与 Phase 1 一致
