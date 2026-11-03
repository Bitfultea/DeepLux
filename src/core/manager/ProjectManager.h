#pragma once

#include "../platform/Platform.h"

#include <QObject>
#include <QStringList>
#include <memory>

namespace DeepLux {

class Project;

/**
 * @brief 项目管理器
 *
 * 管理项目的创建、打开、保存、关闭
 * 维护最近打开的项目列表
 */
class DEEPLUX_API ProjectManager : public QObject {
    Q_OBJECT

public:
    static ProjectManager& instance();

    // 当前项目
    Project* currentProject() const {
        return m_currentProject.get();
    }
    bool hasProject() const {
        return m_currentProject != nullptr;
    }

    // 项目操作（低层入口：不做未保存检查，供测试与内部复用；
    // 生产入口一律使用下方 *Checked 保护版本——阶段1 §3）
    Project* newProject();
    Project* openProject(const QString& path);
    bool saveProject(const QString& path = QString());
    bool saveAsProject(const QString& path);
    void closeProject();

    /**
     * @brief 阶段1 §3（计划 §6.3）：工程切换前的未保存处理策略——GUI/CLI/Agent 共用
     *
     * Cancel：默认保留当前工程（零副作用中止）；
     * Save：先保存当前工程，保存失败不继续；
     * Discard：用户显式选择放弃后继续。
     */
    enum class UnsavedPolicy { Cancel, Save, Discard };

    /// 共享判定：返回 true 可继续切换；false 已按策略中止（errorCode/errorMessage 给出结构化原因）。
    /// savePathOverride 非空时 Save 策略以其落盘（GUI「另存为」兑现无路径新工程的保存语义,复审2 R2）
    bool resolveUnsavedForSwitch(UnsavedPolicy policy, QString* errorCode = nullptr, QString* errorMessage = nullptr,
                                 const QString& savePathOverride = QString());

    /// 受保护入口：先执行未保存判定，再走低层实现
    Project* newProjectChecked(UnsavedPolicy policy, QString* errorCode = nullptr, QString* errorMessage = nullptr,
                               const QString& savePathOverride = QString());
    Project* openProjectChecked(const QString& path, UnsavedPolicy policy, QString* errorCode = nullptr,
                                QString* errorMessage = nullptr, const QString& savePathOverride = QString());
    /// 返回值反映实际是否关闭（引擎忙等低层拒绝 → false + ENGINE_BUSY,复审2 R3）
    bool closeProjectChecked(UnsavedPolicy policy, QString* errorCode = nullptr, QString* errorMessage = nullptr);

    // 最近项目
    QStringList recentProjects() const {
        return m_recentProjects;
    }
    int maxRecentProjects() const {
        return m_maxRecentProjects;
    }
    void setMaxRecentProjects(int max);
    void clearRecentProjects();

    // 项目是否存在未保存修改
    bool hasUnsavedChanges() const;

signals:
    void projectCreated(Project* project);
    void projectOpened(Project* project);
    void projectSaved(Project* project);
    void projectClosed();
    void recentProjectsChanged();
    void errorOccurred(const QString& error);

private:
    ProjectManager();
    ~ProjectManager();

    void addToRecentProjects(const QString& path);
    void loadRecentProjects();
    void saveRecentProjects();

    std::unique_ptr<Project> m_currentProject;
    QStringList m_recentProjects;
    int m_maxRecentProjects = 10;
};

} // namespace DeepLux
