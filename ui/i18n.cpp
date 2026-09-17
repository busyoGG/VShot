#include "i18n.hpp"

#include <QByteArray>
#include <QHash>
#include <QLocale>

namespace vshot {
namespace {

enum class UiLanguage {
    English,
    Chinese,
};

UiLanguage activeLanguage()
{
    static const UiLanguage language = [] {
        const QByteArray override = qgetenv("VSHOT_LANG").trimmed().toLower();
        if (override.startsWith("zh")) {
            return UiLanguage::Chinese;
        }
        if (!override.isEmpty()) {
            return UiLanguage::English;
        }
        return QLocale::system().language() == QLocale::Chinese ? UiLanguage::Chinese
                                                                : UiLanguage::English;
    }();
    return language;
}

// English source text is the lookup key, so call sites stay readable and a
// missing entry degrades to English instead of an empty label.
const QHash<QString, QString> &chineseTable()
{
    static const QHash<QString, QString> table = {
        // Tool buttons.
        {QStringLiteral("Select"), QString::fromUtf8("选择")},
        {QStringLiteral("Rect"), QString::fromUtf8("矩形")},
        {QStringLiteral("Ellipse"), QString::fromUtf8("椭圆")},
        {QStringLiteral("Arrow"), QString::fromUtf8("箭头")},
        {QStringLiteral("Draw"), QString::fromUtf8("涂鸦")},
        {QStringLiteral("Text"), QString::fromUtf8("文本")},
        {QStringLiteral("Mosaic"), QString::fromUtf8("马赛克")},

        // Action buttons and their tooltips.
        {QStringLiteral("Undo"), QString::fromUtf8("撤销")},
        {QStringLiteral("Redo"), QString::fromUtf8("重做")},
        {QStringLiteral("OK"), QString::fromUtf8("确定")},
        {QStringLiteral("Cancel"), QString::fromUtf8("取消")},
        {QStringLiteral("Undo last change (Ctrl+Z)"),
         QString::fromUtf8("撤销上一步 (Ctrl+Z)")},
        {QStringLiteral("Redo last change (Ctrl+Y)"),
         QString::fromUtf8("重做上一步 (Ctrl+Y)")},
        {QStringLiteral("Confirm capture (Enter)"),
         QString::fromUtf8("确认截图 (Enter)")},
        {QStringLiteral("Discard capture (Esc)"),
         QString::fromUtf8("取消截图 (Esc)")},

        // The pinned color card's right-click menu: the heading, and what the
        // badge says once a format has been put back on the clipboard.
        {QStringLiteral("Copy"), QString::fromUtf8("复制")},
        {QStringLiteral("Copied"), QString::fromUtf8("已复制")},
        {QStringLiteral("Copy failed"), QString::fromUtf8("复制失败")},

        // Tool tooltips.
        {QStringLiteral("Adjust selection; click an annotation to select, drag to "
                        "move, handles to resize, double-click text to re-edit"),
         QString::fromUtf8("调整选区；点击标注可选中，拖动移动，拖拽把手缩放，"
                           "双击文本重新编辑")},
        {QStringLiteral("Draw a rectangular annotation"),
         QString::fromUtf8("绘制矩形标注")},
        {QStringLiteral("Draw an elliptical annotation"),
         QString::fromUtf8("绘制椭圆标注")},
        {QStringLiteral("Draw an arrow with an adjustable head"),
         QString::fromUtf8("绘制箭头，头部大小可调")},
        {QStringLiteral("Draw a freehand line"), QString::fromUtf8("自由绘制线条")},
        {QStringLiteral("Click to place a text label, click text to re-edit"),
         QString::fromUtf8("点击放置文本标签，点击已有文本可重新编辑")},
        {QStringLiteral("Pixelate an area: rectangle, ellipse or freehand brush"),
         QString::fromUtf8("区域打码：矩形、椭圆或自由涂抹")},

        // Style segment labels.
        {QStringLiteral("Solid"), QString::fromUtf8("实线")},
        {QStringLiteral("Dash"), QString::fromUtf8("虚线")},
        {QStringLiteral("Dot"), QString::fromUtf8("点线")},
        {QStringLiteral("Open V"), QString::fromUtf8("开口V")},
        {QStringLiteral("Filled"), QString::fromUtf8("实心")},
        {QStringLiteral("Ellip"), QString::fromUtf8("椭圆")},
        {QStringLiteral("Brush"), QString::fromUtf8("涂抹")},

        // Style group tooltips.
        {QStringLiteral("Line style"), QString::fromUtf8("线型")},
        {QStringLiteral("Arrow head style"), QString::fromUtf8("箭头样式")},
        {QStringLiteral("Mosaic shape"), QString::fromUtf8("马赛克形状")},

        // Numeric group labels (static, measured and dynamic forms).
        {QStringLiteral("Width"), QString::fromUtf8("线宽")},
        {QStringLiteral("Width %1"), QString::fromUtf8("线宽 %1")},
        {QStringLiteral("Arrow %1"), QString::fromUtf8("箭头 %1")},
        {QStringLiteral("Mosaic %1"), QString::fromUtf8("马赛克 %1")},

        // Slider/spin tooltips and picker entries.
        {QStringLiteral("Stroke width (1-64 logical pixels)"),
         QString::fromUtf8("线宽（1-64 逻辑像素）")},
        {QStringLiteral("Arrow head size (1-8)"),
         QString::fromUtf8("箭头大小（1-8）")},
        {QStringLiteral("Text size (1-64)"), QString::fromUtf8("字号（1-64）")},
        {QStringLiteral("Mosaic strength (1-3)"),
         QString::fromUtf8("马赛克强度（1-3）")},
        {QStringLiteral("Custom color"), QString::fromUtf8("自定义颜色")},
        {QStringLiteral("Text font family"), QString::fromUtf8("文本字体")},
        {QStringLiteral("Hex color (#rrggbb)"),
         QString::fromUtf8("十六进制颜色（#rrggbb）")},

        // Accessible name templates.
        {QStringLiteral("Annotation color %1"), QString::fromUtf8("标注颜色 %1")},
        {QStringLiteral("Tool: %1"), QString::fromUtf8("工具：%1")},

        // Scrolling-capture hint overlay.
        {QStringLiteral("frames"), QString::fromUtf8("帧")},
        {QStringLiteral("Enter finish · Esc cancel"),
         QString::fromUtf8("Enter 完成 · Esc 取消")},
        // Why a scrolling capture stopped, as the CLI reports it.
        {QStringLiteral("the page ended"), QString::fromUtf8("已到页面末尾")},
        {QStringLiteral("height limit reached"), QString::fromUtf8("已达高度上限")},
        {QStringLiteral("frame limit reached"), QString::fromUtf8("已达帧数上限")},
        {QStringLiteral("time limit reached"), QString::fromUtf8("已超时")},
        {QStringLiteral("stopped by the user"), QString::fromUtf8("已手动结束")},
        {QStringLiteral("the hint overlay went away"),
         QString::fromUtf8("提示条已关闭")},
    };
    return table;
}

} // namespace

void initUiLanguage()
{
    // Force the one-time resolution (and the env snapshot) up front so every
    // later uiTr() call is a plain lookup.
    (void)activeLanguage();
}

QString uiTr(const QString &english)
{
    if (activeLanguage() == UiLanguage::English) {
        return english;
    }
    return chineseTable().value(english, english);
}

} // namespace vshot
