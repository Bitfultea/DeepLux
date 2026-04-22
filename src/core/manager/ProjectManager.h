#pragma once

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
class ProjectManager : public QObject
{
    Q_OBJECT

public:
    static ProjectManager& instance();
    
    // 当前项目
    Project* currentProject() const { return m_currentProject.get(); }
    bool hasProject() const { return m_currentProject != nullptr; }
    
    // 项目操作
    Project* newProject();
    Project* openProject(const QString& path);
    bool saveProject(const QString& path = QString());
    bool saveAsProject(const QString& path);
    void closeProject();
    
    // 最近项目
    QStringList recentProjects() const { return m_recentProjects; }
    int maxRecentProjects() const { return m_maxRecentProjects; }
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
