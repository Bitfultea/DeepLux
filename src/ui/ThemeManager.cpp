#include "ThemeManager.h"

namespace DeepLux {

QString ThemeManager::styleSheet(bool isDark) {
    if (isDark) {
        return QStringLiteral(
            "QMainWindow { background-color: #1e1e1e; color: #ffffff; }"
            "QWidget#MainContentWidget { background-color: #1e1e1e; }"
            "QSplitter#MainSplitter { background-color: #1e1e1e; }"
            "QDockWidget { background-color: #252525; color: #ffffff; border: none; }"
            "QDockWidget::title { background-color: #2d2d2d; color: #ffffff; font-weight: bold; "
            "font-size: 13px; padding: 8px 10px; border-bottom: 1px solid #444444; }"
            "QDockWidget::title:hover { background-color: #333333; }"
            "QTreeWidget { background-color: #252525; color: #ffffff; border: none; font-size: 13px; }"
            "QTreeWidget::item { height: 28px; padding-left: 2px; padding-right: 2px; }"
            "QTreeWidget::item:hover { background-color: #3a3a3a; }"
            "QTreeWidget::item:selected { background-color: #0078d7; }"
            "QTableWidget { background-color: #252525; color: #ffffff; border: none; font-size: 13px; }"
            "QTableWidget::item { border-bottom: 1px solid #333; }"
            "QTableWidget::item:selected { background-color: #0078d7; }"
            "QHeaderView::section { background-color: #333333; padding: 5px; border: none; font-size: 13px; }"
            "QTableCornerButton::section { background-color: #333333; border: none; }"
            "QScrollBar:vertical { background-color: #252525; width: 12px; border: none; }"
            "QScrollBar::handle:vertical { background-color: #555555; min-height: 20px; border-radius: 6px; }"
            "QScrollBar::handle:vertical:hover { background-color: #666666; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }"
            "QScrollBar:horizontal { background-color: #252525; height: 12px; border: none; }"
            "QScrollBar::handle:horizontal { background-color: #555555; min-width: 20px; border-radius: 6px; }"
            "QScrollBar::handle:horizontal:hover { background-color: #666666; }"
            "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; }"
            "QToolBar { background-color: #252525; border: 1px solid #444444; spacing: 5px; padding: 2px; }"
            "QToolBar QToolButton { background-color: transparent; color: #ffffff; padding: 5px; "
            "border: 1px solid transparent; }"
            "QToolBar QToolButton:hover { background-color: #3a3a3a; border: 1px solid #555555; }"
            "QToolBar QToolButton:pressed { background-color: #444444; border: 1px solid #6b7280; }"
            "QToolBar QToolButton:checked { background-color: #0e7490; color: #ffffff; border: 1px solid #06b6d4; }"
            "QMenuBar { background-color: #252525; color: #ffffff; }"
            "QMenuBar::item:selected { background-color: #3a3a3a; }"
            "QMenu { background-color: #252525; color: #ffffff; border: 1px solid #333; }"
            "QMenu::item:selected { background-color: #0078d7; }"
            "QStatusBar { background-color: #252525; color: #ffffff; font-size: 13px; }"
            "QStatusBar QLabel { padding: 0 8px; }"
            "QPushButton { background-color: #0078d7; color: white; padding: 5px 15px; border: none; }"
            "QPushButton:hover { background-color: #1e8ad6; }"
            "QPushButton:disabled { background-color: #555555; }"
            "QLineEdit { background-color: #333333; color: white; border: 1px solid #555; padding: 5px; }"
            "QComboBox { background-color: #333333; color: white; border: 1px solid #555; padding: 5px; }"
            "QSpinBox { background-color: #333333; color: white; border: 1px solid #555; padding: 5px; }"
            "QSplitter#MainSplitter::handle { background-color: #1e1e1e; border: none; }"
            "QSplitter#RightTopSplitter::handle:horizontal { "
            "background-color: #30363d; border-left: 1px solid #3f4750; border-right: 1px solid #252b31; }"
            "QSplitter#RightSplitter::handle:vertical { "
            "background-color: #30363d; border-top: 1px solid #3f4750; border-bottom: 1px solid #252b31; }"
            "QWidget#ProcessPanelWidget { background-color: #252525; border-right: 1px solid #3b4148; }"
            "QWidget#ImageDisplayWidget { background-color: #1e1e1e; border-left: 1px solid #3b4148; "
            "border-bottom: 1px solid #3b4148; }"
            "QDockWidget#LogDock { border-top: 1px solid #3b4148; }"
            "QLabel { color: #ffffff; }"
            "QScrollArea { background-color: #252525; }"
            "QFrame { background-color: transparent; }"
            "QTabWidget::pane { border: none; background-color: #252525; }"
            "QTabBar { background-color: #252525; }"
            "QTabBar::tab { background-color: #333333; color: #ffffff; font-size: 13px; font-weight: 500; "
            "min-height: 26px; padding: 3px 10px; border: none; }"
            "QTabBar::tab:selected { background-color: #444444; }"
            "QTabBar::tab:hover:!selected { background-color: #3a3a3a; }");
    }
    return QStringLiteral(
        "QMainWindow { background-color: #f5f5f5; color: #212121; }"
        "QWidget#MainContentWidget { background-color: #f5f5f5; }"
        "QSplitter#MainSplitter { background-color: #f5f5f5; }"
        "QDockWidget { background-color: #ffffff; color: #212121; border: none; }"
        "QDockWidget::title { background-color: #e8e8e8; color: #212121; font-weight: bold; "
        "font-size: 13px; padding: 8px 10px; border-bottom: 1px solid #cccccc; }"
        "QDockWidget::title:hover { background-color: #d0d0d0; }"
        "QTreeWidget { background-color: #ffffff; color: #212121; border: 1px solid #dddddd; font-size: 13px; }"
        "QTreeWidget::item { height: 28px; padding-left: 2px; padding-right: 2px; }"
        "QTreeWidget::item:hover { background-color: #e5f3ff; }"
        "QTreeWidget::item:selected { background-color: #0078d7; color: #ffffff; }"
        "QTableWidget { background-color: #ffffff; color: #212121; border: 1px solid #dddddd; font-size: 13px; }"
        "QTableWidget::item { border-bottom: 1px solid #eeeeee; }"
        "QTableWidget::item:selected { background-color: #0078d7; color: #ffffff; }"
        "QHeaderView::section { background-color: #f0f0f0; color: #212121; padding: 5px; "
        "border: 1px solid #dddddd; font-size: 13px; }"
        "QTableCornerButton::section { background-color: #f0f0f0; border: none; }"
        "QScrollBar:vertical { background-color: #f0f0f0; width: 12px; border: none; }"
        "QScrollBar::handle:vertical { background-color: #c0c0c0; min-height: 20px; border-radius: 6px; }"
        "QScrollBar::handle:vertical:hover { background-color: #a0a0a0; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }"
        "QScrollBar:horizontal { background-color: #f0f0f0; height: 12px; border: none; }"
        "QScrollBar::handle:horizontal { background-color: #c0c0c0; min-width: 20px; border-radius: 6px; }"
        "QScrollBar::handle:horizontal:hover { background-color: #a0a0a0; }"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; }"
        "QToolBar { background-color: #f8f8f8; border: 1px solid #dddddd; spacing: 5px; padding: 2px; }"
        "QToolBar QToolButton { background-color: transparent; color: #212121; padding: 5px; "
        "border: 1px solid transparent; }"
        "QToolBar QToolButton:hover { background-color: #e5f3ff; border: 1px solid #0078d7; }"
        "QToolBar QToolButton:checked { background-color: #CCEBF2; color: #0C4A6E; border: 1px solid #0891B2; }"
        "QMenuBar { background-color: #f8f8f8; color: #212121; }"
        "QMenuBar::item:selected { background-color: #e5f3ff; }"
        "QMenu { background-color: #ffffff; color: #212121; border: 1px solid #cccccc; }"
        "QMenu::item:selected { background-color: #0078d7; color: #ffffff; }"
        "QStatusBar { background-color: #f8f8f8; color: #212121; font-size: 13px; }"
        "QStatusBar QLabel { padding: 0 8px; }"
        "QPushButton { background-color: #0078d7; color: white; padding: 5px 15px; border: 1px solid #005a9e; }"
        "QPushButton:hover { background-color: #1e8ad6; }"
        "QPushButton:disabled { background-color: #cccccc; color: #999999; }"
        "QLineEdit { background-color: #ffffff; color: #212121; border: 1px solid #cccccc; padding: 5px; }"
        "QLineEdit:focus { border: 1px solid #0078d7; }"
        "QComboBox { background-color: #ffffff; color: #212121; border: 1px solid #cccccc; padding: 5px; }"
        "QComboBox:focus { border: 1px solid #0078d7; }"
        "QSpinBox { background-color: #ffffff; color: #212121; border: 1px solid #cccccc; padding: 5px; }"
        "QSpinBox:focus { border: 1px solid #0078d7; }"
        "QSplitter#MainSplitter::handle { background-color: #f5f5f5; border: none; }"
        "QSplitter#RightTopSplitter::handle:horizontal { "
        "background-color: #e4e8ed; border-left: 1px solid #d2d8e0; border-right: 1px solid #f7f8fa; }"
        "QSplitter#RightSplitter::handle:vertical { "
        "background-color: #e4e8ed; border-top: 1px solid #d2d8e0; border-bottom: 1px solid #f7f8fa; }"
        "QWidget#ProcessPanelWidget { background-color: #ffffff; border-right: 1px solid #dce2e8; }"
        "QWidget#ImageDisplayWidget { background-color: #ffffff; border-left: 1px solid #dce2e8; "
        "border-bottom: 1px solid #dce2e8; }"
        "QDockWidget#LogDock { border-top: 1px solid #dce2e8; }"
        "QLabel { color: #212121; }"
        "QScrollArea { background-color: #ffffff; }"
        "QFrame { background-color: transparent; }"
        "QTabWidget::pane { border: none; background-color: #ffffff; }"
        "QTabBar { background-color: #ffffff; }"
        "QTabBar::tab { background-color: #e8e8e8; color: #212121; font-size: 13px; font-weight: 500; "
        "min-height: 26px; padding: 3px 10px; border: none; }"
        "QTabBar::tab:selected { background-color: #f5f5f5; }"
        "QTabBar::tab:hover:!selected { background-color: #d0d0d0; }");
}

ThemePalette ThemeManager::palette(bool isDark) {
    if (isDark) {
        return {
            QStringLiteral("#2d2d2d"), QStringLiteral("#444444"), QStringLiteral("#ffffff"),
            QStringLiteral("#ffffff"), QStringLiteral("#252525"), QStringLiteral("#ffffff"),
            QStringLiteral("#3a3a3a"), QStringLiteral("#252525"), QStringLiteral("#3b4148"),
            QStringLiteral("#3a3a3a"), QStringLiteral("#ffffff"), QStringLiteral("#4a4a4a"),
            QStringLiteral("#555555"), QStringLiteral("#252525"), QStringLiteral("#333333"),
            QStringLiteral("#ffffff"), QStringLiteral("#3b4148"), QStringLiteral("#3a3a3a"),
            QStringLiteral("#252525"), QStringLiteral("#a0a0a0"), QStringLiteral("#252525"),
            QStringLiteral("#333333"), QStringLiteral("#d1d5db"), QStringLiteral("#ffffff"),
            QStringLiteral("#3a3a3a"), QStringLiteral("#1e1e1e")
        };
    }
    return {
        QStringLiteral("#e8e8e8"), QStringLiteral("#cccccc"), QStringLiteral("#212121"),
        QStringLiteral("#212121"), QStringLiteral("#ffffff"), QStringLiteral("#212121"),
        QStringLiteral("#e5f3ff"), QStringLiteral("#ffffff"), QStringLiteral("#dce2e8"),
        QStringLiteral("#e0e0e0"), QStringLiteral("#212121"), QStringLiteral("#d0d0d0"),
        QStringLiteral("#cccccc"), QStringLiteral("#ffffff"), QStringLiteral("#f0f0f0"),
        QStringLiteral("#212121"), QStringLiteral("#dce2e8"), QStringLiteral("#e0e0e0"),
        QStringLiteral("#ffffff"), QStringLiteral("#666666"), QStringLiteral("#ffffff"),
        QStringLiteral("#e8e8e8"), QStringLiteral("#4b5563"), QStringLiteral("#212121"),
        QStringLiteral("#dce2e8"), QStringLiteral("#ffffff")
    };
}

} // namespace DeepLux
