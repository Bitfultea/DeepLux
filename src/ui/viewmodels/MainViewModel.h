#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QDateTime>

namespace DeepLux {

class ProjectManager;

/**
 * @brief 主视图模型
 */
class MainViewModel : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString currentTime READ currentTime NOTIFY currentTimeChanged)
    Q_PROPERTY(QString projectName READ projectName NOTIFY projectNameChanged)
    Q_PROPERTY(bool modified READ isModified NOTIFY modifiedChanged)

public:
    static MainViewModel& instance();

    QString currentTime() const;
    QString projectName() const;
    bool isModified() const;

    Q_INVOKABLE void newProject();
    Q_INVOKABLE bool openProject(const QString& path);
    Q_INVOKABLE bool saveProject(const QString& path = QString());
    Q_INVOKABLE void runProject();
    Q_INVOKABLE void stopProject();

signals:
    void currentTimeChanged();
    void projectNameChanged();
    void modifiedChanged();
    void projectLoaded();
    void projectSaved();
    void errorOccurred(const QString& error);

private:
    MainViewModel();
    ~MainViewModel();

    void updateCurrentTime();

    QString m_projectName;
    QString m_projectPath;
    bool m_modified = false;
    QTimer* m_timer = nullptr;
    QString m_currentTime;
};

} // namespace DeepLux