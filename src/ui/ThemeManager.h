#pragma once

#include <QString>

namespace DeepLux {

struct ThemePalette {
    QString bgColor;
    QString borderColor;
    QString textColor;
    QString btnColor;
    QString treeBgColor;
    QString treeTextColor;
    QString treeHoverColor;
    QString scrollBgColor;
    QString panelBorderColor;
    QString btnBgColor;
    QString btnTextColor;
    QString btnHoverColor;
    QString toolBtnBorderColor;
    QString logTableBg;
    QString logHeaderBg;
    QString logTextColor;
    QString logLineColor;
    QString processTabBorder;
    QString processTabBg;
    QString processTabFg;
    QString logTabPaneBg;
    QString logTabBg;
    QString logTabFg;
    QString logTabSelFg;
    QString logTabHoverBg;
    QString imageDisplayBg;
};

/**
 * @brief 统一布局尺寸
 *
 * 集中管理整个 UI 中使用的尺寸常量，避免各组件硬编码导致不一致。
 * 所有数值单位为像素(px)，可供 QSplitter / 控件尺寸 / styleSheet 引用。
 */
struct LayoutMetrics {
    // 字号
    int baseFontSize = 13;   // 基础字号
    int headerFontSize = 14; // 栏目标题字号

    // 控件高度
    int controlHeight = 32;         // 常规控件高度
    int compactIconButtonSize = 28; // 紧凑图标按钮

    // 工具栏
    int toolbarHeight = 46; // 工具栏高度
    int iconSize = 20;      // 图标尺寸

    // 间距 / 边距
    int baseSpacing = 6;    // 基础间距
    int contentMargin = 12; // 内容边距

    // 面板装饰
    int panelRadius = 4;         // 面板圆角
    int borderWidth = 1;         // 边框宽度
    int splitterHandleWidth = 6; // Splitter 拖动区宽度

    // 面板宽度
    int toolPanelWidth = 250; // 工具面板宽
    int flowPanelWidth = 340; // 流程面板宽

    // 检查器
    int inspectorWidth = 320;       // 检查器宽
    int inspectorMinWidth = 280;    // 检查器最小宽
    int inspectorMaxWidth = 380;    // 检查器最大宽
    int collapsedHeaderHeight = 34; // 折叠栏高度

    // 底部面板
    int bottomPanelHeight = 220;    // 底部面板默认高
    int bottomPanelMinHeight = 140; // 底部面板最小高
    int bottomPanelMaxHeight = 360; // 底部面板最大高

    /**
     * @brief 返回当前主题下用于 QSplitter handle 的宽度。
     */
    int splitterHandleWidthForTheme() const {
        return splitterHandleWidth;
    }
};

class ThemeManager {
public:
    static QString styleSheet(bool isDark);
    static ThemePalette palette(bool isDark);
    static LayoutMetrics layoutMetrics();
};

} // namespace DeepLux
