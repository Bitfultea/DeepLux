#include "MainViewModel.h"
#include "../core/manager/ProjectManager.h"
#include "../core/model/Project.h"
#include <QTimer>
#include <QDateTime>
#include <QFileInfo>

namespace DeepLux {

MainViewModel& MainViewModel::instance()
{
    static MainViewModel instance;
    return instance;
}

MainViewModel::MainViewModel()
    : QObject(nullptr)
    , m_timer(new QTimer(this))
{
    m_currentTime = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");

    connect(m_timer, &QTimer::timeout, this, &MainViewModel::updateCurrentTime);
    m_timer->start(1000);

    // 连接 ProjectManager 信号
    ProjectManager& pm = ProjectManager::instance();
    connect(&pm, &ProjectManager::projectOpened, this, [this](Project* project) {
        if (project) {
            m_projectName = project->name();
            m_projectPath = project->filePath();
            m_modified = false;
            emit projectNameChanged();
            emit modifiedChanged();
        }
    });
    connect(&pm, &ProjectManager::projectSaved, this, [this](Project* project) {
        if (project) {
            m_modified = false;
            emit modifiedChanged();
        }
    });
    connect(&pm, &ProjectManager::projectClosed, this, [this]() {
        m_projectName.clear();
        m_projectPath.clear();
        m_modified = false;
        emit projectNameChanged();
        emit modifiedChanged();
    });
}

MainViewModel::~MainViewModel()
{
}

QString MainViewModel::currentTime() const
{
    return m_currentTime;
}

QString MainViewModel::projectName() const
{
    if (!m_projectName.isEmpty()) {
        return m_projectName;
    }
    ProjectManager& pm = ProjectManager::instance();
    if (pm.currentProject()) {
        return pm.currentProject()->name();
    }
    return QString();
}

bool MainViewModel::isModified() const
{
    ProjectManager& pm = ProjectManager::instance();
    if (pm.currentProject()) {
        return pm.currentProject()->isModified();
    }
    return m_modified;
}

void MainViewModel::newProject()
{
    ProjectManager& pm = ProjectManager::instance();
    Project* project = pm.newProject();
    if (project) {
        m_projectName = project->name();
        m_projectPath.clear();
        m_modified = false;
        emit projectNameChanged();
        emit modifiedChanged();
    }
}

bool MainViewModel::openProject(const QString& path)
{
    if (path.isEmpty()) {
        emit errorOccurred("Invalid path");
        return false;
    }

    ProjectManager& pm = ProjectManager::instance();
    Project* project = pm.openProject(path);
    if (project) {
        m_projectName = project->name();
        m_projectPath = project->filePath();
        m_modified = false;
        emit projectLoaded();
        return true;
    } else {
        emit errorOccurred(tr("Failed to open project: %1").arg(path));
        return false;
    }
}

bool MainViewModel::saveProject(const QString& path)
{
    ProjectManager& pm = ProjectManager::instance();

    QString savePath = path.isEmpty() ? m_projectPath : path;
    if (savePath.isEmpty()) {
        emit errorOccurred("No path specified for saving");
        return false;
    }

    bool success;
    if (pm.hasProject()) {
        success = pm.saveProject(savePath);
    } else {
        success = pm.saveAsProject(savePath);
    }

    if (success) {
        m_projectPath = savePath;
        emit projectSaved();
    } else {
        emit errorOccurred(tr("Failed to save project: %1").arg(savePath));
    }
    return success;
}

void MainViewModel::runProject()
{
    emit errorOccurred("Run function not yet implemented");
}

void MainViewModel::stopProject()
{
    emit errorOccurred("Stop function not yet implemented");
}

void MainViewModel::updateCurrentTime()
{
    m_currentTime = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");
    emit currentTimeChanged();
}

} // namespace DeepLux