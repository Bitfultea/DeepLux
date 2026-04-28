#ifndef DEEPLUX_ANSI_CONSTANTS_H
#define DEEPLUX_ANSI_CONSTANTS_H

#include <QColor>
#include <QMap>

namespace DeepLux {

// 颜色类型 (namespace 级别，供 CellAttributes 使用)
enum class ColorType {
    Basic,      // 0-7 (或 0-15 亮色)
    Palette256, // 0-255 (256 色模式)
    TrueColor   // 24-bit RGB
};

/**
 * @brief 终端单元格属性
 */
struct CellAttributes {
    // 样式
    bool bold = false;
    bool dim = false;
    bool italic = false;
    bool underline = false;
    bool strikethrough = false;
    bool reverse = false;  // 交换前景/背景

    // 前景色
    ColorType fgType = ColorType::Basic;
    int fgValue = 7;  // 默认白色

    // 背景色
    ColorType bgType = ColorType::Basic;
    int bgValue = 0;  // 默认黑色

    void reset() {
        bold = dim = italic = underline = strikethrough = reverse = false;
        fgType = ColorType::Basic;
        fgValue = 7;
        bgType = ColorType::Basic;
        bgValue = 0;
    }

    bool operator==(const CellAttributes& other) const {
        return bold == other.bold && dim == other.dim && italic == other.italic
            && underline == other.underline && strikethrough == other.strikethrough
            && reverse == other.reverse && fgType == other.fgType && fgValue == other.fgValue
            && bgType == other.bgType && bgValue == other.bgValue;
    }

    bool operator!=(const CellAttributes& other) const {
        return !(*this == other);
    }
};

/**
 * @brief ANSI SGR (Select Graphic Rendition) 参数定义
 */
class AnsiConstants {
public:
    // SGR 参数值
    enum SGRParam {
        // 样式
        SGR_RESET                = 0,
        SGR_BOLD                 = 1,
        SGR_DIM                  = 2,
        SGR_ITALIC               = 3,
        SGR_UNDERLINE            = 4,
        SGR_BLINK                = 5,
        SGR_REVERSE              = 7,
        SGR_STRIKETHROUGH        = 9,
        SGR_NORMAL_INTENSITY    = 22,
        SGR_NOT_ITALIC           = 23,
        SGR_NOT_UNDERLINE        = 24,
        SGR_NOT_BLINK            = 25,
        SGR_NOT_REVERSE          = 27,
        SGR_NOT_STRIKETHROUGH    = 29,

        // 前景色 (30-37)
        SGR_FG_BLACK             = 30,
        SGR_FG_RED               = 31,
        SGR_FG_GREEN             = 32,
        SGR_FG_YELLOW            = 33,
        SGR_FG_BLUE              = 34,
        SGR_FG_MAGENTA           = 35,
        SGR_FG_CYAN              = 36,
        SGR_FG_WHITE             = 37,

        // 前景色亮色 (90-97)
        SGR_FG_BRIGHT_BLACK      = 90,
        SGR_FG_BRIGHT_RED        = 91,
        SGR_FG_BRIGHT_GREEN      = 92,
        SGR_FG_BRIGHT_YELLOW     = 93,
        SGR_FG_BRIGHT_BLUE       = 94,
        SGR_FG_BRIGHT_MAGENTA    = 95,
        SGR_FG_BRIGHT_CYAN       = 96,
        SGR_FG_BRIGHT_WHITE      = 97,

        // 背景色 (40-47)
        SGR_BG_BLACK             = 40,
        SGR_BG_RED               = 41,
        SGR_BG_GREEN             = 42,
        SGR_BG_YELLOW            = 43,
        SGR_BG_BLUE              = 44,
        SGR_BG_MAGENTA           = 45,
        SGR_BG_CYAN              = 46,
        SGR_BG_WHITE             = 47,

        // 背景色亮色 (100-107)
        SGR_BG_BRIGHT_BLACK      = 100,
        SGR_BG_BRIGHT_RED        = 101,
        SGR_BG_BRIGHT_GREEN      = 102,
        SGR_BG_BRIGHT_YELLOW     = 103,
        SGR_BG_BRIGHT_BLUE       = 104,
        SGR_BG_BRIGHT_MAGENTA    = 105,
        SGR_BG_BRIGHT_CYAN       = 106,
        SGR_BG_BRIGHT_WHITE      = 107,

        // 256 色模式
        SGR_FG_256               = 38,
        SGR_BG_256               = 48,

        // True Color 模式
        SGR_FG_TRUE_COLOR        = 38,
        SGR_BG_TRUE_COLOR         = 48
    };

    // 默认 16 色调色板 (标准 ANSI 颜色)
    static const QMap<int, QColor>& basicPalette() {
        static QMap<int, QColor> palette = {
            // 标准前景色 (0-7)
            {0, QColor("#000000")},   // 黑
            {1, QColor("#c23631")},   // 红
            {2, QColor("#25c50a")},   // 绿
            {3, QColor("#c4a40a")},   // 黄/棕
            {4, QColor("#3069c2")},   // 蓝
            {5, QColor("#9c2790")},   // 洋红
            {6, QColor("#0a929c")},   // 青
            {7, QColor("#c4c4c4")},   // 白

            // 亮色前景色 (8-15)
            {8,  QColor("#5a5a5a")},   // 亮黑/灰
            {9,  QColor("#e7504a")},   // 亮红
            {10, QColor("#26d456")},   // 亮绿
            {11, QColor("#e7d62a")},   // 亮黄
            {12, QColor("#4a8fe7")},   // 亮蓝
            {13, QColor("#c43cce")},   // 亮洋红
            {14, QColor("#2ec8c8")},   // 亮青
            {15, QColor("#ffffff")}    // 亮白
        };
        return palette;
    }

    // 256 色调色板 (xterm)
    static const QList<QColor>& palette256() {
        static QList<QColor> palette = []() {
            QList<QColor> colors;

            // 标准 16 色
            for (int i = 0; i < 16; ++i) {
                colors.append(basicPalette().value(i));
            }

            // 216 色 (6x6x6 立方)
            for (int r = 0; r < 6; ++r) {
                for (int g = 0; g < 6; ++g) {
                    for (int b = 0; b < 6; ++b) {
                        int ri = r ? r * 40 + 55 : 0;
                        int gi = g ? g * 40 + 55 : 0;
                        int bi = b ? b * 40 + 55 : 0;
                        colors.append(QColor(ri, gi, bi));
                    }
                }
            }

            // 24 级灰度
            for (int i = 0; i < 24; ++i) {
                int gray = i * 10 + 8;
                colors.append(QColor(gray, gray, gray));
            }

            return colors;
        }();
        return palette;
    }
};

} // namespace DeepLux

#endif // DEEPLUX_ANSI_CONSTANTS_H
