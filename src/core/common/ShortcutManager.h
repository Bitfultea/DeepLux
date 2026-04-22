#pragma once

#include <QObject>
#include <QMap>
#include <QKeySequence>
#include <QVector>

namespace DeepLux {

/**
 * @brief 快捷键类型
 */
enum class ShortcutType {
    None,
    Ctrl,
    Alt,
    Shift,
    CtrlShift,
    AltShift,
    CtrlAlt
};

/**
 * @brief 快捷键标识
 */
enum class ShortcutId {
    Save,               // Ctrl+S
    Open,               // Ctrl+O
    NewProject,         // Ctrl+N
    Edit,               // Ctrl+E
    RunOnce,             // F5
    RunCycle,            // F6
    Stop,               // Escape
    ScreenCapture,       // F8
    Help,                // F9
    StepRun,            // F10
    QuickMode           // Ctrl+Q
};

/**
 * @brief 快捷键项
 */
struct ShortcutItem {
    ShortcutId id;
    QString name;
    QString description;
    ShortcutType type;
    int key;
    bool enabled;
};

/**
 * @brief 快捷键管理器
 */
class ShortcutManager : public QObject
{
    Q_OBJECT

public:
    static ShortcutManager& instance();

    // 注册/注销快捷键
    void registerShortcut(ShortcutId id, const QString& name, const QString& description,
                          ShortcutType type, int key);
    void unregisterShortcut(ShortcutId id);

    // 快捷键列表
    QList<ShortcutItem> shortcuts() const;
    ShortcutItem* findShortcut(ShortcutId id);

    // 检查快捷键
    bool matches(ShortcutId id, ShortcutType type, int key) const;

    // 启用/禁用
    void setEnabled(ShortcutId id, bool enabled);
    bool isEnabled(ShortcutId id) const;

    // 重置为默认
    void resetToDefaults();

signals:
    void shortcutTriggered(ShortcutId id);
    void shortcutChanged(ShortcutId id);

private:
    ShortcutManager();
    ~ShortcutManager();

    ShortcutType getShortcutType(Qt::KeyboardModifiers modifiers) const;
    int getKeyValue(Qt::Key key) const;

    QMap<ShortcutId, ShortcutItem> m_shortcuts;
};

} // namespace DeepLux