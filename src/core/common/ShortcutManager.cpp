#include "ShortcutManager.h"
#include "common/Logger.h"
#include <QKeyEvent>

namespace DeepLux {

ShortcutManager& ShortcutManager::instance()
{
    static ShortcutManager instance;
    return instance;
}

ShortcutManager::ShortcutManager()
{
    // 注册默认快捷键
    registerShortcut(ShortcutId::Save, "Save", "保存", ShortcutType::Ctrl, Qt::Key_S);
    registerShortcut(ShortcutId::Open, "Open", "打开", ShortcutType::Ctrl, Qt::Key_O);
    registerShortcut(ShortcutId::NewProject, "NewProject", "新建项目", ShortcutType::Ctrl, Qt::Key_N);
    registerShortcut(ShortcutId::Edit, "Edit", "编辑", ShortcutType::Ctrl, Qt::Key_E);
    registerShortcut(ShortcutId::RunOnce, "RunOnce", "单次运行", ShortcutType::None, Qt::Key_F5);
    registerShortcut(ShortcutId::RunCycle, "RunCycle", "循环运行", ShortcutType::None, Qt::Key_F6);
    registerShortcut(ShortcutId::Stop, "Stop", "停止", ShortcutType::None, Qt::Key_Escape);
    registerShortcut(ShortcutId::ScreenCapture, "ScreenCapture", "截图", ShortcutType::None, Qt::Key_F8);
    registerShortcut(ShortcutId::Help, "Help", "帮助", ShortcutType::None, Qt::Key_F9);
    registerShortcut(ShortcutId::StepRun, "StepRun", "步进运行", ShortcutType::None, Qt::Key_F10);
    registerShortcut(ShortcutId::QuickMode, "QuickMode", "快速模式", ShortcutType::Ctrl, Qt::Key_Q);

    Logger::instance().debug("Shortcut manager initialized", "Shortcut");
}

ShortcutManager::~ShortcutManager()
{
}

void ShortcutManager::registerShortcut(ShortcutId id, const QString& name,
                                        const QString& description,
                                        ShortcutType type, int key)
{
    ShortcutItem item;
    item.id = id;
    item.name = name;
    item.description = description;
    item.type = type;
    item.key = key;
    item.enabled = true;

    m_shortcuts[id] = item;
}

void ShortcutManager::unregisterShortcut(ShortcutId id)
{
    m_shortcuts.remove(id);
}

QList<ShortcutItem> ShortcutManager::shortcuts() const
{
    return m_shortcuts.values();
}

ShortcutItem* ShortcutManager::findShortcut(ShortcutId id)
{
    if (m_shortcuts.contains(id)) {
        return &m_shortcuts[id];
    }
    return nullptr;
}

bool ShortcutManager::matches(ShortcutId id, ShortcutType type, int key) const
{
    if (!m_shortcuts.contains(id)) {
        return false;
    }

    const ShortcutItem& item = m_shortcuts[id];
    return item.type == type && item.key == key && item.enabled;
}

void ShortcutManager::setEnabled(ShortcutId id, bool enabled)
{
    if (m_shortcuts.contains(id)) {
        m_shortcuts[id].enabled = enabled;
        emit shortcutChanged(id);
    }
}

bool ShortcutManager::isEnabled(ShortcutId id) const
{
    if (m_shortcuts.contains(id)) {
        return m_shortcuts[id].enabled;
    }
    return false;
}

void ShortcutManager::resetToDefaults()
{
    m_shortcuts.clear();

    // 重新注册默认快捷键
    registerShortcut(ShortcutId::Save, "Save", "保存", ShortcutType::Ctrl, Qt::Key_S);
    registerShortcut(ShortcutId::Open, "Open", "打开", ShortcutType::Ctrl, Qt::Key_O);
    registerShortcut(ShortcutId::NewProject, "NewProject", "新建项目", ShortcutType::Ctrl, Qt::Key_N);
    registerShortcut(ShortcutId::Edit, "Edit", "编辑", ShortcutType::Ctrl, Qt::Key_E);
    registerShortcut(ShortcutId::RunOnce, "RunOnce", "单次运行", ShortcutType::None, Qt::Key_F5);
    registerShortcut(ShortcutId::RunCycle, "RunCycle", "循环运行", ShortcutType::None, Qt::Key_F6);
    registerShortcut(ShortcutId::Stop, "Stop", "停止", ShortcutType::None, Qt::Key_Escape);
    registerShortcut(ShortcutId::ScreenCapture, "ScreenCapture", "截图", ShortcutType::None, Qt::Key_F8);
    registerShortcut(ShortcutId::Help, "Help", "帮助", ShortcutType::None, Qt::Key_F9);
    registerShortcut(ShortcutId::StepRun, "StepRun", "步进运行", ShortcutType::None, Qt::Key_F10);
    registerShortcut(ShortcutId::QuickMode, "QuickMode", "快速模式", ShortcutType::Ctrl, Qt::Key_Q);

    Logger::instance().info("Shortcuts reset to defaults", "Shortcut");
}

ShortcutType ShortcutManager::getShortcutType(Qt::KeyboardModifiers modifiers) const
{
    bool ctrl = modifiers & Qt::ControlModifier;
    bool alt = modifiers & Qt::AltModifier;
    bool shift = modifiers & Qt::ShiftModifier;

    if (ctrl && shift) return ShortcutType::CtrlShift;
    if (ctrl && alt) return ShortcutType::CtrlAlt;
    if (alt && shift) return ShortcutType::AltShift;
    if (ctrl) return ShortcutType::Ctrl;
    if (alt) return ShortcutType::Alt;
    if (shift) return ShortcutType::Shift;
    return ShortcutType::None;
}

int ShortcutManager::getKeyValue(Qt::Key key) const
{
    return key;
}

} // namespace DeepLux