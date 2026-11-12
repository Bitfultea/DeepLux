#include "ProjectManager.h"

#include "../common/Logger.h"
#include "../engine/RunEngine.h"
#include "../model/Project.h"
#include "../model/ProjectMigrator.h"
#include "../platform/PathUtils.h"
#include "PluginManager.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QSettings>

namespace DeepLux {

ProjectManager& ProjectManager::instance() {
    static ProjectManager instance;
    return instance;
}

ProjectManager::ProjectManager() {
    loadRecentProjects();
}

ProjectManager::~ProjectManager() {}

bool ProjectManager::resolveUnsavedForSwitch(UnsavedPolicy policy, QString* errorCode, QString* errorMessage,
                                             const QString& savePathOverride) {
    // 阶段1 §3（计划 §6.3）：新建/打开/关闭前的未保存检查——GUI/CLI/Agent 共用同一实现。
    // 用户未明确选择放弃时默认保留当前工程；保存失败不继续。
    Project* current = m_currentProject.get();
    if (!current || !current->isModified())
        return true;
    switch (policy) {
    case UnsavedPolicy::Save:
        // 复审2 R2：savePathOverride 非空时按显式路径保存（GUI「另存为」选择）；
        // 为空走既有路径——从未保存且无路径的工程如实失败,绝不静默丢弃
        if (saveProject(savePathOverride))
            return true;
        if (errorCode)
            *errorCode = QStringLiteral("SAVE_FAILED");
        if (errorMessage)
            *errorMessage = tr("当前工程有未保存修改且保存失败，操作中止");
        return false;
    case UnsavedPolicy::Discard:
        return true; // 用户显式选择放弃
    case UnsavedPolicy::Cancel:
        break;
    }
    if (errorCode)
        *errorCode = QStringLiteral("PROJECT_UNSAVED");
    if (errorMessage)
        *errorMessage = tr("当前工程「%1」有未保存修改").arg(current->name());
    return false;
}

Project* ProjectManager::newProjectChecked(UnsavedPolicy policy, QString* errorCode, QString* errorMessage,
                                           const QString& savePathOverride) {
    return replaceProject(std::make_unique<Project>(), false, true, policy, errorCode, errorMessage, savePathOverride);
}

Project* ProjectManager::openProjectChecked(const QString& path, UnsavedPolicy policy, QString* errorCode,
                                            QString* errorMessage, const QString& savePathOverride) {
    return openProjectTransaction(path, true, policy, errorCode, errorMessage, savePathOverride);
}

bool ProjectManager::closeProjectChecked(UnsavedPolicy policy, QString* errorCode, QString* errorMessage) {
    return closeProjectTransaction(true, policy, errorCode, errorMessage);
}

Project* ProjectManager::newProject() {
    return replaceProject(std::make_unique<Project>(), false, false, UnsavedPolicy::Discard, nullptr, nullptr,
                          QString());
}

Project* ProjectManager::openProject(const QString& path) {
    return openProjectTransaction(path, false, UnsavedPolicy::Discard, nullptr, nullptr, QString());
}

Project* ProjectManager::replaceProject(std::unique_ptr<Project> candidate, bool opened, bool checkUnsaved,
                                        UnsavedPolicy policy, QString* errorCode, QString* errorMessage,
                                        const QString& savePathOverride, const QString& recentPath) {
    if (!candidate)
        return nullptr;

    RunEngine& engine = RunEngine::instance();
    std::unique_ptr<Project> previous;
    Project* resultProject = nullptr;
    bool hadPrevious = false;
    QString switchErrorCode;
    QString switchErrorMessage;
    const auto result = engine.withProjectSwitch([&]() {
        const auto closeMutation = engine.withProjectMutationLease([&]() {
            if (checkUnsaved &&
                !resolveUnsavedForSwitch(policy, &switchErrorCode, &switchErrorMessage, savePathOverride))
                return false;
            hadPrevious = static_cast<bool>(m_currentProject);
            previous = std::move(m_currentProject);
            return true;
        });
        if (closeMutation != RunEngine::ProjectMutationResult::Applied)
            return false;

        // Destroy the old Project after releasing the maintenance lease. Event handlers clear/rebuild
        // RunEngine modules and must be allowed to take their own short maintenance leases. The outer
        // project-switch barrier remains active until every direct signal handler has returned.
        previous.reset();
        if (hadPrevious)
            emit projectClosed();

        // Preserve the established signal contract: projectClosed observers see no current project;
        // install the candidate only after their cleanup has completed, then notify projectCreated/opened.
        const auto openMutation = engine.withProjectMutationLease([&]() {
            if (m_currentProject)
                return false;
            m_currentProject = std::move(candidate);
            resultProject = m_currentProject.get();
            return true;
        });
        if (openMutation != RunEngine::ProjectMutationResult::Applied)
            return false;
        if (opened) {
            if (!recentPath.isEmpty())
                addToRecentProjects(recentPath);
            qDebug() << "Project opened:" << resultProject->name();
            emit projectOpened(resultProject);
        } else {
            qDebug() << "New project created:" << resultProject->name();
            emit projectCreated(resultProject);
        }
        return true;
    });

    if (result == RunEngine::ProjectMutationResult::Busy) {
        switchErrorCode = QStringLiteral("ENGINE_BUSY");
        switchErrorMessage = opened ? tr("流程运行中，无法打开工程") : tr("流程运行中，无法新建工程");
    }
    if (result != RunEngine::ProjectMutationResult::Applied) {
        if (errorCode)
            *errorCode = switchErrorCode;
        if (errorMessage)
            *errorMessage = switchErrorMessage;
        if (!switchErrorMessage.isEmpty())
            emit errorOccurred(switchErrorMessage);
        return nullptr;
    }
    return resultProject;
}

Project* ProjectManager::openProjectTransaction(const QString& path, bool checkUnsaved, UnsavedPolicy policy,
                                                QString* errorCode, QString* errorMessage,
                                                const QString& savePathOverride) {
    if (errorCode)
        errorCode->clear();
    if (errorMessage)
        errorMessage->clear();

    // Cancel is a user decision to keep the current unsaved project. Resolve it before
    // touching the candidate path so callers receive the switch guard result consistently.
    if (checkUnsaved && policy == UnsavedPolicy::Cancel && !resolveUnsavedForSwitch(policy, errorCode, errorMessage))
        return nullptr;

    if (!QFile::exists(path)) {
        emit errorOccurred(tr("文件不存在: %1").arg(path));
        return nullptr;
    }

    // 阶段1 §3（F07）：先完整读入并验证候选工程（load + 迁移），任一环节失败
    // 都不触碰当前工程——其对象、修改标记与撤销历史原样保留；
    // 成功后才关闭当前工程（projectClosed 联动清空 Agent 撤销栈）并替换指针。
    auto candidate = std::make_unique<Project>();
    if (!candidate->load(path)) {
        emit errorOccurred(tr("无法打开项目: %1").arg(path));
        return nullptr;
    }

    if (candidate->formatVersion() != QStringLiteral("3.0")) {
        PluginManager::instance().initialize();
        const MigrationReport report = ProjectMigrator::migrate(*candidate);
        Logger::instance().info(
            tr("工程已从 %1 迁移到 3.0：%2")
                .arg(candidate->migration().originalVersion, report.autoFixed.join(QStringLiteral("；"))),
            QStringLiteral("Project"));
        if (report.hasBlockers()) {
            Logger::instance().warning(tr("工程迁移存在待修复连接：%1").arg(report.blockers.join(QStringLiteral("；"))),
                                       QStringLiteral("Project"));
        }
    }

    return replaceProject(std::move(candidate), true, checkUnsaved, policy, errorCode, errorMessage, savePathOverride,
                          path);
}

bool ProjectManager::saveProject(const QString& path) {
    if (!m_currentProject) {
        emit errorOccurred(tr("没有打开的项目"));
        return false;
    }

    QString savePath = path.isEmpty() ? m_currentProject->filePath() : path;
    if (savePath.isEmpty()) {
        emit errorOccurred(tr("未指定保存路径"));
        return false;
    }

    if (!ProjectMigrator::saveWithBackup(*m_currentProject, savePath)) {
        emit errorOccurred(tr("保存失败: %1").arg(savePath));
        return false;
    }

    addToRecentProjects(savePath);

    qDebug() << "Project saved:" << savePath;
    emit projectSaved(m_currentProject.get());

    return true;
}

bool ProjectManager::saveAsProject(const QString& path) {
    if (!m_currentProject) {
        emit errorOccurred(tr("没有打开的项目"));
        return false;
    }

    if (!ProjectMigrator::saveWithBackup(*m_currentProject, path)) {
        emit errorOccurred(tr("另存为失败: %1").arg(path));
        return false;
    }

    addToRecentProjects(path);

    qDebug() << "Project saved as:" << path;
    emit projectSaved(m_currentProject.get());

    return true;
}

void ProjectManager::closeProject() {
    (void) closeProjectTransaction(false, UnsavedPolicy::Discard, nullptr, nullptr);
}

bool ProjectManager::closeProjectTransaction(bool checkUnsaved, UnsavedPolicy policy, QString* errorCode,
                                             QString* errorMessage) {
    RunEngine& engine = RunEngine::instance();
    std::unique_ptr<Project> previous;
    QString switchErrorCode;
    QString switchErrorMessage;
    const auto result = engine.withProjectSwitch([&]() {
        const auto mutation = engine.withProjectMutationLease([&]() {
            if (checkUnsaved && !resolveUnsavedForSwitch(policy, &switchErrorCode, &switchErrorMessage))
                return false;
            previous = std::move(m_currentProject);
            return true;
        });
        if (mutation != RunEngine::ProjectMutationResult::Applied)
            return false;
        if (previous) {
            qDebug() << "Project closed:" << previous->name();
            previous.reset();
            emit projectClosed();
        }
        return true;
    });
    if (result == RunEngine::ProjectMutationResult::Busy) {
        switchErrorCode = QStringLiteral("ENGINE_BUSY");
        switchErrorMessage = tr("流程运行中，无法关闭工程");
    }
    if (result != RunEngine::ProjectMutationResult::Applied) {
        if (errorCode)
            *errorCode = switchErrorCode;
        if (errorMessage)
            *errorMessage = switchErrorMessage;
        if (!switchErrorMessage.isEmpty())
            emit errorOccurred(switchErrorMessage);
        return false;
    }
    return true;
}

void ProjectManager::setMaxRecentProjects(int max) {
    if (m_maxRecentProjects != max && max > 0) {
        m_maxRecentProjects = max;
        while (m_recentProjects.size() > m_maxRecentProjects) {
            m_recentProjects.removeLast();
        }
        saveRecentProjects();
        emit recentProjectsChanged();
    }
}

void ProjectManager::clearRecentProjects() {
    m_recentProjects.clear();
    saveRecentProjects();
    emit recentProjectsChanged();
}

bool ProjectManager::hasUnsavedChanges() const {
    return m_currentProject && m_currentProject->isModified();
}

void ProjectManager::addToRecentProjects(const QString& path) {
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

void ProjectManager::loadRecentProjects() {
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

void ProjectManager::saveRecentProjects() {
    QSettings settings;
    settings.beginGroup("ProjectManager");
    settings.setValue("recentProjects", m_recentProjects);
    settings.endGroup();
}

} // namespace DeepLux
