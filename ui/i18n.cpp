// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

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
        {QStringLiteral("Line"), QString::fromUtf8("直线")},
        {QStringLiteral("Wave"), QString::fromUtf8("波浪线")},
        {QStringLiteral("Bezier"), QString::fromUtf8("钢笔")},
        {QStringLiteral("Draw"), QString::fromUtf8("涂鸦")},
        {QStringLiteral("Text"), QString::fromUtf8("文本")},
        {QStringLiteral("Mosaic"), QString::fromUtf8("马赛克")},
        {QStringLiteral("Pick"), QString::fromUtf8("取色")},
        {QStringLiteral("Erase"), QString::fromUtf8("橡皮")},

        // Action buttons and their tooltips.
        {QStringLiteral("Undo"), QString::fromUtf8("撤销")},
        {QStringLiteral("Redo"), QString::fromUtf8("重做")},
        // The annotation overlay's own controls: its clear button, the way out
        // of the background process, and the toolbar's grip.
        {QStringLiteral("Clear"), QString::fromUtf8("清除")},
        {QStringLiteral("Quit annotation"), QString::fromUtf8("退出标注")},
        {QStringLiteral("Drag to move"), QString::fromUtf8("拖动移动")},
        {QStringLiteral("OK"), QString::fromUtf8("确定")},
        {QStringLiteral("Cancel"), QString::fromUtf8("取消")},
        // The Pin button: it ends the session the way OK does, but the result
        // goes to the screen rather than to disk.
        {QStringLiteral("Pin"), QString::fromUtf8("钉住")},
        {QStringLiteral("Pin the result on the screen"),
         QString::fromUtf8("把成品钉在屏幕上")},
        {QStringLiteral("Undo last change (Ctrl+Z)"),
         QString::fromUtf8("撤销上一步 (Ctrl+Z)")},
        {QStringLiteral("Redo last change (Ctrl+Y)"),
         QString::fromUtf8("重做上一步 (Ctrl+Y)")},
        {QStringLiteral("Confirm capture (Enter)"),
         QString::fromUtf8("确认截图 (Enter)")},
        {QStringLiteral("Discard capture (Esc)"),
         QString::fromUtf8("取消截图 (Esc)")},

        // The pinned color card's right-click menu: the heading, the action row
        // every pin has, and what the badge says once something has happened to
        // the pin.
        {QStringLiteral("Copy"), QString::fromUtf8("复制")},
        // The action rows every pin's menu ends with. `Copy image` copies the
        // pin's pixels rather than a format string, so it gets a wording of its
        // own; `Copied image` is the badge it leaves behind.
        {QStringLiteral("Copy image"), QString::fromUtf8("复制图像")},
        {QStringLiteral("Copied image"), QString::fromUtf8("已复制图像")},
        {QStringLiteral("Copied"), QString::fromUtf8("已复制")},
        {QStringLiteral("Copy failed"), QString::fromUtf8("复制失败")},
        {QStringLiteral("Save as…"), QString::fromUtf8("另存为…")},
        {QStringLiteral("Recognize text…"), QString::fromUtf8("取字…")},
        {QStringLiteral("Edit"), QString::fromUtf8("编辑")},
        {QStringLiteral("Reset zoom"), QString::fromUtf8("重置缩放")},
        {QStringLiteral("Close"), QString::fromUtf8("关闭")},
        {QStringLiteral("Save pinned image"), QString::fromUtf8("保存浮层图片")},
        {QStringLiteral("PNG image (*.png)"), QString::fromUtf8("PNG 图片 (*.png)")},
        {QStringLiteral("Saved %1"), QString::fromUtf8("已保存 %1")},
        {QStringLiteral("Could not save the image"), QString::fromUtf8("图片保存失败")},

        // The editor's keyboard actions, as the settings page lists them: the
        // name of each and the one line that says what it does. A key the user
        // can rebind has to be findable by what it does, not by the id the file
        // stores it under.
        {QStringLiteral("Keyboard"), QString::fromUtf8("键盘")},
        // The sub-headings a settings card is split into. Short on purpose: a
        // group is a signpost between rows, not a setting of its own, and a
        // heading long enough to need wrapping reads as one more row.
        {QStringLiteral("Tool"), QString::fromUtf8("工具")},
        {QStringLiteral("PNG"), QString::fromUtf8("PNG")},
        {QStringLiteral("HDR"), QString::fromUtf8("HDR")},
        {QStringLiteral("Which output"), QString::fromUtf8("用哪个输出")},
        {QStringLiteral("Scrolling"), QString::fromUtf8("滚动")},
        {QStringLiteral("Stitching"), QString::fromUtf8("拼接")},
        {QStringLiteral("What is recorded"), QString::fromUtf8("录什么")},
        {QStringLiteral("Notification"), QString::fromUtf8("通知")},
        {QStringLiteral("Ring"), QString::fromUtf8("环形缓冲")},
        {QStringLiteral("Confirm the capture"), QString::fromUtf8("确认截图")},
        {QStringLiteral("Accepts the capture and writes it out"),
         QString::fromUtf8("接受这次截图并把它写出来")},
        {QStringLiteral("Discard the capture"), QString::fromUtf8("取消截图")},
        {QStringLiteral("Throws the capture away"), QString::fromUtf8("丢掉这次截图")},
        {QStringLiteral("Undo"), QString::fromUtf8("撤销")},
        {QStringLiteral("Takes back the last change to the marks"),
         QString::fromUtf8("收回对标注的最后一次改动")},
        {QStringLiteral("Redo"), QString::fromUtf8("重做")},
        {QStringLiteral("Puts the last undone change back"),
         QString::fromUtf8("把刚收回的那次改动放回去")},
        {QStringLiteral("Copy the result"), QString::fromUtf8("复制成品")},
        {QStringLiteral("Puts the capture and its marks on the clipboard"),
         QString::fromUtf8("把截图和它的标注放进剪贴板")},
        {QStringLiteral("Copy the selected text"), QString::fromUtf8("复制选中的文字")},
        {QStringLiteral("Copies the text the text-selection mode or a translation picked"),
         QString::fromUtf8("复制取字模式或翻译选中的那部分文字")},
        {QStringLiteral("Paste an image"), QString::fromUtf8("粘贴图片")},
        {QStringLiteral("Pastes an image from the clipboard into the selection"),
         QString::fromUtf8("把剪贴板里的图片贴进选区")},
        {QStringLiteral("Select every mark"), QString::fromUtf8("选中所有标注")},
        {QStringLiteral("Picks up every mark at once"), QString::fromUtf8("一次选中所有标注")},
        {QStringLiteral("Select no mark"), QString::fromUtf8("不选标注")},
        {QStringLiteral("Puts every mark down"), QString::fromUtf8("放开所有标注")},
        {QStringLiteral("Next mark"), QString::fromUtf8("下一个标注")},
        {QStringLiteral("Moves the focus to the next mark"),
         QString::fromUtf8("把焦点移到下一个标注")},
        {QStringLiteral("Previous mark"), QString::fromUtf8("上一个标注")},
        {QStringLiteral("Moves the focus to the previous mark"),
         QString::fromUtf8("把焦点移到上一个标注")},
        {QStringLiteral("Delete the mark"), QString::fromUtf8("删除标注")},
        {QStringLiteral("Deletes the mark the focus is on"),
         QString::fromUtf8("删掉焦点所在的那个标注")},
        {QStringLiteral("Copy the colour under the cursor"),
         QString::fromUtf8("复制光标下的颜色")},
        {QStringLiteral("While the magnifier is up: copies that pixel's colour code"),
         QString::fromUtf8("放大镜显示时：复制那个像素的颜色代码")},
        {QStringLiteral("Use the colour under the cursor"),
         QString::fromUtf8("用光标下的颜色")},
        {QStringLiteral("While the magnifier is up: makes that colour the current tool's"),
         QString::fromUtf8("放大镜显示时：把那个颜色设为当前工具的")},
        {QStringLiteral("Show the magnifier"), QString::fromUtf8("显示放大镜")},
        {QStringLiteral("Shows the magnifier for two seconds, without dragging"),
         QString::fromUtf8("不拖拽也显示两秒放大镜")},
        {QStringLiteral("Keep the aspect ratio"), QString::fromUtf8("保持比例")},
        {QStringLiteral("Hold while resizing: the shape keeps its proportions"),
         QString::fromUtf8("缩放时按住：形状保持原有比例")},
        {QStringLiteral("Take a bigger step"), QString::fromUtf8("加大步长")},
        {QStringLiteral("Hold while walking the cursor: ten pixels at a time"),
         QString::fromUtf8("移动光标时按住：一次走十像素")},
        {QStringLiteral("Pick a mark up"), QString::fromUtf8("选中已有标注")},
        {QStringLiteral("Hold to move a mark under the pointer, rather than drawing "
                        "with the armed tool"),
         QString::fromUtf8("按住后，指针下的标注被拖动，而不是用当前工具作画")},
        {QStringLiteral("Cursor left"), QString::fromUtf8("光标左移")},
        {QStringLiteral("Moves the cursor one pixel left"), QString::fromUtf8("光标左移一像素")},
        {QStringLiteral("Cursor right"), QString::fromUtf8("光标右移")},
        {QStringLiteral("Moves the cursor one pixel right"), QString::fromUtf8("光标右移一像素")},
        {QStringLiteral("Cursor up"), QString::fromUtf8("光标上移")},
        {QStringLiteral("Moves the cursor one pixel up"), QString::fromUtf8("光标上移一像素")},
        {QStringLiteral("Cursor down"), QString::fromUtf8("光标下移")},
        {QStringLiteral("Moves the cursor one pixel down"), QString::fromUtf8("光标下移一像素")},
        {QStringLiteral("Press the keys for this action"),
         QString::fromUtf8("按下这个动作用的键")},
        {QStringLiteral("Click, then press the key"), QString::fromUtf8("点一下，再按键")},
        {QStringLiteral("Click to see and change this action's keys"),
         QString::fromUtf8("点开查看并修改这个动作的按键")},
        {QStringLiteral("Key already in use"), QString::fromUtf8("按键已被占用")},
        {QStringLiteral("%1 is already bound to \"%2\". Move it to \"%3\"?"),
         QString::fromUtf8("%1 已经绑给了「%2」。要把它移给「%3」吗？")},
        {QStringLiteral("Remove"), QString::fromUtf8("移除")},
        {QStringLiteral("Done"), QString::fromUtf8("完成")},
        // The two halves of the colour readout's hint line, which name the keys
        // that act on the pixel under the picker.
        {QStringLiteral("copy"), QString::fromUtf8("复制")},
        {QStringLiteral("use"), QString::fromUtf8("使用")},
        {QStringLiteral("None"), QString::fromUtf8("无")},
        {QStringLiteral("Which key does what, while a capture is on the screen. Every "
                        "action here is also a toolbar button, so a key that is in the "
                        "way can be cleared instead of moved."),
         QString::fromUtf8("截图在屏幕上时，每个键做什么。这里的每个动作在工具栏上也有按钮，"
                           "所以碍事的键可以直接清掉，不必挪到别处。")},

        // Pasting an image into the annotation editor, and reading the text
        // off a capture.
        {QStringLiteral("Image"), QString::fromUtf8("图片")},
        {QStringLiteral("Text+"), QString::fromUtf8("取字")},
        {QStringLiteral("OCR…"), QString::fromUtf8("识别中")},
        {QStringLiteral("Select the text in the selection and copy what you select"),
         QString::fromUtf8("选中选区里的文字，复制选中的部分")},
        {QStringLiteral("No text is selected."), QString::fromUtf8("没有选中文字。")},
        {QStringLiteral("Failed"), QString::fromUtf8("失败")},
        {QStringLiteral("Reading text needs a selection to read from."),
         QString::fromUtf8("要先有选区才能取字。")},
        {QStringLiteral("The selection is empty."), QString::fromUtf8("选区是空的。")},
        {QStringLiteral("The selection is on no output."),
         QString::fromUtf8("选区不在任何输出上。")},
        {QStringLiteral("The selection has no pixels on this output."),
         QString::fromUtf8("选区在这块输出上没有像素。")},
        {QStringLiteral("The captured frame is not available."),
         QString::fromUtf8("拿不到捕获的画面。")},
        {QStringLiteral("Cannot create a temporary directory for the text."),
         QString::fromUtf8("建不了放文字的临时目录。")},
        {QStringLiteral("Cannot write the selection to read its text."),
         QString::fromUtf8("写不出选区，读不了它的文字。")},
        {QStringLiteral("Cannot locate vshot to read the text."),
         QString::fromUtf8("找不到 vshot，读不了文字。")},
        {QStringLiteral("Cannot start vshot to read the text."),
         QString::fromUtf8("启动不了 vshot，读不了文字。")},
        {QStringLiteral("Reading the text took too long."),
         QString::fromUtf8("取字超时。")},
        {QStringLiteral("Reading the text failed."), QString::fromUtf8("取字失败。")},
        {QStringLiteral("No text was found in the selection."),
         QString::fromUtf8("选区里没找到文字。")},
        // Screenshot translation: the same selection the text tool reads is
        // translated and drawn over the text it replaces.
        {QStringLiteral("Translate"), QString::fromUtf8("翻译")},
        {QStringLiteral("Translate the text in the selection and draw it in place"),
         QString::fromUtf8("翻译选区里的文字并就地绘制")},
        {QStringLiteral("Translating…"), QString::fromUtf8("翻译中")},
        {QStringLiteral("Translation needs a selection to read from."),
         QString::fromUtf8("要先有选区才能翻译。")},
        {QStringLiteral("Cannot locate vshot to translate the text."),
         QString::fromUtf8("找不到 vshot，翻译不了文字。")},
        {QStringLiteral("Cannot start vshot to translate the text."),
         QString::fromUtf8("启动不了 vshot，翻译不了文字。")},
        {QStringLiteral("Translating the text took too long."),
         QString::fromUtf8("翻译超时。")},
        {QStringLiteral("Translating the text failed."), QString::fromUtf8("翻译失败。")},
        {QStringLiteral("The translation has no positions to draw."),
         QString::fromUtf8("翻译结果没有可绘制的位置。")},
        {QStringLiteral("There is no translation to save."),
         QString::fromUtf8("没有可保存的翻译。")},
        {QStringLiteral("The session names no file to write the translation to."),
         QString::fromUtf8("会话没有指定保存翻译的文件。")},
        {QStringLiteral("Cannot write the translated image."),
         QString::fromUtf8("写不出翻译后的图片。")},
        {QStringLiteral("Cannot copy the text to the clipboard."),
         QString::fromUtf8("文字复制不到剪贴板。")},
        {QStringLiteral("Paste an image onto the capture (Ctrl+V for the clipboard)"),
         QString::fromUtf8("把一张图片贴到截图上（Ctrl+V 贴剪贴板）")},
        {QStringLiteral("Open an image"), QString::fromUtf8("打开图片")},
        {QStringLiteral("Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif *.tif *.tiff)"),
         QString::fromUtf8("图片 (*.png *.jpg *.jpeg *.webp *.bmp *.gif *.tif *.tiff)")},
        {QStringLiteral("All files (*)"), QString::fromUtf8("所有文件 (*)")},
        {QStringLiteral("Paste needs a selection to paste onto."),
         QString::fromUtf8("要先有选区才能贴图。")},
        {QStringLiteral("`wl-paste` was not found, so the clipboard cannot be read."),
         QString::fromUtf8("找不到 `wl-paste`，读不了剪贴板。")},
        {QStringLiteral("The clipboard holds no image."),
         QString::fromUtf8("剪贴板里没有图片。")},
        {QStringLiteral("The clipboard is empty."), QString::fromUtf8("剪贴板是空的。")},
        {QStringLiteral("Cannot locate the vshot helper to open the file dialog."),
         QString::fromUtf8("找不到 vshot helper，打不开文件对话框。")},
        {QStringLiteral("Could not start the file dialog."),
         QString::fromUtf8("文件对话框没能启动。")},

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
        {QStringLiteral("Draw a curved path: click to add an anchor, drag to bend the "
                        "curve, click the first anchor to close and fill it, "
                        "double-click to finish it open"),
         QString::fromUtf8("绘制曲线路径：点击添加锚点，拖动弯曲曲线，"
                           "点回第一个锚点闭合并填充，双击结束为开放路径")},
        {QStringLiteral("Click to place a text label, click text to re-edit"),
         QString::fromUtf8("点击放置文本标签，点击已有文本可重新编辑")},
        {QStringLiteral("Pixelate an area: rectangle, ellipse or freehand brush"),
         QString::fromUtf8("区域打码：矩形、椭圆或自由涂抹")},
        {QStringLiteral("Pick a color from the image; the pick hands it to the tool "
                        "you were using and leaves you on it"),
         QString::fromUtf8("从画面上取色；取到的颜色交给刚才用的工具，取完回到它")},

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
        {QStringLiteral("Text size in pixels (7-448)"),
         QString::fromUtf8("字号（像素，7-448）")},
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

        // The settings window.  Its labels name the same settings the config
        // file does, so the wording stays close to the keys.
        {QStringLiteral("vshot settings"), QString::fromUtf8("VShot 设置")},
        {QStringLiteral("Settings"), QString::fromUtf8("设置")},
        {QStringLiteral("Save"), QString::fromUtf8("保存")},
        {QStringLiteral("Saved."), QString::fromUtf8("已保存。")},
        {QStringLiteral("Could not write the config file."),
         QString::fromUtf8("写不进配置文件。")},
        {QStringLiteral("Saved to %1"), QString::fromUtf8("保存到 %1")},
        {QStringLiteral("There is no config directory, so nothing can be saved."),
         QString::fromUtf8("没有配置目录，所以什么都存不下。")},
        {QStringLiteral("Annotation editor"), QString::fromUtf8("标注编辑器")},
        {QStringLiteral("The style the toolbar opens with next time. Leaving the editor "
                        "writes this back, whether or not the capture went through."),
         QString::fromUtf8("下次打开工具栏时的样式。退出编辑器时写回，"
                           "跟这次截图有没有成无关。")},
        // One clause per page heading, all ending the same way: the defaults
        // are only defaults, and the command line still wins over them.
        {QStringLiteral("Where a screenshot is written. Used only where the command line "
                        "gives nothing: an argument, or an environment variable, always "
                        "wins over these."),
         QString::fromUtf8("截图写到哪里。只用于命令行没给出的参数："
                           "命令行参数与环境变量永远优先于这里。")},
        {QStringLiteral("Defaults for the long scrolling capture. Used only where the "
                        "command line gives nothing: an argument, or an environment "
                        "variable, always wins over these."),
         QString::fromUtf8("长滚动截图的默认值。只用于命令行没给出的参数："
                           "命令行参数与环境变量永远优先于这里。")},
        {QStringLiteral("What happens once the text is out. Used only where the command "
                        "line gives nothing: an argument, or an environment variable, "
                        "always wins over these."),
         QString::fromUtf8("文字出来之后做什么。只用于命令行没给出的参数："
                           "命令行参数与环境变量永远优先于这里。")},
        {QStringLiteral("Defaults for `record` and `replay`. Used only where the command "
                        "line gives nothing: an argument, or an environment variable, "
                        "always wins over these."),
         QString::fromUtf8("`record` 与 `replay` 的默认值。只用于命令行没给出的参数："
                           "命令行参数与环境变量永远优先于这里。")},
        {QStringLiteral("Opening tool"), QString::fromUtf8("默认工具")},
        {QStringLiteral("The tool editing starts with; session changes are not saved here"),
         QString::fromUtf8("编辑状态的起始工具；会话中的切换不写回这里")},
        {QStringLiteral("Select tool drags"), QString::fromUtf8("选择工具拖动方式")},
        {QStringLiteral("precise presses the mark itself; loose drags a selected mark from "
                        "anywhere"),
         QString::fromUtf8("precise：按在标注上；loose：已选中的标注可从任意位置拖动")},
        {QStringLiteral("Color"), QString::fromUtf8("颜色")},
        {QStringLiteral("Hex, with alpha last when it is not opaque"),
         QString::fromUtf8("十六进制，不透明时省略 alpha，否则写在最后")},
        {QStringLiteral("Annotation color"), QString::fromUtf8("标注颜色")},
        {QStringLiteral("Stroke"), QString::fromUtf8("线条")},
        {QStringLiteral("1-64 logical pixels"),
         QString::fromUtf8("1-64 逻辑像素")},
        {QStringLiteral("Head size"), QString::fromUtf8("箭头大小")},
        {QStringLiteral("Head style"), QString::fromUtf8("箭头样式")},
        {QStringLiteral("1-8"), QString::fromUtf8("1-8")},
        {QStringLiteral("Size"), QString::fromUtf8("字号")},
        {QStringLiteral("1-64"), QString::fromUtf8("1-64")},
        {QStringLiteral("Font"), QString::fromUtf8("字体")},
        {QStringLiteral("System default"), QString::fromUtf8("系统默认")},
        {QStringLiteral("Shape"), QString::fromUtf8("形状")},
        {QStringLiteral("Strength"), QString::fromUtf8("强度")},
        {QStringLiteral("1-3"), QString::fromUtf8("1-3")},
        {QStringLiteral("Output"), QString::fromUtf8("输出")},
        {QStringLiteral("SDR format"), QString::fromUtf8("SDR 格式")},
        {QStringLiteral("The format the SDR half of a capture is written in, and the one "
                        "the clipboard carries. PNG is lossless, so it is the only format "
                        "this build has; the parameters of whichever format is chosen are "
                        "on the Format settings page"),
         QString::fromUtf8("截图的 SDR 那一半写成什么格式，剪贴板也用它。"
                           "PNG 无损，是本构建唯一的格式；所选格式的参数在「格式设置」页")},
        {QStringLiteral("HDR format"), QString::fromUtf8("HDR 格式")},
        {QStringLiteral("The second file of a capture that carries HDR content, written "
                        "beside the SDR one with the same name. AVIF is ten-bit BT.2020 PQ "
                        "and says so in the file, so every reader shows it right, but it is "
                        "lossy; Radiance RGBE is the light exactly as captured, and is read "
                        "by few. Its parameters are on the Format settings page"),
         QString::fromUtf8("截图带 HDR 内容时，与 SDR 那个同名并排写出的第二个文件。"
                           "AVIF 是 10 位 BT.2020 PQ 并在文件里声明，所有读取器都能正确"
                           "显示，但为有损；Radiance RGBE 是原样记录的光，但读取器很少。"
                           "它的参数在「格式设置」页")},
        {QStringLiteral("Format settings"), QString::fromUtf8("格式设置")},
        {QStringLiteral("How each file format writes. The formats listed are the ones "
                        "this build was compiled with, and the settings under each are "
                        "the ones that format itself declares."),
         QString::fromUtf8("每种文件格式怎么写。列出的格式是本构建编译进来的，"
                           "每种格式下面的设置由该格式自己声明。")},
        {QStringLiteral("Asking this build which formats it has…"),
         QString::fromUtf8("正在询问本构建有哪些格式…")},
        {QStringLiteral("This build reports no file formats, so there is nothing "
                        "to set here."),
         QString::fromUtf8("本构建没有报告任何文件格式，这里没有可设的项。")},
        {QStringLiteral("Nothing to tune"), QString::fromUtf8("没有可调的项")},
        {QStringLiteral("This format takes no settings: it writes what it is given, at "
                        "its own defaults"),
         QString::fromUtf8("这种格式不接受设置：给什么写什么，用它自己的默认值")},
        {QStringLiteral("No parameters"), QString::fromUtf8("无参数")},
        {QStringLiteral("SDR"), QString::fromUtf8("SDR")},
        {QStringLiteral("HDR"), QString::fromUtf8("HDR")},
        // The parameters themselves.  A codec declares its own label and hint in
        // Rust, so these are keyed by the English text it declares.
        {QStringLiteral("Compression"), QString::fromUtf8("压缩")},
        {QStringLiteral("All levels are lossless; slower ones buy a smaller file"),
         QString::fromUtf8("各档全部无损；越慢换来越小的文件")},
        {QStringLiteral("Quality"), QString::fromUtf8("质量")},
        {QStringLiteral("Higher keeps more of the picture and writes a bigger file; 90 is "
                        "about what other AVIF encoders call quality 90"),
         QString::fromUtf8("越高保留的画面越多、文件越大；90 大致相当于其他 AVIF "
                           "编码器所说的质量 90")},
        {QStringLiteral("Speed"), QString::fromUtf8("速度")},
        {QStringLiteral("How hard the encoder works, 0 (slowest, smallest) to 10. This is "
                        "most of the time an HDR capture takes"),
         QString::fromUtf8("编码器用多大功夫，0（最慢、最小）到 10。"
                           "一次 HDR 截图的时间大半花在这里")},
        {QStringLiteral("HDR to SDR"), QString::fromUtf8("HDR 转 SDR")},
        {QStringLiteral("How the SDR half of an HDR capture is made from the HDR one. "
                        "Auto reads each capture: an SDR picture comes out exactly as it "
                        "was, and one with highlights makes room for them. Fixed always "
                        "maps SDR white to the level below, so a pixel's value does not "
                        "depend on what else is in the picture. Normalize scales the "
                        "capture so its brightest point becomes white"),
         QString::fromUtf8("一次 HDR 截图的 SDR 那一半怎么由 HDR 那一半得到。"
                           "自动会逐张判断：本来就是 SDR 的画面原样输出，带高光的才腾出空间。"
                           "固定则总是把 SDR 白映射到下面那个档位，"
                           "一个像素的值不取决于画面里还有什么。"
                           "归一化把整张截图缩放到最亮处即白")},
        {QStringLiteral("SDR white level"), QString::fromUtf8("SDR 白电平")},
        {QStringLiteral("Where SDR white lands in the range, as a percentage. The rest is "
                        "spent on light above white, so a lower level keeps highlights more "
                        "apart and makes the picture dimmer. Used by Auto (only for a "
                        "capture that has highlights) and by Fixed"),
         QString::fromUtf8("SDR 白落在整个范围的百分之多少处。剩下的留给比白更亮的光，"
                           "所以档位越低，高光之间分得越开，画面也越暗。"
                           "自动（仅对带高光的截图）和固定会读它")},
        {QStringLiteral("Judge HDR by area"), QString::fromUtf8("按面积判定 HDR")},
        {QStringLiteral("Whether a capture counts as HDR content by how much of it is brighter "
                        "than SDR white rather than by any single pixel. A ten-bit PQ screen "
                        "rounds ordinary SDR white a few thousandths over, so with this off a "
                        "handful of rounding pixels can pass a whole desktop off as HDR and dim "
                        "it. Only outputs the compositor describes as HDR are asked at all"),
         QString::fromUtf8("一张截图算不算 HDR 内容，看的是有多大面积比 SDR 白更亮，"
                           "而不是有没有任何一个像素超过。十位 PQ 屏幕会把普通的 SDR 白"
                           "舍入得高出千分之几，所以关掉它时，几个舍入出来的像素就能把"
                           "整个桌面判成 HDR 并把它压暗。"
                           "只有合成器声明为 HDR 的输出才会被问到")},
        {QStringLiteral("HDR area"), QString::fromUtf8("HDR 面积")},
        {QStringLiteral("How much of the capture has to be brighter than SDR white to count as "
                        "HDR content, as a percentage of it. Zero means every capture of an HDR "
                        "output is HDR content, with no test at all"),
         QString::fromUtf8("截图里有多大比例比 SDR 白更亮才算 HDR 内容。"
                           "零表示 HDR 输出上的每次截图都算，完全不做检测")},
        {QStringLiteral("Off: one bright pixel is enough. On: the ratio below has to be met"),
         QString::fromUtf8("关：一个亮像素就够。开：要达到下面那个比例")},
        {QStringLiteral("Default monitor"), QString::fromUtf8("默认输出")},
        {QStringLiteral("Which output a capture takes when the command line names none. Leave it "
                        "empty to use whichever output the pointer is on -- `current` says the "
                        "same thing -- or write a name like `eDP-1` to pin one down"),
         QString::fromUtf8("命令行不指定时从哪块屏幕截图。留空就用指针所在的那块"
                           "——`current` 是同一个意思——"
                           "或者写 `eDP-1` 这样的名字钉住一块")},
        {QStringLiteral("follow the pointer"), QString::fromUtf8("跟随指针")},
        // A combo box's leading entry names the built-in default, so the label
        // is built from that name rather than being a word of its own.
        {QStringLiteral("%1 (built-in default)"), QString::fromUtf8("%1（内置默认）")},
        // "Pins" used to be the card heading the density row sat under on the
        // command-line page; the row is on the pin page now, under "Pin size",
        // so the entry went with it.
        {QStringLiteral("Density"), QString::fromUtf8("密度")},
        {QStringLiteral("inferred"), QString::fromUtf8("自动推断")},
        {QStringLiteral("Device pixels per logical pixel, 1-4; `inferred` works it out from the "
                        "screen the pin is on"),
         QString::fromUtf8("每逻辑像素对应多少设备像素，1-4；"
                           "`inferred` 表示按 pin 所在的那块屏幕自动推断")},
        {QStringLiteral("Scrolling capture"), QString::fromUtf8("滚动截图")},
        {QStringLiteral("Scroll"), QString::fromUtf8("长截图")},
        {QStringLiteral("Scroll the selection and stitch it into one tall image"),
         QString::fromUtf8("滚动选区并拼成一张长图")},
        {QStringLiteral("Scroll notches"), QString::fromUtf8("滚动格数")},
        {QStringLiteral("Wheel notches sent at a time"),
         QString::fromUtf8("一次发送的滚轮格数")},
        {QStringLiteral("Max height"), QString::fromUtf8("高度上限")},
        {QStringLiteral("Max frames"), QString::fromUtf8("帧数上限")},
        {QStringLiteral("Timeout"), QString::fromUtf8("超时")},
        {QStringLiteral("Ignore top"), QString::fromUtf8("忽略顶部")},
        {QStringLiteral("Rows at the top of every frame left out of the match, for sticky "
                        "headers"),
         QString::fromUtf8("每帧顶部不参与匹配的行数，用于吸顶表头")},
        {QStringLiteral("Scroll backend"), QString::fromUtf8("滚动后端")},
        {QStringLiteral("Text recognition"), QString::fromUtf8("文本识别")},
        {QStringLiteral("Notify when the text is ready"),
         QString::fromUtf8("识别完成时通知")},
        {QStringLiteral("A desktop notification with the text, or with why it failed; it "
                        "needs a notification daemon"),
         QString::fromUtf8("识别结束后弹一条桌面通知，给出文字或失败原因；"
                           "需要通知服务")},
        {QStringLiteral("Shown with the result once recognition ends"),
         QString::fromUtf8("识别结束时随结果一起显示")},
        {QStringLiteral("Recording"), QString::fromUtf8("录制")},
        {QStringLiteral("Encoder"), QString::fromUtf8("编码器")},
        {QStringLiteral("All three encode on the GPU's media engine"),
         QString::fromUtf8("三者都跑 GPU 的媒体引擎")},
        {QStringLiteral("Frame rate"), QString::fromUtf8("帧率")},
        {QStringLiteral("1-240"), QString::fromUtf8("1-240")},
        {QStringLiteral("Target bitrate"), QString::fromUtf8("目标码率")},
        {QStringLiteral("The size the file aims for; 45 Mbit/s whatever the frame's own size, "
                        "which is what a 4K recording needs to survive watching.  There is no "
                        "ceiling but the encoder's own"),
         QString::fromUtf8("文件想达到的大小；不管画面多大都是 45 Mbit/s，这是 4K 录制看着不糊"
                           "所需的量。除了编码器本身的硬上限，没有别的上限")},
        {QStringLiteral("The ring is held in memory, so this is what decides what a long window "
                        "costs; 45 Mbit/s whatever the frame's own size"),
         QString::fromUtf8("环是放在内存里的，所以这个决定一段长历史要占多少；"
                           "不管画面多大都是 45 Mbit/s")},
        {QStringLiteral("Encoder level"), QString::fromUtf8("编码器档位")},
        {QStringLiteral("from the bitrate"), QString::fromUtf8("按码率")},
        {QStringLiteral("On the codec's own scale and taken as written: 0-51 for h264 and hevc, "
                        "0-255 for av1, and 0 is the most expensive end of either.  A level makes "
                        "the encoder hold that quality and spend up to the bitrate instead of "
                        "spending it.  The file and the command line take every value; this box "
                        "leaves 0 for \"let the bitrate decide\""),
         QString::fromUtf8("按编码器自己的尺度，写多少就是多少：h264 与 hevc 是 0-51，"
                           "av1 是 0-255，两者都是 0 最贵。给了档位，编码器就守住这一档、"
                           "最多花到目标码率，而不是把码率花完。"
                           "文件与命令行接受任何取值；这个框把 0 留给「按码率」")},
        {QStringLiteral("As on the recording side: the codec's own scale, taken as written, and "
                        "0 left for \"let the bitrate decide\""),
         QString::fromUtf8("同录制侧：按编码器自己的尺度、写多少就是多少，"
                           "0 留给「按码率」")},
        {QStringLiteral("Through the desktop portal"),
         QString::fromUtf8("走桌面 portal")},
        {QStringLiteral("Needs xdg-desktop-portal and libpipewire; `record all` cannot use it"),
         QString::fromUtf8("需要 xdg-desktop-portal 与 libpipewire；"
                           "`record all` 用不了这条")},
        {QStringLiteral("The compositor's own picker decides what is recorded"),
         QString::fromUtf8("由合成器自己的选择器决定录什么")},
        {QStringLiteral("Microphone"), QString::fromUtf8("麦克风")},
        {QStringLiteral("Recorded into the same MP4 as an AAC track; the name is what the "
                        "file keeps"),
         QString::fromUtf8("与视频一起录进同一个 MP4（AAC 音轨）；"
                           "写进文件的是节点名")},
        {QStringLiteral("Do not record audio"), QString::fromUtf8("不录音")},
        {QStringLiteral("The session's default input"),
         QString::fromUtf8("会话的默认输入设备")},
        {QStringLiteral("Detect"), QString::fromUtf8("检测")},
        {QStringLiteral("Ask the running session which inputs it has"),
         QString::fromUtf8("问当前会话它有哪些输入设备")},
        {QStringLiteral("Hardware encoder"), QString::fromUtf8("硬件编码器")},
        {QStringLiteral("auto tries VAAPI then Vulkan then NVENC; all three encode on the GPU "
                        "(VAAPI and Vulkan import the dma-buf, NVENC copies frames via the CPU)"),
         QString::fromUtf8("auto 先试 VAAPI，再试 Vulkan，最后 NVENC；三者都是 GPU 硬编，"
                           "VAAPI 与 Vulkan 直接导入 dma-buf，NVENC 的帧经 CPU 搬运")},
        {QStringLiteral("Follow the focus"), QString::fromUtf8("跟随焦点")},
        {QStringLiteral("no windows to follow"),
         QString::fromUtf8("不跟随任何窗口")},
        {QStringLiteral("Window names, comma-separated (`record window` with no NAME); the "
                        "recording moves to whichever the focus lands on"),
         QString::fromUtf8("窗口名，用逗号分隔（对不带 NAME 的 `record window`）；"
                           "焦点落到哪扇就录哪扇")},
        {QStringLiteral("Window names, comma-separated (`replay start window` with no NAME); the "
                        "replay records whichever has the focus, keeps recording the last one "
                        "while the focus is elsewhere, and starts a new ring at that window's own "
                        "size when it moves"),
         QString::fromUtf8("窗口名，用逗号分隔（对不带 NAME 的 `replay start window`）；"
                           "焦点在哪扇就回录哪扇，焦点在别处时留在上一扇，"
                           "换到另一扇就按它自己的尺寸重开一段")},
        {QStringLiteral("Notify when the recording is written"),
         QString::fromUtf8("录制写盘时通知")},
        {QStringLiteral("Notify when a save is written"),
         QString::fromUtf8("保存写盘时通知")},
        {QStringLiteral("A desktop notification naming the file; it needs a notification daemon"),
         QString::fromUtf8("弹一条桌面通知，给出文件名；需要通知服务")},
        {QStringLiteral("A receipt for a recording started from a keybinding"),
         QString::fromUtf8("快捷键启动的录制，用它来报收")},
        {QStringLiteral("A receipt for a save triggered from a keybinding"),
         QString::fromUtf8("快捷键触发的保存，用它来报收")},
        {QStringLiteral("Replay"), QString::fromUtf8("回录")},
        {QStringLiteral("History kept"), QString::fromUtf8("保留时长")},
        {QStringLiteral("Seconds of history the ring holds, 1-3600"),
         QString::fromUtf8("内存环保留的历史秒数，1-3600")},
        {QStringLiteral("Key-frame distance"), QString::fromUtf8("关键帧间隔")},
        {QStringLiteral("1-10 seconds; smaller makes a save start closer to the moment you asked "
                        "for, at the cost of a bigger ring"),
         QString::fromUtf8("1-10 秒；越小，一次保存越贴近你要的时间点，代价是内存环更大")},
        {QStringLiteral("1-240; a rate below the recording's halves the encoder's work over a "
                        "long session"),
         QString::fromUtf8("1-240；比录制更低的帧率能把长时间挂着的编码量减半")},
        {QStringLiteral("An experimental route for a compositor vshot cannot capture directly"),
         QString::fromUtf8("对于 vshot 无法直接捕获的合成器，这是一条实验性的路线")},
        {QStringLiteral("Kept in the ring beside the video, as an AAC track"),
         QString::fromUtf8("与视频一起留在内存环里（AAC 音轨）")},
        {QStringLiteral("Save directory"), QString::fromUtf8("保存目录")},
        {QStringLiteral("the videos directory"), QString::fromUtf8("视频目录")},
        {QStringLiteral("Where `replay save` lands when it names no path; strftime is expanded"),
         QString::fromUtf8("`replay save` 不给路径时的落盘位置；展开 strftime 格式")},
        {QStringLiteral("File dialogs"), QString::fromUtf8("文件对话框")},
        {QStringLiteral("How the save and open windows are drawn. They are layer "
                        "surfaces, so the compositor draws them no decoration of its "
                        "own and the rim and shadow below are the only things "
                        "separating them from what is behind."),
         QString::fromUtf8("保存与打开窗口怎么画。它们是 layer surface，"
                           "合成器不给它们画任何装饰，下面这道框和阴影就是它们"
                           "与背后内容之间唯一的东西。")},
        {QStringLiteral("Shape"), QString::fromUtf8("形状")},
        {QStringLiteral("Corner radius"), QString::fromUtf8("圆角半径")},
        {QStringLiteral("0 draws square corners; the painted corner stops at half the "
                        "shorter side of the window"),
         QString::fromUtf8("0 表示直角；实际画的圆角不会超过窗口短边的一半")},
        {QStringLiteral("Frame"), QString::fromUtf8("外框")},
        {QStringLiteral("Border width"), QString::fromUtf8("边框宽度")},
        {QStringLiteral("0 draws no border at all"),
         QString::fromUtf8("0 表示完全不画边框")},
        {QStringLiteral("Border color"), QString::fromUtf8("边框颜色")},
        {QStringLiteral("Automatic derives one from the colour scheme"),
         QString::fromUtf8("自动：按配色方案推导")},
        // The shadow's four rows are shared by the pin page and the file-dialog
        // page, so each string here is the one both pages show.
        {QStringLiteral("Shadow"), QString::fromUtf8("阴影")},
        {QStringLiteral("Lifts it off whatever is behind it"),
         QString::fromUtf8("把它从背后的内容上抬起来")},
        {QStringLiteral("A soft shadow behind every one; turn it off for a hard edge"),
         QString::fromUtf8("身后一道柔和的阴影；关掉就是硬边")},
        {QStringLiteral("Shadow size"), QString::fromUtf8("阴影大小")},
        {QStringLiteral("How far the blur reaches past the edge; 0 turns the blur off"),
         QString::fromUtf8("模糊向外扩多远；0 表示不做模糊")},
        {QStringLiteral("Shadow offset"), QString::fromUtf8("阴影偏移")},
        {QStringLiteral("Drops the shadow below the edge; a negative value lifts it above"),
         QString::fromUtf8("把阴影往下压；负数则往上抬")},
        {QStringLiteral("Shadow opacity"), QString::fromUtf8("阴影浓度")},
        {QStringLiteral("0-255; the blur spreads this rather than adding to it"),
         QString::fromUtf8("0-255；模糊只是把它摊开，不会加深")},
        {QStringLiteral("Follow the colour scheme instead of a colour of its own"),
         QString::fromUtf8("跟随配色方案，不用自己的颜色")},
        {QStringLiteral("Pin appearance"), QString::fromUtf8("Pin 浮层")},
        {QStringLiteral("How a pinned image is drawn, and at what size. A pin is a layer "
                        "surface with nothing but the image in it, so its corners, the "
                        "shadow behind it and the line around it are all vshot's to "
                        "draw."),
         QString::fromUtf8("pin 图怎么画、按什么尺寸画。pin 是只装着图片的 layer surface，"
                           "所以它的圆角、身下的阴影和外面那道线都得 vshot 自己画。")},
        {QStringLiteral("Pin size"), QString::fromUtf8("Pin 尺寸")},
        {QStringLiteral("0 draws square corners, which is what a screenshot usually wants"),
         QString::fromUtf8("0 表示直角，截图一般就该是直角")},
        {QStringLiteral("0 draws square corners, which is what a screenshot usually wants; the "
                        "painted corner stops at half the shorter side of the image"),
         QString::fromUtf8("0 表示直角，截图一般就该是直角；实际画的圆角不会超过图片短边的一半")},
        {QStringLiteral("Border"), QString::fromUtf8("边框")},
        {QStringLiteral("0 draws no border at all"), QString::fromUtf8("0 表示完全不画边框")},
        {QStringLiteral("Automatic uses the built-in light grey"),
         QString::fromUtf8("自动：用内置的浅灰")},
        {QStringLiteral("Active border color"), QString::fromUtf8("激活时边框颜色")},
        {QStringLiteral("The pin the keyboard would act on; automatic uses black"),
         QString::fromUtf8("键盘会作用到的那张 pin；自动即黑色")},
        {QStringLiteral("Use the built-in colour instead of one of its own"),
         QString::fromUtf8("用内置颜色，不用自己的颜色")},
        // The numbered badge.  These strings were already wrapped in uiTr() but
        // had no entry here, so the whole number tool read as English on a
        // Chinese session.
        {QStringLiteral("Number"), QString::fromUtf8("序号")},
        {QStringLiteral("Number style"), QString::fromUtf8("序号样式")},
        // "Disc" rather than "Solid": the line-style row's own "Solid" is
        // already in this table, and one key cannot carry two translations.
        {QStringLiteral("Disc"), QString::fromUtf8("实心")},
        {QStringLiteral("Ring"), QString::fromUtf8("圆环")},
        {QStringLiteral("Square"), QString::fromUtf8("方框")},
        {QStringLiteral("Plain"), QString::fromUtf8("纯数字")},
        {QStringLiteral("Filled circle"), QString::fromUtf8("实心圆")},
        {QStringLiteral("Number: %1 (click to place a number)"),
         QString::fromUtf8("序号：%1（点击放置）")},
        {QStringLiteral("Number: %1 (click again to change the style)"),
         QString::fromUtf8("序号：%1（再次点击换样式）")},
        {QStringLiteral("Click to place a number; each click counts up from one"),
         QString::fromUtf8("单击放置序号，每次点击依次加一")},
        // The badge's own size, and the colour's opacity.
        {QStringLiteral("Size %1"), QString::fromUtf8("大小 %1")},
        {QStringLiteral("Alpha"), QString::fromUtf8("不透明度")},
        {QStringLiteral("Alpha %1"), QString::fromUtf8("不透明度 %1")},
        {QStringLiteral("Color opacity (0-255)"),
         QString::fromUtf8("颜色不透明度（0-255）")},
        // The wave's shape.
        {QStringLiteral("Amplitude"), QString::fromUtf8("幅度")},
        {QStringLiteral("Amplitude %1"), QString::fromUtf8("幅度 %1")},
        {QStringLiteral("Wave height (1-64 logical pixels)"),
         QString::fromUtf8("波高（1-64 逻辑像素）")},
        {QStringLiteral("Wavelength"), QString::fromUtf8("波长")},
        {QStringLiteral("Wavelength %1"), QString::fromUtf8("波长 %1")},
        {QStringLiteral("Wave period (6-256 logical pixels)"),
         QString::fromUtf8("波长，即一个周期多长（6-256 逻辑像素）")},
        {QStringLiteral("Drag between two points, then shape the wave with Amplitude "
                        "and Wavelength"),
         QString::fromUtf8("在两点之间拖出波浪线，再用「幅度」「波长」调整形状")},
        {QStringLiteral("Click a pixel to take its color for the %1"),
         QString::fromUtf8("点击像素取色，颜色交给「%1」")},
        // How a pen path is painted.
        {QStringLiteral("Pen fill mode"), QString::fromUtf8("钢笔填充方式")},
        // "Outline" rather than "Stroke": the settings window's line-style card
        // already owns that word, and one key cannot carry two translations.
        {QStringLiteral("Outline"), QString::fromUtf8("描边")},
        {QStringLiteral("Fill"), QString::fromUtf8("填充")},
        {QStringLiteral("Both"), QString::fromUtf8("填充+描边")},
        {QStringLiteral("Click to drop an anchor, drag from it to bend the curve, click the "
                        "first anchor to close, double click to finish"),
         QString::fromUtf8("单击落锚点，按住拖动弯出弧度，点回第一个锚点闭合，双击结束开放路径")},
        {QStringLiteral("Draw a wavy line between two points; the Amplitude and "
                        "Wavelength sliders shape it"),
         QString::fromUtf8("在两点之间画波浪线，用「幅度」「波长」滑块调整形状")},
        {QStringLiteral("Draw a curved path: click to drop an anchor, drag from it to "
                        "bend the curve, click the first anchor to close the path, "
                        "double-click to finish it open; Stroke/Fill/Both decides how "
                        "it is painted"),
         QString::fromUtf8("画曲线：单击落锚点，按住拖动弯出弧度，点回第一个锚点可闭合，"
                           "双击结束开放路径；用「描边/填充/填充+描边」决定怎么画")},
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
