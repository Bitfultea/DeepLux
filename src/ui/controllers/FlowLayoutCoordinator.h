#pragma once

#include "core/engine/FlowGraphAnalyzer.h"
#include "core/model/Project.h"

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <functional>

namespace DeepLux {

class FlowCanvas;

/**
 * @brief 流程布局协调器
 *
 * 监听 Project 结构信号，合并为一次执行计划分析并将结果应用到 FlowCanvas。
 * 只负责展示布局，不参与运行期排序（RunEngine 仍使用自己的执行顺序）。
 *
 * - 所有结构信号统一 30ms 单次定时器合并，每个事件循环轮次最多布局一次；
 * - 用户拖动只写回坐标，不触发重新布局；
 * - suppressAutoLayout(true) 取消待执行布局并丢弃执行中的结果；
 * - 工程切换取消待执行布局，已初始化且坐标有效时保留原有坐标。
 */
class FlowLayoutCoordinator : public QObject {
    Q_OBJECT

public:
    using CatalogProvider = std::function<FlowGraphCatalog(const Project&)>;

    FlowLayoutCoordinator(Project* project, FlowCanvas* canvas, CatalogProvider catalogProvider,
                          QObject* parent = nullptr);
    ~FlowLayoutCoordinator() override;

    /// 切换工程：取消待执行布局、清空缓存计划；仅在未初始化/坐标无效时触发 Full。
    void setProject(Project* project);
    Project* project() const {
        return m_project.data();
    }

    /// 抑制/恢复自动布局。恢复时不会自动补跑，需等待下一次结构信号或 requestLayout()。
    void suppressAutoLayout(bool suppress);
    bool autoLayoutSuppressed() const {
        return m_suppressed;
    }

    /// 主动请求一次合并布局（受 suppress 影响）。
    void requestLayout();

    /// 最近一次成功应用的执行计划（拖动/测试使用）。
    const FlowOrderPlan& currentPlan() const {
        return m_currentPlan;
    }

    /// 实际执行布局的次数（用于测试断言合并行为）。
    int layoutRunCount() const {
        return m_layoutRunCount;
    }

signals:
    void layoutApplied();

private slots:
    void scheduleLayout();
    void runLayout();

private:
    static bool positionsValid(const Project& project);
    void connectProjectSignals();

    QPointer<Project> m_project;
    QPointer<FlowCanvas> m_canvas;
    CatalogProvider m_catalogProvider;
    QTimer m_layoutTimer;
    FlowOrderPlan m_currentPlan;
    bool m_layoutPending = false;
    bool m_suppressed = false;
    quint64 m_generation = 0;
    int m_layoutRunCount = 0;
};

} // namespace DeepLux
