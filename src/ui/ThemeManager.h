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

class ThemeManager {
public:
    static QString styleSheet(bool isDark);
    static ThemePalette palette(bool isDark);
};

} // namespace DeepLux
