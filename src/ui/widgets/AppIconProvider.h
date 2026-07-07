#pragma once

#include <QColor>
#include <QIcon>

namespace DeepLux {

class AppIconProvider {
public:
    enum class Icon {
        NewFile,
        OpenFolder,
        Save,
        List,
        Play,
        Pause,
        Stop,
        Cycle,
        QuickMode,
        User,
        Variable,
        Camera,
        Communication,
        Hardware,
        Report,
        Home,
        Design,
        Laser,
        Theme,
        Add,
        Delete,
        Refresh,
        Connect,
        Disconnect,
        MoveUp,
        MoveDown,
        Execute,
        Clear,
        Undo,
        LoadImage,
        Confirm,
        Cancel,
        FitWindow,
        ActualSize,
        ZoomIn,
        ZoomOut,
        Close,
        Image,
        PointCloud,
        Folder,
        Copy,
        Settings,
        Agent,
        TestConnection
    };

    static QIcon icon(Icon icon, int size = 24, const QColor& color = QColor("#374151"));
};

} // namespace DeepLux
