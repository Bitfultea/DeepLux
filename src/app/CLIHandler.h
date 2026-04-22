#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace DeepLux {

class CommandContext;

/**
 * @brief CLI 命令接口
 */
class ICommand
{
public:
    virtual ~ICommand() = default;

    // 命令名称
    virtual QString name() const = 0;

    // 命令描述
    virtual QString description() const = 0;

    // 命令帮助
    virtual QString help() const = 0;

    // 执行命令
    virtual int execute(const QStringList& args, CommandContext& ctx) = 0;
};

/**
 * @brief CLI 执行上下文
 */
class CommandContext
{
public:
    bool isGuiMode() const { return m_guiMode; }
    void setGuiMode(bool mode) { m_guiMode = mode; }

    QString projectPath() const { return m_projectPath; }
    void setProjectPath(const QString& path) { m_projectPath = path; }

    bool verbose() const { return m_verbose; }
    void setVerbose(bool v) { m_verbose = v; }

    QString errorString() const { return m_errorString; }
    void setError(const QString& err) { m_errorString = err; }
    bool hasError() const { return !m_errorString.isEmpty(); }

    QVariantMap& data() { return m_data; }
    const QVariantMap& data() const { return m_data; }

private:
    bool m_guiMode = false;
    QString m_projectPath;
    bool m_verbose = false;
    QString m_errorString;
    QVariantMap m_data;
};

/**
 * @brief CLI 处理器
 */
class CLIHandler : public QObject
{
    Q_OBJECT

public:
    static CLIHandler& instance();

    // 解析命令行参数
    bool parse(const QStringList& args);

    // 是否为 CLI 模式（无 GUI）
    bool isCLIOnly() const { return m_cliOnly; }

    // 获取要执行的命令
    QString command() const { return m_command; }

    // 获取命令参数
    QStringList commandArgs() const { return m_commandArgs; }

    // 获取错误信息
    QString errorString() const { return m_errorString; }

    // 显示帮助
    QString helpText() const;

    // 显示版本
    QString versionText() const;

    // 是否详细输出
    bool verbose() const { return m_verbose; }

    // 查找命令
    ICommand* findCommand(const QString& name);

signals:
    void parseCompleted();

private:
    explicit CLIHandler(QObject* parent = nullptr);
    ~CLIHandler() = default;

    CLIHandler(const CLIHandler&) = delete;
    CLIHandler& operator=(const CLIHandler&) = delete;

    void setupCommands();

    bool m_cliOnly = false;
    bool m_verbose = false;
    QString m_command;
    QStringList m_commandArgs;
    QString m_errorString;

    QList<ICommand*> m_commands;
};

} // namespace DeepLux
