#include "ThemeManager.h"

namespace DeepLux {

LayoutMetrics ThemeManager::layoutMetrics() {
    return LayoutMetrics{};
}

namespace {

/// 辅助函数：将整数转字符串并附加 "px"
inline QString px(int v) {
    return QString::number(v) + "px";
}

/// 构建深/浅色主题的通用样式块（不含 ModuleInspector 专用样式）
QString buildBaseStyleSheet(bool isDark, const LayoutMetrics& m) {
    // 通用尺寸常量
    const QString baseFont = px(m.baseFontSize);
    const QString headerFont = px(m.headerFontSize);
    const QString borderW = px(m.borderWidth);
    const QString splitterW = px(m.splitterHandleWidth);
    const QString baseSpacing = px(m.baseSpacing);

    // 深色 / 浅色色组
    QString mainWindowBg, splitterBg;
    QString dockBg, dockTitleBg, dockTitleFg, dockTitleBorder, dockTitleHover;
    QString treeBg, treeFg, treeHover;
    QString tableBg, tableFg, tableItemBorder;
    QString headerBg, headerFg;
    QString scrollBg, scrollHandle, scrollHandleHover;
    QString toolBarBg, toolBarBorder;
    QString toolBtnFg, toolBtnHoverBg, toolBtnHoverBorder, toolBtnCheckedBg, toolBtnCheckedBorder;
    QString menuBarBg, menuBarFg, menuBg, menuFg, menuSelBg, menuSelFg, menuBorder;
    QString statusBg, statusFg;
    QString btnBg, btnFg, btnHoverBg, btnDisabledBg, btnDisabledFg;
    QString inputBg, inputFg, inputBorder;
    QString splitterHandleMainBg;
    QString splitterHandleTopBg, splitterHandleTopLeftBorder, splitterHandleTopRightBorder;
    QString splitterHandleVerBg, splitterHandleVerTopBorder, splitterHandleVerBottomBorder;
    QString processPanelBg, processPanelRightBorder;
    QString imageDisplayBg, imageDisplayLeftBorder;
    QString logDockBorderTop;
    QString labelFg, scrollAreaBg, frameBg;
    QString tabPaneBg, tabBarBg;
    QString tabBarTabBg, tabBarTabFg, tabBarTabSelBg, tabBarTabHoverBg;
    QString inspectorPanelBg, inspectorHeaderBg, inspectorHeaderFg, inspectorHeaderBorder;
    QString inspectorEmptyFg;
    QString inspectorTabSelBg, inspectorTabHoverBg;

    if (isDark) {
        mainWindowBg = "#1e1e1e"; splitterBg = "#1e1e1e";
        dockBg = "#252525"; dockTitleBg = "#2d2d2d"; dockTitleFg = "#ffffff";
        dockTitleBorder = "#444444"; dockTitleHover = "#333333";
        treeBg = "#252525"; treeFg = "#ffffff"; treeHover = "#3a3a3a";
        tableBg = "#252525"; tableFg = "#ffffff"; tableItemBorder = "#333";
        headerBg = "#333333"; headerFg = "#ffffff";
        scrollBg = "#252525"; scrollHandle = "#555555"; scrollHandleHover = "#666666";
        toolBarBg = "#252525"; toolBarBorder = "#444444";
        toolBtnFg = "#ffffff"; toolBtnHoverBg = "#3a3a3a"; toolBtnHoverBorder = "#555555";
        toolBtnCheckedBg = "#0e7490"; toolBtnCheckedBorder = "#06b6d4";
        menuBarBg = "#252525"; menuBarFg = "#ffffff";
        menuBg = "#252525"; menuFg = "#ffffff"; menuSelBg = "#0078d7"; menuSelFg = "#ffffff"; menuBorder = "#333";
        statusBg = "#252525"; statusFg = "#ffffff";
        btnBg = "#0078d7"; btnFg = "white"; btnHoverBg = "#1e8ad6";
        btnDisabledBg = "#555555"; btnDisabledFg = "#999999";
        inputBg = "#333333"; inputFg = "white"; inputBorder = "#555";
        splitterHandleMainBg = "#1e1e1e";
        splitterHandleTopBg = "#30363d"; splitterHandleTopLeftBorder = "#3f4750"; splitterHandleTopRightBorder = "#252b31";
        splitterHandleVerBg = "#30363d"; splitterHandleVerTopBorder = "#3f4750"; splitterHandleVerBottomBorder = "#252b31";
        processPanelBg = "#252525"; processPanelRightBorder = "#3b4148";
        imageDisplayBg = "#1e1e1e"; imageDisplayLeftBorder = "#3b4148";
        logDockBorderTop = "#3b4148";
        labelFg = "#ffffff"; scrollAreaBg = "#252525"; frameBg = "transparent";
        tabPaneBg = "#252525"; tabBarBg = "#252525";
        tabBarTabBg = "#333333"; tabBarTabFg = "#ffffff"; tabBarTabSelBg = "#444444"; tabBarTabHoverBg = "#3a3a3a";
        inspectorPanelBg = "#252525"; inspectorHeaderBg = "#2d2d2d"; inspectorHeaderFg = "#ffffff";
        inspectorHeaderBorder = "#3b4148"; inspectorEmptyFg = "#888888";
        inspectorTabSelBg = "#444444"; inspectorTabHoverBg = "#3a3a3a";
    } else {
        mainWindowBg = "#f5f5f5"; splitterBg = "#f5f5f5";
        dockBg = "#ffffff"; dockTitleBg = "#e8e8e8"; dockTitleFg = "#212121";
        dockTitleBorder = "#cccccc"; dockTitleHover = "#d0d0d0";
        treeBg = "#ffffff"; treeFg = "#212121"; treeHover = "#e5f3ff";
        tableBg = "#ffffff"; tableFg = "#212121"; tableItemBorder = "#eeeeee";
        headerBg = "#f0f0f0"; headerFg = "#212121";
        scrollBg = "#f0f0f0"; scrollHandle = "#c0c0c0"; scrollHandleHover = "#a0a0a0";
        toolBarBg = "#f8f8f8"; toolBarBorder = "#dddddd";
        toolBtnFg = "#212121"; toolBtnHoverBg = "#e5f3ff"; toolBtnHoverBorder = "#0078d7";
        toolBtnCheckedBg = "#CCEBF2"; toolBtnCheckedBorder = "#0891B2";
        menuBarBg = "#f8f8f8"; menuBarFg = "#212121";
        menuBg = "#ffffff"; menuFg = "#212121"; menuSelBg = "#0078d7"; menuSelFg = "#ffffff"; menuBorder = "#cccccc";
        statusBg = "#f8f8f8"; statusFg = "#212121";
        btnBg = "#0078d7"; btnFg = "white"; btnHoverBg = "#1e8ad6";
        btnDisabledBg = "#cccccc"; btnDisabledFg = "#999999";
        inputBg = "#ffffff"; inputFg = "#212121"; inputBorder = "#cccccc";
        splitterHandleMainBg = "#f5f5f5";
        splitterHandleTopBg = "#e4e8ed"; splitterHandleTopLeftBorder = "#d2d8e0"; splitterHandleTopRightBorder = "#f7f8fa";
        splitterHandleVerBg = "#e4e8ed"; splitterHandleVerTopBorder = "#d2d8e0"; splitterHandleVerBottomBorder = "#f7f8fa";
        processPanelBg = "#ffffff"; processPanelRightBorder = "#dce2e8";
        imageDisplayBg = "#ffffff"; imageDisplayLeftBorder = "#dce2e8";
        logDockBorderTop = "#dce2e8";
        labelFg = "#212121"; scrollAreaBg = "#ffffff"; frameBg = "transparent";
        tabPaneBg = "#ffffff"; tabBarBg = "#ffffff";
        tabBarTabBg = "#e8e8e8"; tabBarTabFg = "#212121"; tabBarTabSelBg = "#f5f5f5"; tabBarTabHoverBg = "#d0d0d0";
        inspectorPanelBg = "#ffffff"; inspectorHeaderBg = "#e8e8e8"; inspectorHeaderFg = "#212121";
        inspectorHeaderBorder = "#dce2e8"; inspectorEmptyFg = "#666666";
        inspectorTabSelBg = "#f5f5f5"; inspectorTabHoverBg = "#d0d0d0";
    }

    QString s;
    s.reserve(8000);

    s += QString("QMainWindow { background-color: %1; color: %2; }").arg(mainWindowBg, labelFg);
    s += QString("QWidget#MainContentWidget { background-color: %1; }").arg(mainWindowBg);
    s += QString("QSplitter#MainSplitter { background-color: %1; }").arg(splitterBg);
    s += QString("QSplitter::handle { width: %1; }").arg(splitterW);

    s += QString("QDockWidget { background-color: %1; color: %2; border: none; }").arg(dockBg, labelFg);
    s += QString("QDockWidget::title { background-color: %1; color: %2; font-weight: bold; "
                 "font-size: %3; padding: 8px 10px; border-bottom: %4 solid %5; }")
             .arg(dockTitleBg, dockTitleFg, baseFont, borderW, dockTitleBorder);
    s += QString("QDockWidget::title:hover { background-color: %1; }").arg(dockTitleHover);

    s += QString("QTreeWidget { background-color: %1; color: %2; border: none; font-size: %3; }")
             .arg(treeBg, treeFg, baseFont);
    s += "QTreeWidget::item { height: 28px; padding-left: 2px; padding-right: 2px; }";
    s += QString("QTreeWidget::item:hover { background-color: %1; }").arg(treeHover);
    s += "QTreeWidget::item:selected { background-color: #0078d7; }";

    s += QString("QTableWidget { background-color: %1; color: %2; border: none; font-size: %3; }")
             .arg(tableBg, tableFg, baseFont);
    s += QString("QTableWidget::item { border-bottom: %1 solid %2; }").arg(borderW, tableItemBorder);
    s += "QTableWidget::item:selected { background-color: #0078d7; }";
    s += QString("QHeaderView::section { background-color: %1; color: %2; padding: 5px; border: none; "
                 "font-size: %3; }").arg(headerBg, headerFg, baseFont);
    s += QString("QTableCornerButton::section { background-color: %1; border: none; }").arg(headerBg);

    s += QString("QScrollBar:vertical { background-color: %1; width: 12px; border: none; }").arg(scrollBg);
    s += QString("QScrollBar::handle:vertical { background-color: %1; min-height: 20px; border-radius: 6px; }").arg(scrollHandle);
    s += QString("QScrollBar::handle:vertical:hover { background-color: %1; }").arg(scrollHandleHover);
    s += "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }";
    s += QString("QScrollBar:horizontal { background-color: %1; height: 12px; border: none; }").arg(scrollBg);
    s += QString("QScrollBar::handle:horizontal { background-color: %1; min-width: 20px; border-radius: 6px; }").arg(scrollHandle);
    s += QString("QScrollBar::handle:horizontal:hover { background-color: %1; }").arg(scrollHandleHover);
    s += "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; }";

    s += QString("QToolBar { background-color: %1; border: %2 solid %3; spacing: %4; padding: 2px; }")
             .arg(toolBarBg, borderW, toolBarBorder, baseSpacing);
    s += QString("QToolBar QToolButton { background-color: transparent; color: %1; padding: 5px; "
                 "border: %2 solid transparent; }").arg(toolBtnFg, borderW);
    s += QString("QToolBar QToolButton:hover { background-color: %1; border: %2 solid %3; }")
             .arg(toolBtnHoverBg, borderW, toolBtnHoverBorder);
    s += QString("QToolBar QToolButton:pressed { background-color: %1; border: %2 solid %3; }")
             .arg(toolBtnCheckedBg, borderW, toolBtnCheckedBorder);
    s += QString("QToolBar QToolButton:checked { background-color: %1; color: #ffffff; border: %2 solid %3; }")
             .arg(toolBtnCheckedBg, borderW, toolBtnCheckedBorder);

    s += QString("QMenuBar { background-color: %1; color: %2; }").arg(menuBarBg, menuBarFg);
    s += QString("QMenuBar::item:selected { background-color: %1; }").arg(treeHover);
    s += QString("QMenu { background-color: %1; color: %2; border: %3 solid %4; }")
             .arg(menuBg, menuFg, borderW, menuBorder);
    s += QString("QMenu::item:selected { background-color: %1; color: %2; }").arg(menuSelBg, menuSelFg);

    s += QString("QStatusBar { background-color: %1; color: %2; font-size: %3; }")
             .arg(statusBg, statusFg, baseFont);
    s += QString("QStatusBar QLabel { padding: 0 %1; }").arg(baseSpacing);

    s += QString("QPushButton { background-color: %1; color: %2; padding: 5px 15px; border: none; }")
             .arg(btnBg, btnFg);
    s += QString("QPushButton:hover { background-color: %1; }").arg(btnHoverBg);
    s += QString("QPushButton:disabled { background-color: %1; color: %2; }").arg(btnDisabledBg, btnDisabledFg);

    s += QString("QLineEdit { background-color: %1; color: %2; border: %3 solid %4; padding: 5px; }")
             .arg(inputBg, inputFg, borderW, inputBorder);
    s += QString("QComboBox { background-color: %1; color: %2; border: %3 solid %4; padding: 5px; }")
             .arg(inputBg, inputFg, borderW, inputBorder);
    s += QString("QSpinBox { background-color: %1; color: %2; border: %3 solid %4; padding: 5px; }")
             .arg(inputBg, inputFg, borderW, inputBorder);

    s += QString("QSplitter#MainSplitter::handle { background-color: %1; border: none; }").arg(splitterHandleMainBg);
    s += QString("QSplitter#RightTopSplitter::handle:horizontal { background-color: %1; "
                 "border-left: %2 solid %3; border-right: %2 solid %4; }")
             .arg(splitterHandleTopBg, borderW, splitterHandleTopLeftBorder, splitterHandleTopRightBorder);
    s += QString("QSplitter#RightSplitter::handle:vertical { background-color: %1; "
                 "border-top: %2 solid %3; border-bottom: %2 solid %4; }")
             .arg(splitterHandleVerBg, borderW, splitterHandleVerTopBorder, splitterHandleVerBottomBorder);

    s += QString("QWidget#ProcessPanelWidget { background-color: %1; border-right: %2 solid %3; }")
             .arg(processPanelBg, borderW, processPanelRightBorder);
    s += QString("QWidget#ImageDisplayWidget { background-color: %1; border-left: %2 solid %3; "
                 "border-bottom: %2 solid %3; }")
             .arg(imageDisplayBg, borderW, imageDisplayLeftBorder);
    s += QString("QDockWidget#LogDock { border-top: %1 solid %2; }").arg(borderW, logDockBorderTop);

    s += QString("QLabel { color: %1; }").arg(labelFg);
    s += QString("QScrollArea { background-color: %1; }").arg(scrollAreaBg);
    s += QString("QFrame { background-color: %1; }").arg(frameBg);

    s += QString("QTabWidget::pane { border: none; background-color: %1; }").arg(tabPaneBg);
    s += QString("QTabBar { background-color: %1; }").arg(tabBarBg);
    s += QString("QTabBar::tab { background-color: %1; color: %2; font-size: %3; font-weight: 500; "
                 "min-height: 26px; padding: 3px 10px; border: none; }")
             .arg(tabBarTabBg, tabBarTabFg, baseFont);
    s += QString("QTabBar::tab:selected { background-color: %1; }").arg(tabBarTabSelBg);
    s += QString("QTabBar::tab:hover:!selected { background-color: %1; }").arg(tabBarTabHoverBg);

    // ===== ModuleInspector 专用样式 =====
    s += QString("QWidget#ModuleInspectorPanel { background-color: %1; }").arg(inspectorPanelBg);
    s += QString("QFrame#InspectorHeader { background-color: %1; border-bottom: %2 solid %3; "
                 "min-height: %4; max-height: %4; }")
             .arg(inspectorHeaderBg, borderW, inspectorHeaderBorder, px(m.collapsedHeaderHeight));
    s += QString("QLabel#InspectorModuleName { color: %1; font-size: %2; font-weight: 600; }")
             .arg(inspectorHeaderFg, headerFont);
    s += QString("QLabel#InspectorStatus { font-size: %1; }").arg(baseFont);
    s += QString("QLabel#InspectorElapsed { font-size: %1; color: %2; }").arg(baseFont, inspectorHeaderFg);
    s += QString("QToolButton#InspectorPinBtn, QToolButton#InspectorCollapseBtn, "
                 "QToolButton#InspectorCloseBtn { "
                 "background-color: transparent; border: none; padding: 2px; "
                 "min-width: %1; min-height: %1; max-width: %1; max-height: %1; }")
             .arg(px(m.compactIconButtonSize));
    s += QString("QToolButton#InspectorPinBtn:hover, QToolButton#InspectorCollapseBtn:hover, "
                 "QToolButton#InspectorCloseBtn:hover { background-color: rgba(128,128,128,40); }");
    s += QString("QTabWidget#InspectorTabs::pane { border: none; background-color: %1; }").arg(inspectorPanelBg);
    s += QString("QTabBar::tab { background-color: %1; color: %2; font-size: %3; font-weight: 500; "
                 "min-height: 26px; padding: 3px 10px; border: none; }")
             .arg(tabBarTabBg, tabBarTabFg, baseFont);
    s += QString("QTabBar::tab:selected { background-color: %1; }").arg(inspectorTabSelBg);
    s += QString("QTabBar::tab:hover:!selected { background-color: %1; }").arg(inspectorTabHoverBg);
    s += QString("QWidget#InspectorEmptyState { background-color: %1; }").arg(inspectorPanelBg);
    s += QString("QLabel#InspectorEmptyLabel { color: %1; font-size: %2; }").arg(inspectorEmptyFg, baseFont);
    s += QString("QPushButton#InspectorRerunBtn, QPushButton#InspectorResetBtn, "
                 "QPushButton#InspectorAdvancedBtn { min-height: %1; padding: 4px 12px; }")
             .arg(px(m.controlHeight));

    return s;
}

} // namespace

QString ThemeManager::styleSheet(bool isDark) {
    return buildBaseStyleSheet(isDark, layoutMetrics());
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
