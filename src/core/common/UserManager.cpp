#include "UserManager.h"
#include "Logger.h"
#include <QDebug>

namespace DeepLux {

UserManager& UserManager::instance()
{
    static UserManager instance;
    return instance;
}

UserManager::UserManager()
{
    // 默认以操作员身份登录
    m_currentUser.userName = "Operator";
    m_currentUser.type = UserType::Operator;
    m_currentUser.isLoggedIn = true;
    m_currentUser.loginTime = QDateTime::currentDateTime();
    updatePermissions();
}

UserManager::~UserManager()
{
}

bool UserManager::login(const QString& userName, const QString& password)
{
    Q_UNUSED(password)

    // 简化逻辑：根据用户名确定用户类型
    if (userName.toLower() == "developer" || userName.toLower() == "dev") {
        m_currentUser.userName = userName;
        m_currentUser.type = UserType::Developer;
    } else if (userName.toLower() == "admin" || userName.toLower() == "administrator") {
        m_currentUser.userName = userName;
        m_currentUser.type = UserType::Administrator;
    } else if (userName.toLower() == "operator" || userName.toLower() == "op") {
        m_currentUser.userName = userName;
        m_currentUser.type = UserType::Operator;
    } else {
        m_currentUser.userName = userName;
        m_currentUser.type = UserType::Guest;
    }

    m_currentUser.isLoggedIn = true;
    m_currentUser.loginTime = QDateTime::currentDateTime();
    updatePermissions();

    emit userLoggedIn(m_currentUser);
    Logger::instance().info(QString("User logged in: %1").arg(userName), "Auth");
    return true;
}

void UserManager::logout()
{
    QString userName = m_currentUser.userName;
    m_currentUser.userName.clear();
    m_currentUser.type = UserType::Guest;
    m_currentUser.isLoggedIn = false;
    updatePermissions();

    emit userLoggedOut();
    Logger::instance().info(QString("User logged out: %1").arg(userName), "Auth");
}

bool UserManager::hasPermission(const QString& permission) const
{
    if (permission == "CameraSet") return m_permissions.cameraSet;
    if (permission == "QuickMode") return m_permissions.quickMode;
    if (permission == "HardwareConfig") return m_permissions.hardwareConfig;
    if (permission == "CommunicationSet") return m_permissions.communicationSet;
    if (permission == "Camera") return m_permissions.camera;
    if (permission == "Barcode") return m_permissions.barcode;
    if (permission == "ServoDebug") return m_permissions.servoDebug;
    if (permission == "IODebug") return m_permissions.ioDebug;
    if (permission == "Home") return m_permissions.home;
    if (permission == "Open") return m_permissions.open;
    if (permission == "Edit") return m_permissions.edit;
    if (permission == "Save") return m_permissions.save;
    if (permission == "RunOnce") return m_permissions.runOnce;
    if (permission == "RunCycle") return m_permissions.runCycle;
    if (permission == "Stop") return m_permissions.stop;
    if (permission == "OpenFile") return m_permissions.openFile;
    if (permission == "SaveFile") return m_permissions.saveFile;
    if (permission == "SystemConfig") return m_permissions.systemConfig;
    if (permission == "DeviceParam") return m_permissions.deviceParam;
    if (permission == "SystemParam") return m_permissions.systemParam;
    if (permission == "View") return m_permissions.view;
    if (permission == "NewSolution") return m_permissions.newSolution;
    if (permission == "SolutionList") return m_permissions.solutionList;
    if (permission == "GlobalVar") return m_permissions.globalVar;
    if (permission == "UIDesign") return m_permissions.uiDesign;
    return false;
}

void UserManager::updatePermissions()
{
    switch (m_currentUser.type) {
        case UserType::Developer:
            m_permissions.cameraSet = true;
            m_permissions.quickMode = true;
            m_permissions.hardwareConfig = true;
            m_permissions.communicationSet = true;
            m_permissions.camera = true;
            m_permissions.barcode = true;
            m_permissions.servoDebug = true;
            m_permissions.ioDebug = true;
            m_permissions.home = true;
            m_permissions.open = true;
            m_permissions.edit = true;
            m_permissions.save = true;
            m_permissions.runOnce = true;
            m_permissions.runCycle = true;
            m_permissions.stop = true;
            m_permissions.openFile = true;
            m_permissions.saveFile = true;
            m_permissions.systemConfig = true;
            m_permissions.deviceParam = true;
            m_permissions.systemParam = true;
            m_permissions.view = true;
            m_permissions.newSolution = true;
            m_permissions.solutionList = true;
            m_permissions.globalVar = true;
            m_permissions.uiDesign = true;
            break;

        case UserType::Administrator:
            m_permissions.cameraSet = false;
            m_permissions.quickMode = true;
            m_permissions.hardwareConfig = false;
            m_permissions.communicationSet = false;
            m_permissions.camera = false;
            m_permissions.barcode = false;
            m_permissions.servoDebug = true;
            m_permissions.ioDebug = true;
            m_permissions.home = true;
            m_permissions.open = true;
            m_permissions.edit = true;
            m_permissions.save = true;
            m_permissions.runOnce = true;
            m_permissions.runCycle = true;
            m_permissions.stop = true;
            m_permissions.openFile = true;
            m_permissions.saveFile = true;
            m_permissions.systemConfig = true;
            m_permissions.deviceParam = true;
            m_permissions.systemParam = true;
            m_permissions.view = true;
            m_permissions.newSolution = false;
            m_permissions.solutionList = true;
            m_permissions.globalVar = false;
            m_permissions.uiDesign = true;
            break;

        case UserType::Operator:
            m_permissions.cameraSet = false;
            m_permissions.quickMode = false;
            m_permissions.hardwareConfig = false;
            m_permissions.communicationSet = false;
            m_permissions.camera = false;
            m_permissions.barcode = false;
            m_permissions.servoDebug = false;
            m_permissions.ioDebug = true;
            m_permissions.home = true;
            m_permissions.open = true;
            m_permissions.edit = false;
            m_permissions.save = false;
            m_permissions.runOnce = true;
            m_permissions.runCycle = true;
            m_permissions.stop = true;
            m_permissions.openFile = true;
            m_permissions.saveFile = false;
            m_permissions.systemConfig = false;
            m_permissions.deviceParam = false;
            m_permissions.systemParam = false;
            m_permissions.view = false;
            m_permissions.newSolution = false;
            m_permissions.solutionList = false;
            m_permissions.globalVar = false;
            m_permissions.uiDesign = false;
            break;

        case UserType::Guest:
        default:
            // 访客权限最小化
            m_permissions.runOnce = true;
            m_permissions.runCycle = true;
            m_permissions.stop = true;
            break;
    }

    emit permissionChanged();
}

} // namespace DeepLux