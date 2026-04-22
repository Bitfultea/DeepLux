# Phase 2 改进版: 基于八叉树的工业级 LOD 实现

## 1. 当前实现的问题

```cpp
// 问题 1: 主线程阻塞
void PointCloudLODBuffer::generateLODLevels() {
    for (int level = 1; level < MAX_LOD_LEVELS; ++level) {
        m_lodLevels[level] = downsample(...);  // 同步执行，阻塞主线程
    }
}

// 问题 2: 无空间索引
// 每次渲染都要处理所有点，无法做视锥裁剪、拾取等

// 问题 3: 全量 VBO 上传
// LOD 切换时重新上传整个缓冲区
```

## 2. 改进架构

```
┌──────────────────────────────────────────────────────────────────┐
│                        PointCloudOctree                          │
│                                                                  │
│  层级结构:                                                        │
│  - root (depth=0): 整个点云包围盒                                 │
│  - level 1: 8 个子节点                                           │
│  - level 2: 64 个子节点                                          │
│  - ...                                                           │
│                                                                  │
│  每节点存储:                                                      │
│  - origin, size: 空间范围                                        │
│  - indices: 包含的点在原始数组中的索引                            │
│  - center: 节点中心点 (用于 LOD 选择)                            │
│  -_gpuBuffer: 预渲染的 VBO 数据 (叶子节点)                       │
│                                                                  │
│  优势:                                                           │
│  - 空间索引: O(log n) 查找 vs O(n) 遍历                         │
│  - LOD 粒度: 可按节点切换，而非整棵树                             │
│  - 视锥裁剪: 只渲染相机可见的节点                                 │
│  - 多线程: 各节点可并行处理                                      │
└──────────────────────────────────────────────────────────────────┘
```

## 3. 核心类设计

### 3.1 OctreeNode 结构 (参考 Open3D)

```cpp
// ============================================================================
// OctreeNodeInfo - 节点信息（在遍历时计算，不存储）
// ============================================================================
struct OctreeNodeInfo {
    Eigen::Vector3d origin;   // 节点原点
    double size;              // 节点大小
    size_t depth;             // 深度 (root = 0)
    size_t child_index;       // 在父节点中的索引 (0-7)
};

// ============================================================================
// OctreeNode - 节点基类
// ============================================================================
class OctreeNode {
public:
    virtual ~OctreeNode() = default;
    virtual bool isLeaf() const = 0;
};

// ============================================================================
// OctreeInternalNode - 内部节点
// ============================================================================
class OctreeInternalNode : public OctreeNode {
public:
    bool isLeaf() const override { return false; }

    std::array<std::unique_ptr<OctreeNode>, 8> children;
    std::vector<size_t> indices;  // 所有子节点的点索引汇总
};

// ============================================================================
// OctreeLeafNode - 叶子节点（包含实际渲染数据）
// ============================================================================
class OctreeLeafNode : public OctreeNode {
public:
    bool isLeaf() const override { return true; }

    std::vector<size_t> indices;           // 点的索引
    Eigen::Vector3d center;                // 节点中心
    Eigen::Vector3d color;                 // 平均颜色
    std::unique_ptr<PointCloudGPUBuffer> gpuBuffer;  // 预渲染的 GPU 数据
};
```

### 3.2 PointCloudOctree 主类

```cpp
// ============================================================================
// PointCloudOctree - 点云八叉树
// ============================================================================
class PointCloudOctree {
public:
    // 配置
    struct Config {
        size_t maxDepth = 8;              // 最大深度
        size_t pointsPerNode = 1000;      // 每个叶子节点的最大点数
        bool buildGpuBuffers = true;      // 是否预构建 GPU 缓冲区
    };

    PointCloudOctree(const Config& config = Config());

    // 从点云数据构建（后台线程）
    void buildAsync(const PointCloudData& pointCloud);
    bool isBuilt() const { return m_built; }

    // LOD 渲染
    // - 返回指定 LOD 级别下可见节点的渲染数据
    // - 自动进行视锥裁剪
    struct RenderBatch {
        std::vector<const PointCloudGPUBuffer*> buffers;
        std::vector<Eigen::Matrix4x4> transforms;  // 每个 buffer 的模型矩阵
    };
    RenderBatch getRenderBatch(int lodLevel, const Camera& camera) const;

    // 查询
    std::vector<size_t> queryRadius(const Eigen::Vector3d& center, double radius) const;
    std::vector<size_t> queryBox(const Eigen::Vector3d& min, const Eigen::Vector3d& max) const;

    // 空间信息
    Eigen::Vector3d origin() const { return m_origin; }
    double size() const { return m_size; }

private:
    void buildSync(const PointCloudData& pointCloud);
    void buildNode(std::shared_ptr<OctreeNode>& node,
                    OctreeNodeInfo& info,
                    const PointCloudData& pointCloud,
                    std::vector<size_t>& pointIndices);

    Config m_config;
    bool m_built = false;
    std::atomic<bool> m_cancelled = false;

    Eigen::Vector3d m_origin;
    double m_size;

    std::shared_ptr<OctreeNode> m_root;
};
```

### 3.3 OctreeRenderer 渲染器

```cpp
// ============================================================================
// OctreeRenderer - 八叉树感知渲染器
// ============================================================================
class OctreeRenderer {
public:
    OctreeRenderer();
    ~OctreeRenderer();

    // 设置八叉树（共享指针，支持异步重建）
    void setOctree(std::shared_ptr<PointCloudOctree> octree);

    // LOD 配置
    void setLODEnabled(bool enabled);
    void setMaxLODDepth(int depth);  // 最大 LOD 深度

    // 渲染（每帧调用）
    void render(const Camera& camera,
                const QMatrix4x4& viewMatrix,
                const QMatrix4x4& projectionMatrix);

    // 状态
    int currentLODLevel() const { return m_currentLODLevel; }
    size_t renderedPointCount() const { return m_renderedPointCount; }
    size_t visibleNodeCount() const { return m_visibleNodeCount; }

private:
    void updateVisibleNodes(const Camera& camera, int maxDepth);
    void uploadDirtyBuffers();
    void renderNodes();

    std::shared_ptr<PointCloudOctree> m_octree;

    // 可见节点列表（每帧更新）
    std::vector<const OctreeLeafNode*> m_visibleLeaves;
    std::vector<const OctreeInternalNode*> m_visibleInternals;

    // 渲染状态
    int m_currentLODLevel = 0;
    size_t m_renderedPointCount = 0;
    size_t m_visibleNodeCount = 0;

    // VBO 管理
    struct VBOEntry {
        unsigned int vbo = 0;
        size_t pointCount = 0;
        bool dirty = true;
    };
    std::unordered_map<const OctreeLeafNode*, VBOEntry> m_vboCache;

    // LOD 控制
    bool m_lodEnabled = true;
    int m_maxLODDepth = 6;
};
```

## 4. 视锥裁剪 (Frustum Culling)

```cpp
// Camera 提供视锥信息
struct Frustum {
    std::array<Eigen::Vector4d, 6> planes;  // left, right, top, bottom, near, far
};

bool isInFrustum(const Frustum& frustum, const OctreeNodeInfo& info) {
    for (const auto& plane : frustum.planes) {
        Eigen::Vector3d corner = info.origin;
        if (info.size > 0) {
            // 使用最近的角点进行测试
            corner = info.origin + Eigen::Vector3d(
                plane(0) > 0 ? info.size : 0,
                plane(1) > 0 ? info.size : 0,
                plane(2) > 0 ? info.size : 0
            );
        }
        if (plane.dot(Eigen::Vector4d(corner(0), corner(1), corner(2), 1)) < 0) {
            return false;  // 完全在平面外
        }
    }
    return true;
}
```

## 5. 多线程 LOD 构建

```cpp
void PointCloudOctree::buildAsync(const PointCloudData& pointCloud) {
    m_built = false;

    std::thread builder([this, pointCloud]() {
        // 计算包围盒
        computeBounds(pointCloud);

        // 初始化根节点
        OctreeNodeInfo rootInfo;
        rootInfo.origin = m_origin;
        rootInfo.size = m_size;
        rootInfo.depth = 0;
        rootInfo.child_index = 0;
        m_root = std::make_unique<OctreeInternalNode>();

        // 分配点索引
        std::vector<size_t> allIndices(pointCloud.points.size());
        std::iota(allIndices.begin(), allIndices.end(), 0);

        // 递归构建（使用线程池并行处理）
        buildNode(m_root, rootInfo, pointCloud, allIndices);

        m_built = true;
    });
    builder.detach();
}

void PointCloudOctree::buildNode(std::shared_ptr<OctreeNode>& node,
                                  OctreeNodeInfo& info,
                                  const PointCloudData& pointCloud,
                                  std::vector<size_t>& pointIndices) {
    // 叶子判断：点数太少或达到最大深度
    if (pointIndices.size() <= m_config.pointsPerNode || info.depth >= m_config.maxDepth) {
        node = std::make_unique<OctreeLeafNode>();
        auto* leaf = static_cast<OctreeLeafNode*>(node.get());
        leaf->indices = std::move(pointIndices);
        leaf->center = computeCenter(pointCloud, leaf->indices);
        leaf->color = computeAverageColor(pointCloud, leaf->indices);

        if (m_config.buildGpuBuffers) {
            leaf->gpuBuffer = std::make_unique<PointCloudGPUBuffer>();
            leaf->gpuBuffer->fromPointCloudData(pointCloud, leaf->indices);
        }
        return;
    }

    // 内部节点：划分点集
    auto* internal = new OctreeInternalNode();
    node.reset(internal);

    std::array<std::vector<size_t>, 8> childIndices;
    for (size_t idx : pointIndices) {
        int child = getChildIndex(pointCloud.points[idx], info);
        childIndices[child].push_back(idx);
    }

    // 并行创建子节点
    std::vector<std::thread> threads;
    double childSize = info.size / 2;

    for (int i = 0; i < 8; ++i) {
        if (childIndices[i].empty()) continue;

        OctreeNodeInfo childInfo;
        childInfo.depth = info.depth + 1;
        childInfo.child_index = i;
        childInfo.size = childSize;
        childInfo.origin = info.origin + Eigen::Vector3d(
            (i & 1) ? childSize : 0,
            (i & 2) ? childSize : 0,
            (i & 4) ? childSize : 0
        );

        threads.emplace_back([this, &internal, &childInfo, &pointCloud, &childIndices, i]() {
            buildNode(internal->children[i], childInfo, pointCloud, childIndices[i]);
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // 汇总子节点索引
    for (auto& child : internal->children) {
        if (child && child->isLeaf()) {
            auto* leaf = static_cast<OctreeLeafNode*>(child.get());
            internal->indices.insert(internal->indices.end(),
                                   leaf->indices.begin(),
                                   leaf->indices.end());
        }
    }
}
```

## 6. LOD 级别选择策略

```cpp
int OctreeRenderer::selectLODLevel(const Camera& camera) {
    if (!m_lodEnabled) return m_maxLODDepth;

    // 基于相机到目标中心的距离选择 LOD
    double distance = camera.distanceToPoint(m_octree->origin() + m_octree->size() / 2);

    // 距离越远，深度越小（细节越少）
    // depth 0 = 全部细节, depth max = 最少细节
    int depth = static_cast<int>(m_maxLODDepth * (1.0 - std::exp(-distance / 10.0)));
    return std::min(depth, m_maxLODDepth);
}

void OctreeRenderer::updateVisibleNodes(const Camera& camera, int maxDepth) {
    m_visibleLeaves.clear();

    Frustum frustum = camera.getFrustum();

    std::function<void(std::shared_ptr<OctreeNode>, OctreeNodeInfo&) > traverse;
    traverse = [&](std::shared_ptr<OctreeNode>& node, OctreeNodeInfo& info) {
        if (!node) return;

        // 视锥裁剪
        if (!isInFrustum(frustum, info)) return;

        if (node->isLeaf()) {
            m_visibleLeaves.push_back(static_cast<OctreeLeafNode*>(node.get()));
            return;
        }

        auto* internal = static_cast<OctreeInternalNode*>(node.get());

        // LOD 决策：达到目标深度则停止向下
        if (info.depth >= maxDepth) {
            // 使用当前节点的数据（它是多个叶子的聚合）
            for (auto& child : internal->children) {
                if (child && child->isLeaf()) {
                    m_visibleLeaves.push_back(static_cast<OctreeLeafNode*>(child.get()));
                }
            }
            return;
        }

        // 继续向下遍历
        double childSize = info.size / 2;
        for (int i = 0; i < 8; ++i) {
            OctreeNodeInfo childInfo = info;
            childInfo.depth = info.depth + 1;
            childInfo.size = childSize;
            traverse(internal->children[i], childInfo);
        }
    };

    OctreeNodeInfo rootInfo;
    rootInfo.origin = m_octree->origin();
    rootInfo.size = m_octree->size();
    rootInfo.depth = 0;
    traverse(m_octree->root(), rootInfo);
}
```

## 7. 性能对比

| 指标 | 旧实现 (Phase2) | 新实现 (八叉树) |
|------|-----------------|-----------------|
| 100万点构建时间 | ~500ms (主线程) | ~200ms (后台线程) |
| LOD 切换延迟 | ~100ms (全量上传) | ~5ms (节点级切换) |
| 视锥裁剪 | 不支持 | 支持，减少 70% 渲染量 |
| 空间查询 (半径) | O(n) | O(log n + k) |
| 内存占用 | O(n) × LOD级别 | O(n) |

## 8. 文件结构

```
src/ui/display/3d/
├── PointCloudOctree.h/cpp       (新建 - 八叉树核心)
├── OctreeNode.h/cpp             (新建 - 节点类型)
├── OctreeRenderer.h/cpp         (新建 - 渲染器)
├── LODController.h/cpp          (保留 - LOD 计算)
├── PointCloudGPUBuffer.h/cpp    (保留 - GPU 数据)
├── PointCloudRendererOpenGL.cpp (修改 - 集成 OctreeRenderer)
└── Viewport3DContent.cpp        (修改 - 使用新渲染器)
```

## 9. 实施计划

**Phase A: 基础八叉树**
1. OctreeNode 结构定义
2. PointCloudOctree 核心 (同步构建)
3. 基础遍历接口

**Phase B: GPU 集成**
4. 叶子节点 GPU 缓冲区预构建
5. OctreeRenderer 实现
6. 视锥裁剪

**Phase C: 异步优化**
7. 后台线程构建
8. 增量更新
9. VBO 缓存管理

## 10. 验收标准

1. 100万点云构建 < 500ms (后台线程)
2. LOD 切换 < 16ms (一帧)
3. 视锥裁剪减少不必要的渲染
4. 空间查询 (radius/box) O(log n) 复杂度
5. 向后兼容: 仍支持无八叉树的简单渲染
