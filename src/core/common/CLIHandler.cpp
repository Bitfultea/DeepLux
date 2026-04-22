#include "CLIHandler.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QUuid>

#include "core/manager/PluginManager.h"
#include "core/manager/ProjectManager.h"
#include "core/device/CameraManager.h"
#include "core/model/Project.h"
#include "core/platform/Platform.h"
#include "core/common/Logger.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

// ============== 命令实现 ==============

class VersionCommand : public ICommand
{
public:
    QString name() const override { return "version"; }
    QString description() const override { return "显示版本信息"; }
    QString help() const override { return "deeplux version"; }

    int execute(const QStringList& args, CommandContext& ctx) override
    {
        Q_UNUSED(args);
        Q_UNUSED(ctx);
        printf("DeepLux Vision %s\n", DEEPLUX_VERSION_STRING);
        printf("Platform: %s\n", DEEPLUX_PLATFORM_NAME);
        printf("Qt Version: %s\n", qVersion());
#ifdef DEEPLUX_HAS_OPENCV
        printf("OpenCV: %s\n", CV_VERSION);
#else
        printf("OpenCV: Not available (demo mode)\n");
#endif
        return 0;
    }
};

class HelpCommand : public ICommand
{
public:
    QString name() const override { return "help"; }
    QString description() const override { return "显示帮助信息"; }
    QString help() const override { return "deeplux help [command]"; }

    int execute(const QStringList& args, CommandContext& ctx) override
    {
        Q_UNUSED(ctx);
        CLIHandler& handler = CLIHandler::instance();

        if (args.isEmpty()) {
            printf("%s\n", qPrintable(handler.helpText()));
            return 0;
        }

        QString cmdName = args.first();
        ICommand* cmd = handler.findCommand(cmdName);
        if (cmd) {
            printf("%s\n\n%s\n", qPrintable(cmd->name()), qPrintable(cmd->help()));
            return 0;
        }

        printf("未知命令: %s\n", qPrintable(cmdName));
        return 1;
    }
};

class InfoCommand : public ICommand
{
public:
    QString name() const override { return "info"; }
    QString description() const override { return "显示系统信息"; }
    QString help() const override { return "deeplux info"; }

    int execute(const QStringList& args, CommandContext& ctx) override
    {
        Q_UNUSED(args);
        Q_UNUSED(ctx);

        printf("=== DeepLux Vision System Info ===\n");
        printf("Version:    %s\n", DEEPLUX_VERSION_STRING);
        printf("Platform:   %s\n", DEEPLUX_PLATFORM_NAME);
        printf("Qt:         %s\n", qVersion());

        QStringList paths = PluginManager::instance().pluginPaths();
        printf("Plugin Dir: %s\n", qPrintable(paths.isEmpty() ? QString("(not set)") : paths.first()));

        int pluginCount = PluginManager::instance().moduleInfos().size();
        printf("Plugins:    %d loaded\n", pluginCount);

        printf("================================\n");
        return 0;
    }
};

class ListPluginsCommand : public ICommand
{
public:
    QString name() const override { return "list-plugins"; }
    QString description() const override { return "列出所有已加载的插件"; }
    QString help() const override { return "deeplux list-plugins"; }

    int execute(const QStringList& args, CommandContext& ctx) override
    {
        Q_UNUSED(args);
        Q_UNUSED(ctx);

        // 先加载所有插件
        PluginManager::instance().loadAllPlugins();

        auto plugins = PluginManager::instance().moduleInfos();

        if (plugins.isEmpty()) {
            printf("No plugins loaded.\n");
            return 0;
        }

        printf("=== Loaded Plugins (%d) ===\n", plugins.size());
        for (const auto& info : plugins) {
            QString status = info.loaded ? "OK" : "FAIL";
            printf("[%s] %s v%s (%s)\n",
                   qPrintable(status),
                   qPrintable(info.name),
                   qPrintable(info.version),
                   qPrintable(info.category));
        }
        printf("=========================\n");
        return 0;
    }
};

class ConnectCameraCommand : public ICommand
{
public:
    QString name() const override { return "connect-camera"; }
    QString description() const override { return "连接相机"; }
    QString help() const override { return "deeplux connect-camera <camera_id>"; }

    int execute(const QStringList& args, CommandContext& ctx) override
    {
        Q_UNUSED(ctx);
        if (args.isEmpty()) {
            printf("用法: deeplux connect-camera <camera_id>\n");
            return 1;
        }
        PluginManager::instance().loadAllPlugins();
        QString cameraId = args.first();
        bool ok = CameraManager::instance().connectCamera(cameraId);
        if (ok) {
            printf("Camera connected: %s\n", qPrintable(cameraId));
            return 0;
        } else {
            QString error = CameraManager::instance().lastError(cameraId);
            printf("Failed to connect camera: %s (%s)\n", qPrintable(cameraId), qPrintable(error));
            return 1;
        }
    }
};

class RunCommand : public ICommand
{
public:
    QString name() const override { return "run"; }
    QString description() const override { return "运行工程或流程"; }
    QString help() const override { return "deeplux run <project.dproj> [--flow <name>]"; }

    int execute(const QStringList& args, CommandContext& ctx) override
    {
        if (args.isEmpty()) {
            ctx.setError("请指定要运行的工程文件");
            return 1;
        }

        QString projectPath = args.first();

        // 检查文件是否存在
        if (!QFileInfo::exists(projectPath)) {
            ctx.setError(QString("文件不存在: %1").arg(projectPath));
            return 1;
        }

        // 打开工程
        Project* project = ProjectManager::instance().openProject(projectPath);
        if (!project) {
            ctx.setError(QString("无法打开工程: %1").arg(projectPath));
            return 1;
        }

        printf("Opened project: %s\n", qPrintable(project->name()));

        // 查找流程
        QString flowName;
        for (int i = 1; i < args.size(); ++i) {
            if (args[i] == "--flow" && i + 1 < args.size()) {
                flowName = args[++i];
            }
        }

        // TODO: 执行流程
        if (!flowName.isEmpty()) {
            printf("Running flow: %s (not implemented yet)\n", qPrintable(flowName));
        } else {
            printf("Running all flows in project...\n");
            // RunEngine would be used here
        }

        return 0;
    }
};

class CreateProjectCommand : public ICommand
{
public:
    QString name() const override { return "create-project"; }
    QString description() const override { return "创建新工程"; }
    QString help() const override { return "deeplux create-project <name> [--path <dir>]"; }

    int execute(const QStringList& args, CommandContext& ctx) override
    {
        if (args.isEmpty()) {
            ctx.setError("请指定工程名称");
            return 1;
        }

        QString projectName = args.first();
        QString savePath;

        // 解析选项
        for (int i = 1; i < args.size(); ++i) {
            if (args[i] == "--path" && i + 1 < args.size()) {
                savePath = args[++i];
            }
        }

        Project* project = ProjectManager::instance().newProject();
        project->setName(projectName);

        if (!savePath.isEmpty()) {
            project->setFilePath(savePath + "/" + projectName + ".dproj");
        } else {
            // 默认保存到当前目录
            project->setFilePath(QDir::currentPath() + "/" + projectName + ".dproj");
        }

        // 立即保存以便持久化
        if (project->save()) {
            printf("Created project: %s\n", qPrintable(projectName));
            printf("Saved to: %s\n", qPrintable(project->filePath()));
        } else {
            printf("Created project: %s (save failed)\n", qPrintable(projectName));
        }

        // 保存以便后续命令可以使用
        ctx.data()["current_project"] = projectName;
        ctx.data()["current_project_ptr"] = QVariant::fromValue<void*>(project);

        return 0;
    }
};

class AddModuleCommand : public ICommand
{
public:
    QString name() const override { return "add-module"; }
    QString description() const override { return "添加模块到工程"; }
    QString help() const override { return "deeplux add-module <project> <module_type> [--name <name>] [--pos <x,y>]"; }

    int execute(const QStringList& args, CommandContext& ctx) override
    {
        if (args.size() < 2) {
            ctx.setError("用法: add-module <project> <module_type> [--name <name>] [--pos <x,y>]");
            return 1;
        }

        QString projectPath = args[0];
        QString moduleType = args[1];
        QString moduleName = moduleType;
        int posX = 0, posY = 0;

        // 解析选项
        for (int i = 2; i < args.size(); ++i) {
            if (args[i] == "--name" && i + 1 < args.size()) {
                moduleName = args[++i];
            } else if (args[i] == "--pos" && i + 1 < args.size()) {
                QStringList pos = args[++i].split(",");
                if (pos.size() == 2) {
                    posX = pos[0].toInt();
                    posY = pos[1].toInt();
                }
            }
        }

        // 打开或获取工程
        Project* project = nullptr;
        if (QFileInfo::exists(projectPath)) {
            project = ProjectManager::instance().openProject(projectPath);
        } else {
            ctx.setError(QString("工程文件不存在: %1").arg(projectPath));
            return 1;
        }

        if (!project) {
            ctx.setError("无法打开工程");
            return 1;
        }

        // 检查模块类型是否可用
        QStringList available = PluginManager::instance().availableModules();
        if (!available.contains(moduleType)) {
            printf("Warning: module '%s' not in available modules, but will be added anyway\n",
                   qPrintable(moduleType));
        }

        // 创建模块实例
        ModuleInstance instance;
        instance.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        instance.moduleId = moduleType;
        instance.name = moduleName;
        instance.posX = posX;
        instance.posY = posY;

        project->addModule(instance);

        // 保存工程
        if (project->save()) {
            printf("Added module '%s' (%s) to project\n", qPrintable(moduleName), qPrintable(moduleType));
            printf("Module ID: %s\n", qPrintable(instance.id));
            printf("Project saved.\n");
        } else {
            printf("Added module '%s' (%s) but save failed\n", qPrintable(moduleName), qPrintable(moduleType));
        }

        return 0;
    }
};

class ConnectModulesCommand : public ICommand
{
public:
    QString name() const override { return "connect"; }
    QString description() const override { return "连接两个模块"; }
    QString help() const override { return "deeplux connect <project> <from_id> <to_id>"; }

    int execute(const QStringList& args, CommandContext& ctx) override
    {
        if (args.size() < 3) {
            ctx.setError("用法: connect <project> <from_module_id> <to_module_id>");
            return 1;
        }

        QString projectPath = args[0];
        QString fromId = args[1];
        QString toId = args[2];

        // 打开工程
        Project* project = nullptr;
        if (QFileInfo::exists(projectPath)) {
            project = ProjectManager::instance().openProject(projectPath);
        } else {
            ctx.setError(QString("工程文件不存在: %1").arg(projectPath));
            return 1;
        }

        if (!project) {
            ctx.setError("无法打开工程");
            return 1;
        }

        // 验证模块存在
        ModuleInstance* fromModule = project->findModule(fromId);
        ModuleInstance* toModule = project->findModule(toId);

        if (!fromModule) {
            ctx.setError(QString("源模块不存在: %1").arg(fromId));
            return 1;
        }
        if (!toModule) {
            ctx.setError(QString("目标模块不存在: %1").arg(toId));
            return 1;
        }

        // 创建连接
        ModuleConnection conn;
        conn.fromModuleId = fromId;
        conn.toModuleId = toId;

        project->addConnection(conn);

        // 保存工程
        if (project->save()) {
            printf("Connected %s -> %s\n", qPrintable(fromModule->name), qPrintable(toModule->name));
            printf("Project saved.\n");
        } else {
            printf("Connected %s -> %s (save failed)\n", qPrintable(fromModule->name), qPrintable(toModule->name));
        }

        return 0;
    }
};

class SaveProjectCommand : public ICommand
{
public:
    QString name() const override { return "save-project"; }
    QString description() const override { return "保存工程"; }
    QString help() const override { return "deeplux save-project <project> [--as <path>]"; }

    int execute(const QStringList& args, CommandContext& ctx) override
    {
        if (args.isEmpty()) {
            ctx.setError("请指定要保存的工程");
            return 1;
        }

        QString projectPath = args[0];
        QString savePath;

        // 解析选项
        for (int i = 1; i < args.size(); ++i) {
            if (args[i] == "--as" && i + 1 < args.size()) {
                savePath = args[++i];
            }
        }

        // 打开工程
        Project* project = nullptr;
        if (QFileInfo::exists(projectPath)) {
            project = ProjectManager::instance().openProject(projectPath);
        } else {
            ctx.setError(QString("工程文件不存在: %1").arg(projectPath));
            return 1;
        }

        if (!project) {
            ctx.setError("无法打开工程");
            return 1;
        }

        // 保存
        if (!savePath.isEmpty()) {
            if (project->save(savePath)) {
                printf("Project saved to: %s\n", qPrintable(savePath));
            } else {
                ctx.setError(QString("保存失败: %1").arg(savePath));
                return 1;
            }
        } else {
            if (project->save()) {
                printf("Project saved: %s\n", qPrintable(project->filePath()));
            } else {
                ctx.setError("保存失败");
                return 1;
            }
        }

        return 0;
    }
};

class ListModulesCommand : public ICommand
{
public:
    QString name() const override { return "list-modules"; }
    QString description() const override { return "列出工程中的模块"; }
    QString help() const override { return "deeplux list-modules <project>"; }

    int execute(const QStringList& args, CommandContext& ctx) override
    {
        if (args.isEmpty()) {
            ctx.setError("请指定工程文件");
            return 1;
        }

        QString projectPath = args[0];

        Project* project = nullptr;
        if (QFileInfo::exists(projectPath)) {
            project = ProjectManager::instance().openProject(projectPath);
        } else {
            ctx.setError(QString("工程文件不存在: %1").arg(projectPath));
            return 1;
        }

        if (!project) {
            ctx.setError("无法打开工程");
            return 1;
        }

        auto modules = project->modules();
        if (modules.isEmpty()) {
            printf("No modules in project.\n");
            return 0;
        }

        printf("=== Modules in %s (%d) ===\n", qPrintable(project->name()), modules.size());
        for (const auto& mod : modules) {
            printf("[%s] %s (%s) at (%d, %d)\n",
                   qPrintable(mod.id),
                   qPrintable(mod.name),
                   qPrintable(mod.moduleId),
                   mod.posX, mod.posY);
        }

        auto connections = project->connections();
        if (!connections.isEmpty()) {
            printf("\n=== Connections (%d) ===\n", connections.size());
            for (const auto& conn : connections) {
                printf("%s -> %s\n", qPrintable(conn.fromModuleId), qPrintable(conn.toModuleId));
            }
        }

        return 0;
    }
};

class ListCommandsCommand : public ICommand
{
public:
    QString name() const override { return "list-commands"; }
    QString description() const override { return "列出所有可用命令"; }
    QString help() const override { return "deeplux list-commands"; }

    int execute(const QStringList& args, CommandContext& ctx) override
    {
        Q_UNUSED(args);
        Q_UNUSED(ctx);

        printf("Available commands:\n");
        printf("  help             - 显示帮助信息\n");
        printf("  version          - 显示版本信息\n");
        printf("  info             - 显示系统信息\n");
        printf("  list-plugins     - 列出已加载插件\n");
        printf("  list-commands    - 列出所有命令\n");
        printf("  run              - 运行工程\n");
        printf("  create-project   - 创建新工程\n");
        printf("  add-module       - 添加模块到工程\n");
        printf("  connect          - 连接两个模块\n");
        printf("  save-project     - 保存工程\n");
        printf("  list-modules     - 列出工程中的模块\n");

        return 0;
    }
};

// ============== CLIHandler 实现 ==============

CLIHandler::CLIHandler(QObject* parent)
    : QObject(parent)
{
    setupCommands();
}

CLIHandler& CLIHandler::instance()
{
    static CLIHandler instance;
    return instance;
}

void CLIHandler::setupCommands()
{
    m_commands.append(new VersionCommand());
    m_commands.append(new HelpCommand());
    m_commands.append(new InfoCommand());
    m_commands.append(new ListPluginsCommand());
    m_commands.append(new ConnectCameraCommand());
    m_commands.append(new ListCommandsCommand());
    m_commands.append(new RunCommand());
    m_commands.append(new CreateProjectCommand());
    m_commands.append(new AddModuleCommand());
    m_commands.append(new ConnectModulesCommand());
    m_commands.append(new SaveProjectCommand());
    m_commands.append(new ListModulesCommand());
}

ICommand* CLIHandler::findCommand(const QString& name)
{
    for (ICommand* cmd : m_commands) {
        if (cmd->name() == name) {
            return cmd;
        }
    }
    return nullptr;
}

bool CLIHandler::parse(const QStringList& args)
{
    if (args.size() < 2) {
        // 无参数，默认 GUI 模式
        m_cliOnly = false;
        return true;
    }

    // 跳过第一个参数（程序名）
    QStringList remaining = args.mid(1);

    // 检查是否为 GUI 模式保留的参数
    if (remaining.contains("--gui") || remaining.contains("-g")) {
        m_cliOnly = false;
        remaining.removeAll("--gui");
        remaining.removeAll("-g");
    }

    // 检查是否为纯 CLI 模式
    if (remaining.contains("--cli") || remaining.contains("-c")) {
        m_cliOnly = true;
        remaining.removeAll("--cli");
        remaining.removeAll("-c");
    }

    if (remaining.isEmpty()) {
        m_cliOnly = false;
        return true;
    }

    // 解析通用选项
    if (remaining.contains("--verbose") || remaining.contains("-v")) {
        m_verbose = true;
        remaining.removeAll("--verbose");
        remaining.removeAll("-v");
    }

    // 解析 --help 和 --version（独立选项）
    QString first = remaining.first();
    if (first == "--help" || first == "-h") {
        m_command = "help";
        remaining.removeFirst();
        m_commandArgs = remaining;
        return true;
    }

    if (first == "--version" || first == "-V") {
        m_command = "version";
        remaining.removeFirst();
        m_commandArgs = remaining;
        return true;
    }

    // 解析子命令
    m_command = remaining.first();
    remaining.removeFirst();
    m_commandArgs = remaining;

    // 检查命令是否存在
    if (!findCommand(m_command)) {
        m_errorString = QString("未知命令: %1").arg(m_command);
        return false;
    }

    return true;
}

QString CLIHandler::helpText() const
{
    return QString(
        "DeepLux Vision %1\n"
        "\n"
        "用法: deeplux [选项] [命令] [参数]\n"
        "\n"
        "选项:\n"
        "  -c, --cli          纯 CLI 模式（无 GUI）\n"
        "  -g, --gui          强制 GUI 模式\n"
        "  -v, --verbose      详细输出\n"
        "  -h, --help         显示帮助\n"
        "  -V, --version      显示版本\n"
        "\n"
        "命令:\n"
        "  help [command]         显示帮助\n"
        "  version                显示版本信息\n"
        "  info                   显示系统信息\n"
        "  list-plugins          列出已加载插件\n"
        "  list-commands         列出所有命令\n"
        "  run <file>            运行工程\n"
        "  create-project <name> 创建新工程\n"
        "  add-module <proj> <type> [--name <n>] [--pos <x,y>]  添加模块\n"
        "  connect <proj> <from> <to>  连接两个模块\n"
        "  save-project <proj> [--as <path>]  保存工程\n"
        "  list-modules <proj>    列出工程中的模块\n"
        "\n"
        "示例:\n"
        "  deeplux --version\n"
        "  deeplux list-plugins\n"
        "  deeplux create-project MyProject\n"
        "  deeplux add-module project.dproj GrabImage --name \"Grab1\" --pos 100,100\n"
        "  deeplux list-modules project.dproj\n"
        "  deeplux save-project project.dproj --as output.dproj\n"
    ).arg(DEEPLUX_VERSION_STRING);
}

QString CLIHandler::versionText() const
{
    return QString("DeepLux Vision %1").arg(DEEPLUX_VERSION_STRING);
}

} // namespace DeepLux
