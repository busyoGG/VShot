// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 VShot contributors

//! Chinese translations for the CLI's own help output.
//!
//! The Qt helper has had this for a while (`ui/i18n.cpp`); the CLI did not, so
//! `--help` was English whatever `VSHOT_LANG` said.  The selection rule is the
//! helper's, and the table is addressed the way the helper's is — not by the
//! English wording, but by *place*: a subcommand path and an argument id.  That
//! keeps the Chinese independent of the English wording (the two drift on
//! purpose) and a missing or stale entry degrades to English instead of to an
//! empty label.
//!
//! Language selection, mirroring `ui/i18n.cpp`: `VSHOT_LANG` starting with `zh`
//! selects Chinese, any other non-empty value English, and without an override
//! the locale decides (`LANG`/`LC_ALL`/`LC_MESSAGES` mentioning `zh`).

use clap::Command;

#[derive(Clone, Copy, PartialEq)]
enum Language {
    English,
    Chinese,
}

fn language() -> Language {
    match std::env::var("VSHOT_LANG") {
        Ok(value) => {
            let value = value.trim().to_lowercase();
            if value.is_empty() {
                locale_language()
            } else if value.starts_with("zh") {
                Language::Chinese
            } else {
                Language::English
            }
        }
        Err(_) => locale_language(),
    }
}

/// Whether this side should show Chinese, for the user-facing text outside
/// `--help`: the notification `vshot ocr` finishes with (see `crate::notify`).
/// The rule is the one above, in one place.
pub(crate) fn prefers_chinese() -> bool {
    language() == Language::Chinese
}

/// The locale as far as the plain environment reaches — the helper asks Qt,
/// which reads the same variables.
fn locale_language() -> Language {
    let locale = ["LC_ALL", "LC_MESSAGES", "LANG"]
        .iter()
        .find_map(|name| std::env::var(name).ok())
        .unwrap_or_default()
        .to_lowercase();
    if locale.starts_with("zh") {
        Language::Chinese
    } else {
        Language::English
    }
}

/// `(subcommand path, about, after_help)`.
///
/// The path is the subcommand chain joined by spaces; `""` is `vshot` itself and
/// `window active` the nested one.  An empty `after_help` leaves the English one
/// in place — every command here has one, the field only exists so a future
/// entry can omit it.
const COMMANDS: &[(&str, &str, &str)] = &[
    (
        "",
        "严格冻结式 Wayland 截图工具，支持 wlroots 系与 KWin/Plasma",
        r#"vshot 只冻结一次桌面，之后所有捕获都来自那一帧静止画面，因此选区期间屏幕上没有任何东西会动。

捕获目标
  region              在冻结场景里拖出一个矩形，或用固定的 --geometry
  monitor [NAME]      按名字截一块输出，`current` 取指针所在的那块
  all                 所有输出，按各自的逻辑位置拼合
  window active       焦点窗口：优先合成器元数据，--pixel 则改用像素识别
  window pick         你点中的那扇窗：实时桌面上高亮候选、其余压暗
  long                一块会滚动的区域：vshot 替你滚动，边动边抓帧，再拼成一张长图
  record monitor|all|region|window
                      录成 MP4，GPU 编码：一块屏、整个桌面、一块屏上的矩形，或一扇窗自己的
                      像素（见下）
  record mics|stop    能录的音频输入列表，以及结束录制的那道信号
  replay start|save|status|stop
                      回录：把最近一段留在内存里，按键随时拷成 MP4（流拷贝，不重编）

输出目标：每次捕获恰好去 --output / --clipboard / --pin 中的一个，--cursor 与
--png-compression 对所有捕获生效；具体取值见上方 Options。

合成器：wlroots 会话（Hyprland、Sway、labwc、niri）经 wlr-screencopy 捕获；KWin/Plasma 走它
私有的 org.kde.KWin.ScreenShot2，而 KWin 只把权限授给「已安装的 desktop 文件声明了
`X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2`」的客户端——包里给 /usr/bin/vshot
装了一个，所以直接从 target/ 里跑的构建在 Plasma 下截不了图。焦点窗口自己的像素来自 KWin 与
niri 的截图调用；其下的矩形路线读取 Hyprland、Sway 或 KWin 的元数据。niri 经 IPC 报不出平铺
窗口的位置，所以那里的 `window active` 与 `window pick` 走 niri 自己的截图（挑窗也用 niri
自己的十字选窗）；`--pixel` 则直接走像素识别。

`vshot pin` 自己不截屏：它驱动常驻 pin daemon。可以 pin 图片文件；或者用 --clipboard 把剪贴板
里的东西 pin 出来——颜色（pin 成一张卡片，列出该颜色的 hex、RGB、HSL、HSV 与 CMYK）、图像，或
渲染成卡片的文字（保留 HTML、markdown 或代码格式）。pin 出来的图可以拖动，滚轮绕图像中心缩放，
双击关闭；光标停在哪张上、哪张就是黑边，按 Space 即进入与 `vshot region` 相同的标注编辑器；
--toggle/--show/--hide 控制所有 pin 的显隐。右键点击任意 pin 都会弹出菜单：色卡先列出它的各种
格式，点哪一项就把那个值抄回剪贴板；所有 pin 的最后一行都是「另存为…」，把这张图写成 PNG。

屏幕选区、挑窗与 pin 编辑器都由 Qt helper `vshot-qt-ui` 完成；VSHOT_QT_HELPER 可以指向另一份
副本。其余环境变量：VSHOT_LANG（界面语言）、VSHOT_PIXEL_DEBUG=1（窗口识别看到了什么）、
VSHOT_SESSION_DEBUG=1（本次会话被判成了哪个合成器、依据是什么）、VSHOT_LONG_DEBUG_DIR=<dir>
（长截图的每一帧与每次拼接决定）、VSHOT_PIN_FOCUS_DEBUG=1（pin 渲染面每一次拿到/交还键盘）、
VSHOT_PIN_SOCKET、VSHOT_PIN_DENSITY=N。

每个子命令有自己的说明：`vshot <command> --help`。"#,
    ),
    (
        "region",
        "从冻结的桌面上选一块区域，也可用固定的 geometry",
        r#"不给 --geometry 时，冻结场景交给 Qt overlay：拖出一个矩形，用八个手柄或方向键调整
（按住 Shift 一次走 10px），放大镜与尺寸读数跟着指针走，然后 Enter、在选区内双击或工具栏的
OK 确认。Esc 或右键取消整次截图——只有文本框里的 Esc 只关那个框。工具栏用矩形、椭圆、箭头、
涂鸦、文本和马赛克标注这一帧：Ctrl+Z / Ctrl+Y 撤销与重做，Delete 删除选中的标注，Ctrl+V 把
剪贴板里的图片贴上来（工具栏的「图片」按钮从磁盘挑一张）。贴进来的图按原尺寸落在选区正中，
比选区大时等比缩小，贴完是选中态，可以直接拖把手缩放。最终 PNG
由标注重新渲染，因此与预览一致。

VSHOT_QT_HELPER 指定运行哪一份 vshot-qt-ui，VSHOT_LANG 指定它的语言（以 `zh` 开头选中文，
其他非空值选英文；缺省跟随系统 locale）。"#,
    ),
    (
        "monitor",
        "按名字截一块显示器，`current` 取指针所在的那块",
        r#"输出名取自冻结 overlay 的拓扑，因此每次捕获都需要一块能应答 `zxdg_output_manager_v1` 的
输出。`current` 要读指针，所以需要一个带指针能力的 seat——嵌套或虚拟的 KWin 给不了；那种环境里
直接指名输出即可。"#,
    ),
    (
        "all",
        "截取完整的冻结桌面场景",
        "所有输出都按各自的逻辑位置摆放，多显示器桌面按实际布局拼合，输出之间的缝隙保持透明。",
    ),
    (
        "window",
        "截取焦点窗口，或交互式挑一扇",
        r#"`active` 从合成器元数据解析焦点窗口（Hyprland、Sway、KWin/Plasma 都可以查），查不到就退回
从捕获的像素里识别；`pick` 高亮实时桌面上的窗口（其余压暗），截取你点中的那扇。挑窗之后会重新
抓一帧，并交给与 `vshot region` 相同的编辑器，所以标注画在点击之后捕获的那一帧上，而不是挑选
开始时的那一帧。"#,
    ),
    (
        "long",
        "截取会滚动的区域：自动滚动、逐帧抓取，再拼成一张长图",
        r#"不给 --geometry 时交互式选区，然后 vshot 用合成的滚轮事件滚动这块区域，边动边抓帧，拼成
一张长图。期间屏幕上有一条提示条：Enter、Space 或在它上面左键点击结束并保留拼好的图，Esc 或
右键取消。选区必须落在一块显示器内。滚轮每 120 ms 发一次，帧按合成器交出来的速度抓取，所以
页面还在动、每帧与上一帧有重叠。连续六次滚轮没有任何移动（页面到底了）即结束，或者到达
--max-height、--max-frames 或 --timeout；对滚轮没反应的目标，捕获开始后不久就会结束。

VSHOT_LONG_DEBUG_DIR=<dir> 把每一帧存成 grab-NNNN.png，并把每次拼接决定追加到 steps.log。"#,
    ),
    (
        "pin",
        "管理常驻 pin daemon 显示的 pin；能 pin 什么、pin 的行为见 --help",
        r#"pin 由一个常驻 daemon 持有：第一次 `pin` 会启动它。pin 的对象是图片文件；或者用
--clipboard 把剪贴板里的东西 pin 出来——颜色（pin 成一张卡片，列出该颜色的 hex、RGB、HSL、HSV
与 CMYK）、图像，或渲染成卡片的文字（保留 HTML、markdown 或代码格式）。pin 出来的图可以拖动，
滚轮绕图像中心缩放（0.1x-8x，倍率显示在图的角落），双击关闭；光标停在哪张上、哪张就是黑边——
它就是按 Space 会打开的那个与 `vshot region` 相同的编辑器的对象。点击负责把键盘交给那块屏；
黑边跟着指针而不是键盘走，因为在用的这几个合成器都不会告诉一个 layer 面「你已经不是焦点了」。
一张 pin 会覆盖它压住的每一块输出，所以可以从一台显示器拖到另一台。

显隐控制通过 socket 发给常驻 daemon，因此 --toggle/--show/--hide 立即生效、也不会再起一个
daemon。Wayland 客户端收不到全局按键，所以没有内置热键：请在合成器里自己绑一个，例如 Hyprland：
    bind = SUPER, P, exec, vshot pin --toggle
    bind = SUPER SHIFT, P, exec, vshot pin --close-all

VSHOT_PIN_SOCKET 覆盖 daemon 监听的 socket，VSHOT_PIN_DENSITY=N 指定每张 pin 图的来源密度，与
--density 相同。VSHOT_PIN_DEBUG=1 与 VSHOT_PIN_FOCUS_DEBUG=1 让 daemon 把密度判定和渲染面的焦点
踪迹打到 stderr；只要开了任一调试开关，daemon 的 stderr 就会留在启动它的那个终端里，踪迹因此
可读，而 daemon 照常常驻。"#,
    ),
    (
        "window active",
        "截取当前焦点窗口：合成器能自己画就画，否则用合成器元数据，再否则在捕获帧上做像素识别",
        r#"窗口像素能由合成器自己画就由它画——KWin 的 ScreenShot2 与 niri 的 `screenshot-window`
都直接交出窗口自己的像素，场景里无需再找。niri 上半透明窗口带着 alpha 出来，vshot 会在该输出
的一次新捕获上定位这张渲染并裁取屏幕像素：输出即屏幕所见，背景俱全。定位失败先做稳定性核查：再渲染
一次并比对，内容未变说明是位置性失败（几乎全透明、悬在输出边缘、工作区不可见），退回 niri 原样的
透明渲染；内容变了（视频、动画）说明模板过期，带着新渲染重试（至多 3 次）。`--no-blend` 跳过这
整段定位，直接用 niri 交出来的渲染：绝不错位，但半透明处是空的、边框也不在内。其余情况矩形来自合成器元数据（Hyprland、Sway），
然后从冻结帧里裁出来，与 `region` 相同。--pixel 跳过元数据，改为从捕获的帧上读焦点描边，再
退回背景分割——没有任何窗口列表可用时也是这条路。无缝无边框平铺（无 gaps、无阴影）没有像素
信号，此时如实报错而不是瞎猜。VSHOT_PIXEL_DEBUG=1 把每一级看到了什么打到 stderr。"#,
    ),
    (
        "window pick",
        "在屏幕上挑一扇窗：悬停高亮，点击截取",
        r#"实时桌面被显示出来，候选窗口逐个高亮、其余压暗；左键点击截取高亮的那扇，Esc 或右键取消。
点击之后会重新抓一帧，所以截到的是点击那一刻可见的窗口，而不是悬停时的。候选来自合成器的窗口
列表，除非给了 --pixel。

niri 上是它自己的挑窗：它的 IPC 报不出平铺窗口的位置，overlay 没有矩形可以高亮，所以改用
niri 画的一个十字（无高亮）来指认窗口；点中的窗口由 niri 自己渲染截图——因此之后同样没有
标注编辑器。半透明窗口从 niri 出来带着 alpha，vshot 会在该输出的一次新捕获上定位这张渲染并
裁取屏幕像素：输出即屏幕所见，背景俱全。定位不到且窗口内容未变（几乎全透明、悬在输出边缘、工作区
不可见）则退回 niri 原样的透明渲染；内容在变化（视频、动画）则带新渲染重试至多 3 次后再退回。`--no-blend` 跳过这整段定位，直接用 niri 交出来的渲染：绝不错位，但半透明处是空的、边框也不在内。--pixel 要回 overlay 与像素识别。"#,
    ),
    (
        "settings",
        "用窗口来改记住的设置：标注编辑器样式与命令行默认值，两者都在同一个配置文件里",
        r#"打开一个窗口，操作的就是标注编辑器与 CLI 一直在读的那份
`$XDG_CONFIG_HOME/vshot/config.json`。保存即写入文件，于是下次 `vshot region` 就是你挑的样式，
下次没写出来的参数也按你挑的默认值走。它不截任何图，除了显示一个窗口之外不需要任何合成器协议，
所以在一个 vshot 本来截不了图的合成器上也能用。"#,
    ),
    (
        "record",
        "把屏幕录成 MP4，用 GPU 编码",
        r#"帧来自与截图相同的捕获后端（wlroots 会话走 wlr-screencopy，Plasma 走 KWin 的
ScreenShot2），编码在 GPU 的媒体引擎上完成。--mic 可把麦克风一起录进 MP4（AAC，`--no-mic`
强制不录；默认跟随配置里的 `cli.record.mic`）。编码与封装都跑在 ffmpeg 的库上，与 wf-recorder
同一条路线：libavcodec 出码流，libavformat 写 MP4 盒子，两者都在运行时用 dlopen 加载，所以
没有 ffmpeg 的机器截图照常可用，只有 `record` 会说明缺什么。`--encoder` 选编码器（h264 默认，
或 hevc、av1；不写时跟随配置里的 `cli.record.encoder`）。`monitor [NAME]` 录一块输出，NAME 缺省
即 `current`（问你当前所在的那块：
合成器能报指针位置时是指针那块，否则是焦点那块），`all` 把每块输出按各自的逻辑位置拼合录制，
`region` 录一块输出上的一块矩形（在冻结桌面上拖出，或用 --geometry 固定；wlroots 会话上由合成器
直接渲染进 dma-buf，与 `monitor` 一样零拷贝），`window [NAME]` 录一扇窗自己的像素（不是它所在的
屏幕区域）：被别的窗口盖住也录得完整，拖到屏幕
外一半也录得完整，窗口背后有什么都不会出现。窗口按 app id 或标题指定（先整名，再大小写不敏感的
子串），`--pick` 点选，不写就是焦点那扇。VIDEO_PATH 与截图路径
一样展开 strftime；默认是视频目录下的
vshot-%Y%m%d-%H%M%S.mp4（先取 $XDG_VIDEOS_DIR，再取 xdg-user-dirs 里那个目录，最后是 ~/Videos），
目录不存在时会建出来；名字没有 `.mp4` 后缀时会补上。

--portal 改走桌面 portal（org.freedesktop.portal.ScreenCast）录制，而不是合成器自己的捕获协议，
这是 vshot 说不了那个合成器语言时的路线。portal 不是静默回退：合成器会弹出它自己的选择器，
在那里选中的就是录下来的内容 —— `record monitor --portal` 让它列出屏幕，`record window
--portal` 让它列出窗口，但名字与 --pick 决定不了具体那一块。屏幕投射在画面变化时才出一帧，
不变时一帧都不出，所以 --fps 是向合成器要的采样上限、而不是文件的帧率，静止画面会变成一帧长
帧。`record all --portal` 会被拒绝：portal 一次只给一路流，而且哪块屏幕由 portal 说了算。这条
路需要 libpipewire（以及 xdg-desktop-portal）；VSHOT_PORTAL_SHM=1 改为要内存帧而不是 dma-buf，
这是编码器导入不了合成器缓冲时的备用路线。配置里记着 `cli.record.portal` 就不必每次写 --portal，
`--no-portal` 是对那一次录制把它关掉。

--mic 把麦克风录进同一个 MP4：不带名字就是会话的默认输入设备，给名字（或节点序号）录另一个
输入；音轨是 AAC，由 ffmpeg 自己的编码器编出。麦克风在视频编码器之前打开，因为它的采样率与
声道数要写进 MP4 的文件头；样本在每个视频帧后抽干一次，所以两条轨共用一个时钟。--no-mic 在
配置里记着 `cli.record.mic` 时也强制不录；两个都不给就是"按配置"。没有默认输入设备的会话会
得到一句提示，让你看 `wpctl status`，而不是一条看不出所以然的 PipeWire 错误。

`vshot record mics` 列出这次会话里能录的输入，每行三列：节点序号、节点名、说明。节点名就是
`--mic` 收的名字，设置窗口的麦克风一项也是照这份列表给的。没有输入设备的会话会得到一句说明，
而不是一条错误。

--encoder、--fps、--portal、--mic、--follow 都不给时，各自去配置的 `cli.record` 段取值；回录
对应 `cli.replay` 段，设置窗口里改的就是这些键（加上 `encoder-backend` 与回录的 `save-dir` /
`notify`）。

录制一直进行到被停止：`vshot record stop` 发信号，或者在启动它的终端里按 Ctrl+C。两种方式都会
在进程退出前把文件正常收尾（可寻址的 MP4，采样表写完整）。--duration 秒数让它自己结束。
--fps N 设定循环瞄准的帧率（1-240，默认 60；不写时跟随配置里的 `cli.record.fps`）；每帧带着它在
屏上停留的真实时长，所以回放跟随真实节奏而非名义帧率。`vshot record stop` 不需要显示器，可以
直接绑快捷键：
    bind = SUPER, R, exec, vshot record monitor current
    bind = SUPER SHIFT, R, exec, vshot record stop

录制没能开始时不会留下任何文件；进程被强行杀掉时留下的文件缺少采样表（播放器会如实报
"无法播放"，而不是放一段错的视频）。VSHOT_RECORD_PIDFILE 覆盖 `stop` 读取的 pid 文件，
VSHOT_RECORD_DEBUG=1 把每帧的阶段与所用 libavcodec 版本打到 stderr，
VSHOT_RECORD_NO_OVERLAY=1 强制黑边走备用合成路径而不是 GPU overlay（给不能混合的驱动用的测试开关）。"#,
    ),
    (
        "record monitor",
        "录一块输出；NAME 缺省即 `current`（你当前所在的那块）",
        r#"NAME 省略就是 `current`，即你当前所在的那块输出；给了就是输出名（同 `vshot monitor`）。录制期间
不冻结桌面：帧是实时抓的，屏幕上东西照常动——所以 `current` 问的是合成器，而不是 seat：
录制时 vshot 在屏幕上没有自己的面，指针不会进到我们这边来。合成器能报指针位置时（Hyprland）
取指针所在的那块，否则取焦点所在的那块。"#,
    ),
    (
        "record all",
        "录整个桌面：每块输出按各自的逻辑位置拼合",
        "与 `vshot all` 相同的拼合方式，但逐帧进行：多显示器桌面按实际布局合成为一段视频。",
    ),
    (
        "record region",
        "录屏幕上的一块矩形——一个区域，而不是整块输出",
        r#"矩形用桌面逻辑坐标表示，格式与 `vshot region --geometry` 相同（`x,y 宽x高`），且必须落在
单块输出内：没有任何一个合成器调用能复制横跨两块输出的区域。不给 --geometry 时把冻结的桌面
交给 Qt overlay，在那里拖出矩形——就是 `vshot region` 那套选区器，跑在实时桌面上——确认之前
不打开任何设备、不创建任何文件，所以取消选区不会留下任何东西。

wlroots 会话上合成器把这块矩形直接渲染进 dma-buf，与 `record monitor` 一样零拷贝进编码器；
会话没有 linux-dmabuf 时退回软件路径。录制跟随区域内容变化，所以被拖进矩形里的窗口会一起录；
但矩形本身固定在你画的位置——想换一块区域录制，停止后再开一次。

加 `--portal` 时录的是 portal 选择器挑中的那一路流，矩形就不是这条命令说了算了：portal 提供
的是整块屏幕或整扇窗口，录的是那边选中的屏幕。要录一块矩形，不要加 --portal。"#,
    ),
    (
        "record window",
        "录一扇窗自己的像素，而不是它所在的屏幕区域",
        r#"合成器把窗口本身复制给你，所以被别的窗口盖住的窗口录出来是完整的，被拖到屏幕外一半的
窗口也是完整的；窗口背后有什么，视频里不会出现。这是「窗口」，不是「窗口所在的那块区域」。
—— 那件事由 `record monitor` 对着一块屏幕录即可，但盖住窗口的东西会一起录进去。

窗口怎么指定：给 app id 或标题（先按整名匹配，再按大小写不敏感的子串），`--pick` 点选一扇，
什么都不给就是当前焦点那扇。它需要合成器的 `ext_image_copy_capture_v1` 与窗口列表
（`ext_foreign_toplevel_list_v1`）；没有这些的合成器会明确说明，让你改录屏幕。

录制期间窗口被缩放不会中断录制：新尺寸会被等比缩放适配进录制自己的画布（大则缩小、居中、
加黑边），因为一个 MP4 只有一种帧尺寸——窗口一直在屏幕上，文件就该是它完整的历史。窗口被关掉
则在那里结束：文件正常收尾（trailer 写完整），并说明原因。窗口所在的那块输出如果是关着、禁用或已断开，永远不会有帧送过来，
这种情况几秒后会报出来，而不是一直等下去。加 --portal 时由合成器自己的选择器挑窗口，这里写的
名字就决定不了具体哪一扇了；--portal 决定的是选择器列出窗口而不是屏幕。"#,
    ),
    (
        "record mics",
        "列出这次会话里能录的音频输入：`--mic` 收的就是这些",
        r#"每行三列，制表符分隔：节点序号、节点名、说明。节点名是 `--mic` 收的名字（比序号稳），说明是
给人看的。没有输入设备不是错误，只会说一句这个会话没有可录的输入——设置窗口的麦克风一项读的
就是这份输出。"#,
    ),
    (
        "record stop",
        "停止正在进行的录制",
        r#"读取 pid 文件（VSHOT_RECORD_PIDFILE 可覆盖）向录制进程发送 SIGTERM，并等它把文件
收尾完毕后返回。没有录制在跑时如实报错。"#,
    ),
    (
        "replay",
        "在内存里滚动保留屏幕的最近一段，需要时随时落盘",
        r#"回录是一段把最近若干秒留在内存里、而不是直接写文件的录制：屏幕持续编码，编码后的包进
内存环，`vshot replay save` 把环里现有的内容拷成一个 MP4 —— 流拷贝，不重新编码 —— 所以触发几乎不
花时间，触发之前不落盘。`--window` 秒就是一次保存能往回够到的范围。

`replay start` 跑起这个会话：一直编码到被停止，期间随时服务保存。`replay save` 让正在跑的会话写
一个文件，`replay status` 打印它现在握着多少历史，`replay stop` 结束它。控制通道是
$XDG_RUNTIME_DIR 下的一个 socket，所以 save 与 stop 都不需要显示器，可以直接绑快捷键：
    bind = SUPER, R, exec, vshot replay start --background
    bind = SUPER SHIFT, R, exec, vshot replay save
    bind = SUPER ALT, R, exec, vshot replay stop

录制目标与 `record` 相同：`monitor [NAME]`（不写即你当前所在的那块）、`all`、`region`（--geometry
或冻结桌面上拖出）、`window`（焦点窗口、按名字、或 `--pick`）。帧来自相同的捕获后端，走相同的
libavcodec GPU 编码器，所以能录的会话就能回录，`record` 用零拷贝 dma-buf 的地方回录也用。

编码器用有界关键帧间隔（`--gop` 秒，默认 1），因此环只是全帧内编码码流的一小部分，而且每个 GOP
边界都是一次保存可以起头的地方。一次保存从「不晚于 `now - seconds` 的最后一个关键帧」开始，所以
它至少有你要求的秒数，并且从第一个字节就能解码；要的比环里有的还多就给全部。

`--fps` 对回录默认 30（录制默认 60）：回录会长时间挂着，30 fps 把编码量减半，而动起来的画面看起
来依然顺。`--encoder` 选 h264（默认）、hevc 或 av1。`--mic` 像 `record --mic` 那样把麦克风一起留在
环里。

`replay save` 写进 `--save-dir`（展开 strftime，默认视频目录下带时间戳的名字），除非给了路径：
`vshot replay save /tmp/clip.mp4`。`--background` 让会话脱离终端，从而活得比启动它的 shell 久。

配置文件的 `cli.replay` 段给出默认值：`window`、`encoder`、`encoder-backend`、`fps`、`gop`、
`mic`、`follow`、`portal`、`save-dir` 与 `notify`。命令行永远压过文件——`--no-follow` 对那一次
会话把记着的 `follow` 列表关掉，正如 `--no-mic` 对记着的麦克风那样。记着的 `follow` 列表只在
不带窗口名的 `replay start window` 上生效，与录制侧同一条规则。

VSHOT_REPLAY_SOCKET 覆盖控制 socket，VSHOT_REPLAY_PIDFILE 覆盖 `replay stop` 读的 pid 文件，
VSHOT_RECORD_DEBUG=1 追踪每一帧。"#,
    ),
    (
        "replay start",
        "启动回录会话，把最近 `--window` 秒留在内存里",
        r#"会话一直编码到被停止，期间服务 save 与 status。目标与 `record` 相同：`monitor`、`all`、
`region`、`window`。`--background` 让它脱离终端，活得比启动它的 shell 久——快捷键里绑的就是这个
形状。回录按会话只跑一个（pid 文件挡着第二个），控制走 $XDG_RUNTIME_DIR 下的 socket。"#,
    ),
    (
        "replay save",
        "把正在跑的会话的历史写成一个文件",
        r#"把环里最近 `--seconds` 秒（不写就是整个 `--window`）拷进一个 MP4：一次流拷贝，不重新
编码，所以几乎不花时间。不给 PATH 时写到 `--save-dir`（默认视频目录下带时间戳的名字）。文件从
关键帧起头，所以它至少有你要的秒数，并且能从头解出来。没有回录在跑时如实报错。"#,
    ),
    (
        "replay status",
        "打印正在跑的会话握有多少历史",
        r#"输出两列，制表符分隔：环当前覆盖的秒数，以及这个会话已经服务过多少次保存。没有回录在跑
时报错。"#,
    ),
    (
        "replay stop",
        "结束正在跑的回录会话",
        r#"通过控制 socket 让会话退出（读 VSHOT_REPLAY_PIDFILE 等它离开），并清掉 socket 与 pid
文件。没有回录在跑时如实报错。"#,
    ),
    (
        "replay start monitor",
        "回录一块输出；NAME 缺省即 `current`（你当前所在的那块）",
        r#"与 `record monitor` 相同的选择：不给名字就是 `current`，即你当前所在的那块输出；给了就是
输出名。回录期间不冻结桌面，帧是实时抓的。"#,
    ),
    (
        "replay start all",
        "回录整个桌面：每块输出按各自的逻辑位置拼合",
        "与 `vshot all` 相同的拼合方式，但逐帧进行；`record all` 一样支持。",
    ),
    (
        "replay start region",
        "回录屏幕上的一块矩形——一个区域，而不是整块输出",
        r#"矩形用桌面逻辑坐标（`x,y 宽x高`），必须落在单块输出内。不给 --geometry 时把冻结桌面交给
Qt overlay 拖出矩形，确认之前不打开任何设备、不创建任何文件，所以取消选区不会留下任何东西。
wlroots 会话上走零拷贝 dma-buf，与 `record region` 相同。"#,
    ),
    (
        "replay start window",
        "回录一扇窗自己的像素，而不是它所在的屏幕区域",
        r#"与 `record window` 相同的路线：合成器把窗口本身复制给你，所以被盖住、被拖到屏幕外的窗口
也录得完整。窗口按 app id 或标题指定，`--pick` 点选，不写就是焦点那扇。窗口被缩放时会适配进回录
的画布（大则缩小、居中、加黑边），因为一个环只有一个帧尺寸。"#,
    ),
    (
        "ocr",
        "把屏幕上某块区域的文字读出来",
        r#"不给 --geometry 时，冻结场景交给 Qt overlay 框住文字——和 `vshot region` 一样，只是没有
标注编辑器：框一个矩形，按 Enter，它的文字就回来了。文字写到 stdout，加 --clipboard 则进剪贴板。
--input 改为读一个图片文件，标注编辑器里的「取字」按钮走的也是这条路。

识别用的是 PaddleOCR 自己的 PP-OCR 模型（转换出来的 ONNX 版本），跑在本进程的 ONNX Runtime 上，
用 CPU。模型装在 /usr/share/vshot/models，也在可执行文件旁边找，所以源码树里不装任何东西也能跑。

要用 GPU，就把 $XDG_CONFIG_HOME/vshot/config.json 里的 `ocr.engine` 指向一个外部程序：vshot 给它
一张 PNG，它把文字写到 stdout，vshot 自己不链接任何 GPU 运行时。那个配置项长什么样，见 README 的
OCR 一节。"#,
    ),
];

/// `(argument id, Chinese help)`.
///
/// The id is the field name clap derives (`--max-height` → `max_height`).  The
/// string is used for the short and the long form alike, and only where the
/// English side has one — an argument with no help stays without.
const ARGS: &[(&str, &str)] = &[
    ("cursor", "每次原生 screencopy 捕获时把合成器光标画进去。"),
    (
        "output",
        "把 PNG 写到 PATH，展开 `%Y%m%d` 这类 strftime 格式；写完后把文件 URI 复制进 Wayland 剪贴板。填 `-` 则写到 stdout。",
    ),
    ("clipboard", "把结果复制进 Wayland 剪贴板：截图是 PNG 字节，`vshot ocr` 是识别出的文字。"),
    ("pin", "把截到的图像 pin 到屏幕上，而不是写到任何地方。"),
    (
        "png_compression",
        "写到文件、stdout 或剪贴板的 PNG 压缩级别：`none`、`fastest`、`fast`（默认）、`balanced` 或 `high`。全部无损；越慢换来越小的文件。`fast` 与 `fastest` 用 fdeflate，一张 4K 帧几十毫秒就能编完；`balanced`（多数 PNG 写入器的默认档）与 `high` 可能要一秒以上。pin 到屏幕不落盘，所以对 `--pin` 不生效。",
    ),
    ("geometry", "固定的全局矩形，格式为 `x,y 宽x高`。"),
    ("interactive", "明确要求用指针选区。不给 --geometry 时这就是默认行为。"),
    (
        "input",
        "改为读这个文件里的图片，而不是截屏。标注编辑器的取字按钮用的就是这条路。",
    ),
    (
        "name",
        "输出名；`current` 表示你当前所在的那块输出。`record window` / `replay start window` 下是窗口的 app id 或标题，省略即焦点窗口。",
    ),
    ("notches", "捕获滚动时一次发送的滚轮格数。"),
    ("max_height", "拼接结果的高度上限，单位像素。"),
    ("max_frames", "单次捕获的帧数上限。"),
    ("timeout", "单次捕获的时限，单位秒。"),
    (
        "ignore_top",
        "每帧顶部不参与匹配的行数，用于吸顶表头和固定工具栏。从不动的行会被单独识别；这个参数是给那些会动的留的。",
    ),
    (
        "inject",
        "用哪个滚轮后端：`auto`（按顺序尝试）、`wlr`（合成器的虚拟指针协议）、`portal`（XDG RemoteDesktop portal）或 `uinput`（`/dev/uinput`）。",
    ),
    ("files", "要 pin 的图片文件（daemon 没在跑时会自动启动）。"),
    ("toggle", "翻转所有 pin 的显隐（没给其他开关时这是默认行为）。"),
    ("show", "显示所有 pin。"),
    ("hide", "隐藏所有 pin。"),
    ("close_all", "关闭所有 pin（daemon 保持常驻）。"),
    ("quit", "退出 pin daemon。"),
    ("list", "报告 pin 的数量与显隐状态。"),
    (
        "density",
        "pin 出的图每逻辑像素对应多少设备像素（1-4），例如在 2 倍屏上截的图就填 2。vshot 会自己从捕获、图片自带的 PNG 密度（96 DPI 声明即 1 倍）、截图工具的记录或图片尺寸里推断；推断错了或推不出来时，用这个覆盖。",
    ),
    (
        "apply",
        "内部使用：对 daemon 写出的 pin-edit 会话 JSON 跑一次标注编辑器，再把结果渲染回那张 pin。不是给交互使用的。",
    ),
    (
        "pixel",
        "跳过合成器元数据/窗口列表，直接从捕获的像素识别（先是描边带，再退回背景分割）。用于测试识别器、没有元数据或窗口列表查询的合成器；无缝无边框平铺没有像素信号，会如实报错。",
    ),
    (
        "fps",
        "循环瞄准的帧率，1-240（录制默认 60，回录默认 30）。不写 --fps 时跟随配置里的 `cli.record.fps` / `cli.replay.fps`。每帧带着它在屏上的真实时长，所以低了也不会变速。",
    ),
    (
        "duration",
        "录制到这个秒数后自动停止。",
    ),
    (
        "encoder",
        "视频编码器：h264（默认）、hevc 或 av1，三者都跑 GPU 的媒体引擎。不写 --encoder 时跟随配置里的 `cli.record.encoder` / `cli.replay.encoder`。某台机器的 ffmpeg 或显卡不支持所选编码器时，录制一开始就会说明是哪一个（例如 h264 的 4096 宽度上限）。",
    ),
    (
        "portal",
        "改走桌面 portal（org.freedesktop.portal.ScreenCast）录制，而不是合成器自己的捕获协议。合成器会弹出它自己的选择器，在那里选中的屏幕/窗口就是录下来的内容；一次只录一路流，所以 `record all --portal` 不支持。不写 --portal 时跟随配置里的 `cli.record.portal`，`--no-portal` 对那一次录制把它关掉。需要 xdg-desktop-portal 与 libpipewire；VSHOT_PORTAL_SHM=1 强制走内存拷贝（dma-buf 导入不了时的备用路线）。",
    ),
    (
        "mic",
        "把麦克风一起收进来。录制时它进同一个 MP4，回录时它留在内存环里。不带值就是会话的默认输入设备，给名字（或节点序号）则收另一个输入；音轨是 AAC，由 ffmpeg 自己的编码器编出。`vshot record mics` 列出这台机器上可以录的输入。",
    ),
    (
        "no_mic",
        "强制不收麦克风，即使配置里记着 `cli.record.mic` / `cli.replay.mic`。",
    ),
    (
        "no_portal",
        "强制不走 portal，即使配置里记着 `cli.record.portal`。`record all` 只能用这条：portal 一次只给一路流，整个桌面是合成器自己协议的活。",
    ),
    (
        "follow",
        "焦点在这些窗口之间移动时换源：给若干窗口名（`--follow NAME`，可重复），焦点落在其中哪扇就录（回录）哪扇，落在别处就保持上一扇。只有 `record window` 与 `replay start window` 有窗口可换，且不能与窗口 NAME 同用。`record window` / `replay start window` 不带任何 `--follow` 时，跟随配置里 `cli.record.follow` / `cli.replay.follow` 记着的窗口。",
    ),
    (
        "no_follow",
        "强制不跟随焦点，即使配置里记着 `cli.record.follow` / `cli.replay.follow`：这一扇——焦点窗口——从头录到尾。",
    ),
    (
        "window",
        "在内存里保留多少秒的历史；不写时跟随配置里的 `cli.replay.window`，默认 30。",
    ),
    (
        "gop",
        "关键帧间隔的秒数（1-10）；不写时跟随配置里的 `cli.replay.gop`，默认 1。越小，一次保存越贴近你要的时间点，代价是内存环更大。",
    ),
    (
        "save_dir",
        "`replay save` 不给路径时落盘的目录；展开 strftime。默认是视频目录。",
    ),
    (
        "background",
        "让回录会话脱离终端（只对 `replay start` 有效）。",
    ),
    (
        "seconds",
        "从环里取多少秒；不写就是整个 `--window`。",
    ),
    (
        "path",
        "写到这个文件；不写时写到 `--save-dir`（默认视频目录下带时间戳的名字）。",
    ),
    (
        "no_blend",
        "niri 专用：直接用它交出来的窗口渲染，不再合成到截下的背景上。半透明窗口于是带着 alpha 出来、背后空无一物，niri 的边框也不在内（边框画在 tile 上，不在窗口上）。这是「在屏幕上定位渲染」出问题时的备用路线。",
    ),
    (
        "pick",
        "点选一扇窗：实时桌面上高亮候选，点中的那扇被录/回录。窗口在隐藏工作区上也能录到——录的是窗口自己的像素，不是屏幕上那一块。",
    ),
];

/// Replaces every help string that has a Chinese translation.
///
/// The tree is walked in place, so `--help` and every subcommand's help follow
/// the same language the Qt helper's UI does.
pub fn localize(cmd: &mut Command) {
    localize_with(cmd, language());
}

/// `language` is a parameter on this level so a test can translate without
/// setting environment variables under a parallel test runner.
fn localize_with(cmd: &mut Command, language: Language) {
    if language != Language::Chinese {
        return;
    }
    // Global arguments (--cursor and friends) are propagated into the
    // subcommands during the build, so the tree has to exist before their help
    // can be translated everywhere they show up.
    localize_command(cmd, "");
}

fn localize_command(cmd: &mut Command, path: &str) {
    // Every clap builder takes `self`, so the command is swapped out, rebuilt
    // with its translations, and put back.
    let mut translated = std::mem::replace(cmd, Command::new("__vshot_localizing__"));
    if let Some((_, about, after_help)) = COMMANDS.iter().find(|entry| entry.0 == path) {
        translated = translated.about(*about);
        if !after_help.is_empty() {
            translated = translated.after_help(*after_help);
        }
    }
    for (id, zh) in ARGS {
        if translated.get_arguments().any(|arg| arg.get_id() == *id) {
            translated = translated.mut_arg(id, |arg| {
                let arg = if arg.get_help().is_some() {
                    arg.help(*zh)
                } else {
                    arg
                };
                if arg.get_long_help().is_some() {
                    arg.long_help(*zh)
                } else {
                    arg
                }
            });
        }
    }
    *cmd = translated;
    let names: Vec<String> = cmd
        .get_subcommands()
        .map(|sub| sub.get_name().to_string())
        .collect();
    for name in names {
        let child = if path.is_empty() {
            name.clone()
        } else {
            format!("{path} {name}")
        };
        if let Some(sub) = cmd.find_subcommand_mut(&name) {
            localize_command(sub, &child);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::cli::Cli;
    use clap::CommandFactory;

    #[test]
    fn every_command_path_resolves() {
        let mut cmd = Cli::command();
        // A table entry whose command was renamed would be skipped silently by
        // `localize_command`, so every path in the table has to exist here.
        // `""` is `vshot` itself and always resolves.
        for (path, ..) in COMMANDS {
            let mut current = &mut cmd;
            let mut ok = true;
            for name in path.split(' ').filter(|name| !name.is_empty()) {
                match current.find_subcommand_mut(name) {
                    Some(sub) => current = sub,
                    None => {
                        ok = false;
                        break;
                    }
                }
            }
            assert!(ok, "command path `{path}` does not exist in the tree");
        }
    }

    #[test]
    fn every_argument_id_exists_somewhere() {
        let mut cmd = Cli::command();
        localize_with(&mut cmd, Language::Chinese);
        let mut ids: Vec<String> = Vec::new();
        collect_argument_ids(&mut cmd, &mut ids);
        for (id, _) in ARGS {
            assert!(
                ids.iter().any(|known| known == id),
                "argument `{id}` has a translation but no such argument exists"
            );
        }
    }

    fn collect_argument_ids(cmd: &mut Command, ids: &mut Vec<String>) {
        for arg in cmd.get_arguments() {
            ids.push(arg.get_id().as_str().to_string());
        }
        for sub in cmd.get_subcommands_mut() {
            collect_argument_ids(sub, ids);
        }
    }

    #[test]
    fn translating_replaces_about_and_after_help() {
        let mut cmd = Cli::command();
        let english = cmd.get_about().map(|about| about.to_string()).unwrap();
        localize_with(&mut cmd, Language::Chinese);
        let chinese = cmd.get_about().map(|about| about.to_string()).unwrap();
        assert_ne!(english, chinese, "the top-level about was not translated");
        assert!(chinese.starts_with("严格冻结式"));
        let region = cmd.find_subcommand("region").expect("region exists");
        assert!(region
            .get_after_help()
            .map(|text| text.to_string().contains("放大镜"))
            .unwrap_or(false));
    }

    #[test]
    fn without_the_override_the_english_stays() {
        let mut cmd = Cli::command();
        localize_with(&mut cmd, Language::English);
        assert!(cmd
            .get_about()
            .map(|about| about.to_string().starts_with("Strict-freeze"))
            .unwrap_or(false));
    }
}
