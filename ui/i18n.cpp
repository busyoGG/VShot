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

        // The pinned color card's right-click menu: the heading, the action row
        // every pin has, and what the badge says once something has happened to
        // the pin.
        {QStringLiteral("Copy"), QString::fromUtf8("复制")},
        {QStringLiteral("Copied"), QString::fromUtf8("已复制")},
        {QStringLiteral("Copy failed"), QString::fromUtf8("复制失败")},
        {QStringLiteral("Save as…"), QString::fromUtf8("另存为…")},
        {QStringLiteral("Save pinned image"), QString::fromUtf8("保存浮层图片")},
        {QStringLiteral("PNG image (*.png)"), QString::fromUtf8("PNG 图片 (*.png)")},
        {QStringLiteral("Saved %1"), QString::fromUtf8("已保存 %1")},
        {QStringLiteral("Could not save the image"), QString::fromUtf8("图片保存失败")},

        // Pasting an image into the annotation editor.
        {QStringLiteral("Image"), QString::fromUtf8("图片")},
        {QStringLiteral("Text+"), QString::fromUtf8("取字")},
        {QStringLiteral("Copy the text in the selection to the clipboard"),
         QString::fromUtf8("把选区里的文字复制到剪贴板")},
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
        {QStringLiteral("PNG compression"), QString::fromUtf8("PNG 压缩")},
        {QStringLiteral("All levels are lossless; slower ones buy a smaller file"),
         QString::fromUtf8("各档全部无损；越慢换来越小的文件")},
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
        {QStringLiteral("auto tries VAAPI then NVENC; both encode on the GPU (NVENC copies frames via the CPU)"),
         QString::fromUtf8("auto 先试 VAAPI 再试 NVENC；两者都是 GPU 硬编，NVENC 的帧经 CPU 搬运")},
        {QStringLiteral("Follow the focus"), QString::fromUtf8("跟随焦点")},
        {QStringLiteral("no windows to follow"),
         QString::fromUtf8("不跟随任何窗口")},
        {QStringLiteral("Window names, comma-separated (`record window` with no NAME); the "
                        "recording moves to whichever the focus lands on"),
         QString::fromUtf8("窗口名，用逗号分隔（对不带 NAME 的 `record window`）；"
                           "焦点落到哪扇就录哪扇")},
        {QStringLiteral("Window names, comma-separated (`replay start window` with no NAME); the "
                        "ring moves to whichever the focus lands on"),
         QString::fromUtf8("窗口名，用逗号分隔（对不带 NAME 的 `replay start window`）；"
                           "焦点落到哪扇就回录哪扇")},
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
