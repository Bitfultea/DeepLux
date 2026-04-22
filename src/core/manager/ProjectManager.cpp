#include "ProjectManager.h"
#include "../model/Project.h"
#include "../platform/PathUtils.h"
#include <QFile>
#include <QDir>
#include <QSettings>
#include <QDebug>

namespace DeepLux {

ProjectManager& ProjectManager::instance()
{
    static ProjectManager instance;
    return instance;
}

ProjectManager::ProjectManager()
{
    loadRecentProjects();
}

ProjectManager::~ProjectManager()
{
    closeProject();
}

Project* ProjectManager::newProject()
{
    closeProject();
    
    m_currentProject = std::make_unique<Project>();
    
    qDebug() << "New project created:" << m_currentProject->name();
    emit projectCreated(m_currentProject.get());
    
    return m_currentProject.get();
}

Project* ProjectManager::openProject(const QString& path)
{
    if (!QFile::exists(path)) {
        emit errorOccurred(tr("文件不存在: %1").arg(path));
        return nullptr;
    }
    
    closeProject();
    
    m_currentProject = std::make_unique<Project>();
    if (!m_currentProject->load(path)) {
        m_currentProject.reset();
        emit errorOccurred(tr("无法打开项目: %1").arg(path));
        return nullptr;
    }
    
    addToRecentProjects(path);
    
    qDebug() << "Project opened:" << m_currentProject->name();
    emit projectOpened(m_currentProject.get());
    
    return m_currentProject.get();
}

bool ProjectManager::saveProject(const QString& path)
{
    if (!m_currentProject) {
        emit errorOccurred(tr("没有打开的项目"));
        return false;
    }
    
    QString savePath = path.isEmpty() ? m_currentProject->filePath() : path;
    if (savePath.isEmpty()) {
        emit errorOccurred(tr("未指定保存路径"));
        return false;
    }
    
    if (!m_currentProject->save(savePath)) {
        emit errorOccurred(tr("保存失败: %1").arg(savePath));
        return false;
    }
    
    addToRecentProjects(savePath);
    
    qDebug() << "Project saved:" << savePath;
    emit projectSaved(m_currentProject.get());
    
    return true;
}

bool ProjectManager::saveAsProject(const QString& path)
{
    if (!m_currentProject) {
        emit errorOccurred(tr("没有打开的项目"));
        return false;
    }
    
    if (!m_currentProject->save(path)) {
        emit errorOccurred(tr("另存为失败: %1").arg(path));
        return false;
    }
    
    addToRecentProjects(path);
    
    qDebug() << "Project saved as:" << path;
    emit projectSaved(m_currentProject.get());
    
    return true;
}

void ProjectManager::closeProject()
{
    if (m_currentProject) {
        qDebug() << "Project closed:" << m_currentProject->name();
        m_currentProject.reset();
        emit projectClosed();
    }
}

void ProjectManager::setMaxRecentProjects(int max)
{
    if (m_maxRecentProjects != max && max > 0) {
        m_maxRecentProjects = max;
        while (m_recentProjects.size() > m_maxRecentProjects) {
            m_recentProjects.removeLast();
        }
        saveRecentProjects();
        emit recentProjectsChanged();
    }
}

void ProjectManager::clearRecentProjects()
{
    m_recentProjects.clear();
    saveRecentProjects();
    emit recentProjectsChanged();
}

bool ProjectManager::hasUnsavedChanges() const
{
    return m_currentProject && m_currentProject->isModified();
}

void ProjectManager::addToRecentProjects(const QString& path)
{
    // 移除重复项
    m_recentProjects.removeAll(path);
    
    // 添加到开头
    m_recentProjects.prepend(path);
    
    // 限制数量
    while (m_recentProjects.size() > m_maxRecentProjects) {
        m_recentProjects.removeLast();
    }
    
    saveRecentProjects();
    emit recentProjectsChanged();
}

void ProjectManager::loadRecentProjects()
{
    QSettings settings;
    settings.beginGroup("ProjectManager");
    m_recentProjects = settings.value("recentProjects").toStringList();
    settings.endGroup();
    
    // 验证文件是否存在
    QStringList validProjects;
    for (const QString& path : m_recentProjects) {
        if (QFile::exists(path)) {
            validProjects.append(path);
        }
    }
    m_recentProjects = validProjects;
}

void ProjectManager::saveRecentProjects()
{
    QSettings settings;
    settings.beginGroup("ProjectManager");
    settings.setValue("recentProjects", m_recentProjects);
    settings.endGroup();
}

} // namespace DeepLux
