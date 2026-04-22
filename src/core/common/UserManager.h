#pragma once

#include <QObject>
#include <QString>
#include <QDateTime>

namespace DeepLux {

/**
 * @brief 用户类型
 */
enum class UserType {
    Developer,      // 开发者
    Administrator, // 管理员
    Operator,      // 操作员
    Guest          // 访客
};

/**
 * @brief 用户信息
 */
struct UserInfo {
    QString id;
    QString userName;
    UserType type;
    QDateTime loginTime;
    bool isLoggedIn = false;
};

/**
 * @brief 权限控制项
 */
struct Permission {
    bool cameraSet = false;           // 相机设置
    bool quickMode = false;           // 快速模式
    bool hardwareConfig = false;       // 硬件配置
    bool communicationSet = false;     // 通讯设置
    bool camera = false;              // 相机
    bool barcode = false;              // 条码
    bool servoDebug = false;           // 伺服调试
    bool ioDebug = false;             // IO调试
    bool home = false;                // 回原点
    bool open = false;                // 打开
    bool edit = false;                // 编辑
    bool save = false;                // 保存
    bool runOnce = true;              // 单次运行
    bool runCycle = true;             // 循环运行
    bool stop = true;                 // 停止
    bool openFile = false;            // 打开文件
    bool saveFile = false;            // 保存文件
    bool systemConfig = false;        // 系统配置
    bool deviceParam = false;         // 设备参数
    bool systemParam = false;         // 系统参数
    bool view = false;                // 视图
    bool newSolution = false;         // 新建方案
    bool solutionList = false;        // 方案列表
    bool globalVar = false;           // 全局变量
    bool uiDesign = false;           // UI设计
};

/**
 * @brief 用户管理器
 */
class UserManager : public QObject
{
    Q_OBJECT

public:
    static UserManager& instance();

    // 用户登录
    bool login(const QString& userName, const QString& password);
    void logout();

    // 当前用户
    UserInfo currentUser() const { return m_currentUser; }
    bool isLoggedIn() const { return m_currentUser.isLoggedIn; }

    // 权限检查
    bool hasPermission(const QString& permission) const;
    Permission permissions() const { return m_permissions; }

    // 用户类型检查
    bool isDeveloper() const { return m_currentUser.type == UserType::Developer; }
    bool isAdministrator() const { return m_currentUser.type == UserType::Administrator; }
    bool isOperator() const { return m_currentUser.type == UserType::Operator; }

signals:
    void userLoggedIn(const UserInfo& user);
    void userLoggedOut();
    void permissionChanged();

private:
    UserManager();
    ~UserManager();

    void updatePermissions();

    UserInfo m_currentUser;
    Permission m_permissions;
};

} // namespace DeepLux