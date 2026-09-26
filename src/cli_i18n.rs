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
/// in place, which is what a command without any English `after_help` wants.
const COMMANDS: &[(&str, &str, &str)] = &[
    (
        "",
        "严格冻结式 Wayland 截图工具，支持 wlroots 系与 KWin/Plasma",
        r#"vshot 只冻结一次桌面，之后所有捕获都来自那一帧静止画面，选区期间屏幕上不会有东西移动。

捕获目标
  region、monitor、all、long    冻结桌面的截图
  window active、window pick    焦点窗口，或你点中的那扇
  record monitor|all|region|window
                               GPU 编出的 MP4，一块屏或一扇窗
  record mics|stop             一次录制能用的音频输入
  replay start|save|status|stop
                               内存里留着的最近一段画面

输出目标（每次捕获恰好去其中一个）
  -o, --output PATH   写到 PATH 的 PNG，展开 strftime
                      （shots/%Y%m%d-%H%M%S.png）；写完后把文件 URI 复制进剪贴板，
                      `-` 则写到 stdout
  --clipboard         把 PNG 复制进剪贴板
  --pin               把图 pin 到屏幕上，由常驻 pin daemon 显示

共用修饰
  -c, --cursor        把合成器光标画进捕获
  --png-compression   none|fastest|fast（默认）|balanced|high，全部无损

合成器：wlroots 会话（Hyprland、Sway、labwc、niri）走 wlr-screencopy；KWin/Plasma 走
org.kde.KWin.ScreenShot2，只授给「已安装的 desktop 文件声明了
`X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2`」的客户端——包里给
/usr/bin/vshot 装了一个，所以直接从 target/ 跑的构建在 Plasma 下截不了图。niri 经 IPC
报不出平铺窗口的位置，那里的 `window active` 与 `window pick` 用它自己的截图与十字选窗。

`vshot pin` 自己不截屏：它驱动常驻 pin daemon，可以 pin 图片文件或 --clipboard 里的东西，
没有 pin 时 daemon 自己退出。

环境变量：VSHOT_QT_HELPER（用哪份 vshot-qt-ui）、VSHOT_LANG（界面语言）、
VSHOT_PIXEL_DEBUG=1、VSHOT_SESSION_DEBUG=1、VSHOT_LONG_DEBUG_DIR=<dir>、
VSHOT_PIN_DEBUG=1、VSHOT_PIN_FOCUS_DEBUG=1、VSHOT_PIN_SOCKET、VSHOT_PIN_DENSITY=N。

$XDG_CONFIG_HOME/vshot/config.json（或 ~/.config/vshot/config.json）可选：`editor` 是标注
编辑器样式，`cli` 给没写出来的参数提供默认值——命令行永远压过文件。文件损坏时回退内置默认。

每个子命令有自己的说明：`vshot <command> --help`。"#,
    ),
    (
        "region",
        "从冻结的桌面上选一块区域，也可用固定的 geometry",
        r#"在冻结场景里拖出矩形，用八个手柄或方向键调整（按住 Shift 一次走 10px），放大镜与尺寸
读数跟着指针，然后 Enter、在选区内双击或工具栏 OK 确认。Esc 或右键取消；文本框里的 Esc 只关
那个框。工具栏用矩形、椭圆、箭头、涂鸦、文本和马赛克标注：Ctrl+Z / Ctrl+Y 撤销重做，Delete
删除选中的标注，Ctrl+V 贴上剪贴板里的图片。最终 PNG 由标注重新渲染，与预览一致。

VSHOT_QT_HELPER 指定运行哪一份 vshot-qt-ui，VSHOT_LANG 指定它的语言。"#,
    ),
    (
        "monitor",
        "按名字截一块显示器，`current` 取指针所在的那块",
        r#"输出名取自冻结 overlay 的拓扑，所以每次捕获都需要一块能应答 `zxdg_output_manager_v1`
的输出。`current` 要读指针，需要一个带指针能力的 seat——嵌套或虚拟的 KWin 给不了；那种环境里
直接指名输出。"#,
    ),
    (
        "all",
        "截取完整的冻结桌面场景",
        "所有输出都按各自的逻辑位置摆放，多显示器桌面按实际布局拼合，输出之间的缝隙保持透明。",
    ),
    (
        "window",
        "截取焦点窗口，或交互式挑一扇",
        r#"`active` 从合成器元数据解析焦点窗口（Hyprland、Sway、KWin/Plasma 都可以查），查不到就
退回从捕获的像素里识别；`pick` 高亮实时桌面上的窗口（其余压暗），截取你点中的那扇。挑窗之后
会重新抓一帧，所以标注画在点击之后捕获的那一帧上。"#,
    ),
    (
        "long",
        "截取会滚动的区域：自动滚动、逐帧抓取，再拼成一张长图",
        r#"不给 --geometry 时交互式选区，然后 vshot 用合成的滚轮事件滚动这块区域，把抓到的帧拼成
一张长图。期间屏幕上有一条提示条：Enter、Space 或左键点它结束并保留，Esc 或右键取消。选区
必须落在一块显示器内。连续六次滚轮没有任何移动（到底了）即结束，或者到达 --max-height、
--max-frames、--timeout。

VSHOT_LONG_DEBUG_DIR=<dir> 把每一帧存成 grab-NNNN.png，并把每次拼接决定追加到 steps.log。"#,
    ),
    (
        "pin",
        "管理常驻 pin daemon 显示的 pin；能 pin 什么、pin 的行为见 --help",
        r#"pin 由常驻 daemon 持有：第一次 `pin` 启动它，没有 pin 时它自己退出。pin 出来的图可以
拖动，滚轮绕图像中心缩放（0.1x-8x，倍率显示在图的角落），双击关闭；光标停在哪张上、哪张就是
黑边，按 Space 进入与 `vshot region` 相同的标注编辑器。右键点击任意 pin 都会弹出菜单：色卡先
列出它的各种格式，点哪一项就把那个值抄回剪贴板；每张 pin 的最后一行都是「另存为…」，把图写成
PNG。

显隐控制通过 socket 发给常驻 daemon，因此 --toggle/--show/--hide 立即生效、也不会再起一个
daemon。Wayland 客户端收不到全局按键，请在合成器里自己绑热键，例如 Hyprland：
    bind = SUPER, P, exec, vshot pin --toggle
    bind = SUPER SHIFT, P, exec, vshot pin --close-all

VSHOT_PIN_SOCKET 覆盖 daemon 监听的 socket，VSHOT_PIN_DENSITY=N 指定每张 pin 图的来源密度，
与 --density 相同。VSHOT_PIN_DEBUG=1 与 VSHOT_PIN_FOCUS_DEBUG=1 把密度判定和渲染面的焦点
踪迹打到 stderr。"#,
    ),
    (
        "settings",
        "用窗口来改记住的设置：标注编辑器样式与命令行默认值，两者都在同一个配置文件里",
        r#"打开一个窗口，操作的就是标注编辑器与 CLI 一直在读的那份
`$XDG_CONFIG_HOME/vshot/config.json`。保存即写入文件，于是下次运行就是你挑的样式与默认值。
它不截任何图，除了显示一个窗口之外不需要任何合成器协议。"#,
    ),
    (
        "record",
        "把屏幕录成 MP4，用 GPU 编码",
        r#"编码在 GPU 的媒体引擎上完成，走 ffmpeg 的库（libavcodec），运行时加载：没有 ffmpeg 的
机器截图照常可用，只有 `record` 会说明缺什么。

目标：`monitor [NAME]` 一块输出（不写名字即 `current`，你所在的那块）、`all` 每块输出按逻辑
位置拼合、`region` 一块输出上的一块矩形（--geometry，或在冻结桌面上拖出）、`window [NAME]`
一扇窗自己的像素（不是它所在的屏幕区域）。组合桌面宽于 4096 像素时也要用 HEVC，这类 GPU 的
H.264 编不了那么宽。

录制一直进行到被停止：`vshot record stop` 发信号，或在启动它的终端里按 Ctrl+C；两种方式都会
在退出前把文件正常收尾（可寻址的 MP4，采样表写完整）。`record stop` 不需要显示器，适合绑键：
    bind = SUPER SHIFT, R, exec, vshot record stop

输出路径来自全局 -o/--output：展开 strftime，默认是视频目录下的
vshot-%Y%m%d-%H%M%S.mp4（先取 $XDG_VIDEOS_DIR，再取 xdg-user-dirs 那个，最后 ~/Videos），
不存在时建出来。合成器会说 linux-dmabuf 的会话上，帧不经过 CPU 直接进编码器；其余情况和
`record all` 走软件路径。

--portal 不是静默回退：合成器自己的选择器决定录什么，所以名字与 --pick 决定不了它，而
`record all --portal` 会被拒（portal 一次只给一路流）。屏幕投射只在画面变化时出一帧，所以那里
的 --fps 是上限而不是文件的帧率。需要 libpipewire 与 xdg-desktop-portal；VSHOT_PORTAL_SHM=1
改为要内存帧而不是 dma-buf。

没能开始的录制不会留下任何文件；进程被强行杀掉时留下的文件缺少采样表。

配置键：`cli.record` 提供 `encoder`、`encoder-backend`、`fps`、`portal`、`mic`、`follow` 与
`notify`；命令行永远压过文件，记着的 `follow` 列表只在不带窗口名的 `record window` 上生效。

VSHOT_RECORD_PIDFILE 覆盖 `stop` 读的 pid 文件，VSHOT_RECORD_DEBUG=1 追踪每帧的阶段，
VSHOT_RECORD_NO_OVERLAY=1 强制黑边走备用合成路径而不是 GPU overlay。"#,
    ),
    (
        "record monitor",
        "录一块输出；NAME 缺省即 `current`（你当前所在的那块）",
        r#"不写名字就是 `current`，即合成器说你在的那块输出——能报指针位置时是指针那块，否则是焦点
那块。录制期间不冻结桌面，帧是实时抓的，指针也不会进到 vshot 这边：`current` 是合成器的答案，
不是 seat 的。"#,
    ),
    (
        "record all",
        "录整个桌面：每块输出按各自的逻辑位置拼合",
        "每块输出按各自的逻辑位置逐帧拼合，多显示器桌面于是成为一段与桌面布局一致的视频。",
    ),
    (
        "record region",
        "录屏幕上的一块矩形——一个区域，而不是整块输出",
        r#"矩形用桌面逻辑坐标（`x,y 宽x高`），格式与 `vshot region --geometry` 相同，且必须落在
单块输出内：没有任何一个合成器调用能复制横跨两块输出的区域。不给 --geometry 时把冻结桌面交给
Qt overlay 拖出矩形，确认之前不打开任何设备、不创建任何文件。

wlroots 会话上合成器把这块矩形直接渲染进 dma-buf，与 `record monitor` 一样零拷贝进编码器。
被拖进矩形里的窗口会跟着录，但矩形本身固定在你画的位置。

加 `--portal` 时录的是选择器挑中的那一路流，而 portal 提供的是整块屏幕，矩形就不是这条命令
说了算了。"#,
    ),
    (
        "record window",
        "录一扇窗自己的像素，而不是它所在的屏幕区域",
        r#"合成器把窗口本身复制给你，所以被别的窗口盖住的窗口录出来是完整的，被拖到屏幕外一半的也
是完整的；窗口背后有什么，视频里不会出现。

窗口按 app id 或标题指定（先整名，再大小写不敏感的子串），`--pick` 点选，什么都不给就是焦点
那扇。这条路需要 `ext_image_copy_capture_v1`；niri 没有这个协议，改走它自己的投射服务
（`org.gnome.Mutter.ScreenCast`），不需要 flag、也不弹选择器，那条路不支持 `--follow`。

录制期间窗口被缩放不会中断：新尺寸会被等比缩放适配进录制自己的画布（缩小、居中、加黑边），
因为一个 MP4 只有一种帧尺寸。窗口被关掉则在那里结束，文件正常收尾；窗口所在的那块输出如果
关着或已断开，永远不会有帧送过来，这种情况几秒后会报出来。"#,
    ),
    (
        "record mics",
        "列出这次会话里能录的音频输入：`--mic` 收的就是这些",
        "",
    ),
    ("record stop", "停止正在进行的录制", ""),
    (
        "replay",
        "在内存里滚动保留屏幕的最近一段，需要时随时落盘",
        r#"回录把最近若干秒留在内存里而不是写文件：`replay save` 把环里现有的内容拷成一个 MP4——
流拷贝，不重新编码——所以触发几乎不花时间，触发之前不落盘。`--window` 秒就是一次保存能往回
够到的范围。

`replay start` 跑起会话，`replay save` 让它写文件，`replay status` 打印它握着多少历史，
`replay stop` 结束它。控制通道是 $XDG_RUNTIME_DIR 下的一个 socket，所以 save 与 stop 不需要
显示器，可以直接绑快捷键：
    bind = SUPER SHIFT, R, exec, vshot replay save

目标与 `record` 相同，帧来自相同的捕获后端、走相同的 GPU 编码器。`--fps` 默认 30（录制默认
60），因为回录会长时间挂着。

一次保存从「不晚于 `now - seconds` 的最后一个关键帧」开始，所以它至少有你要求的秒数；要的比
环里有的还多就给全部。`--background` 让会话脱离终端。

配置键：`cli.replay` 提供 `window`、`encoder`、`encoder-backend`、`fps`、`gop`、`mic`、
`follow`、`portal`、`save-dir` 与 `notify`；命令行永远压过文件，记着的 `follow` 列表只在
不带窗口名的 `replay start window` 上生效。

VSHOT_REPLAY_SOCKET 覆盖控制 socket，VSHOT_REPLAY_PIDFILE 覆盖 `replay stop` 读的 pid 文件，
VSHOT_RECORD_DEBUG=1 追踪每一帧。"#,
    ),
    (
        "replay start",
        "启动回录会话，把最近 `--window` 秒留在内存里",
        "",
    ),
    (
        "replay start monitor",
        "回录一块输出；NAME 缺省即 `current`（你当前所在的那块）",
        r#"与 `record monitor` 相同的选择：不写名字就是 `current`，即合成器说你在的那块输出；给了
就是输出名。回录期间也不冻结桌面，帧是实时抓的。"#,
    ),
    (
        "replay start all",
        "回录整个桌面：每块输出按各自的逻辑位置拼合",
        "与 `vshot all` 相同的拼合方式，但逐帧进行。",
    ),
    (
        "replay start region",
        "回录屏幕上的一块矩形——一个区域，而不是整块输出",
        r#"矩形用桌面逻辑坐标（`x,y 宽x高`），必须落在单块输出内。不给 --geometry 时把冻结桌面交给
Qt overlay 拖出矩形，确认之前不打开任何设备、不创建任何文件。wlroots 会话上走与
`record region` 相同的零拷贝 dma-buf 路线。"#,
    ),
    (
        "replay start window",
        "回录一扇窗自己的像素，而不是它所在的屏幕区域",
        r#"与 `record window` 相同的路线：合成器把窗口本身复制给你，所以被盖住、被拖到屏幕外的窗口
也录得完整。窗口按 app id 或标题指定，`--pick` 点选，不写就是焦点那扇。窗口被缩放时会适配进
回录的画布（缩小、居中、加黑边），因为一个环只有一个帧尺寸。"#,
    ),
    ("replay save", "把正在跑的会话的历史写成一个文件", ""),
    ("replay status", "打印正在跑的会话握有多少历史", ""),
    ("replay stop", "结束正在跑的回录会话", ""),
    (
        "ocr",
        "把屏幕上某块区域的文字读出来",
        r#"不给 --geometry 时，冻结场景交给 Qt overlay 框住文字——和 `vshot region` 一样，只是没有
标注编辑器：框一个矩形，按 Enter，文字就回来了（写到 stdout，加 --clipboard 则进剪贴板）。
--input 改为读一个图片文件。

识别跑 PaddleOCR 的 PP-OCR 模型（ONNX 版本），在本进程的 ONNX Runtime 上，用 CPU。模型装在
/usr/share/vshot/models，也在可执行文件旁边找。

要用 GPU，就把 $XDG_CONFIG_HOME/vshot/config.json 里的 `ocr.engine` 指向一个外部程序：给它
一张 PNG，它把文字写到 stdout。见 README 的 OCR 一节。"#,
    ),
    (
        "window active",
        "截取当前焦点窗口：合成器能自己画就画，否则用合成器元数据，再否则在捕获帧上做像素识别",
        r#"窗口像素能由合成器自己画就由它画：KWin 的 ScreenShot2 与 niri 的 `screenshot-window` 都
直接交出窗口自己的像素。niri 上半透明窗口带着 alpha 出来，vshot 会在该输出的一次新捕获上定位
这张渲染并裁取屏幕像素；定位失败会重新渲染并比对，内容变了（视频、动画）就带新渲染重试至多
3 次，内容没变则退回 niri 原样的透明渲染。其余情况矩形来自合成器元数据（Hyprland、Sway），
--pixel 则改为从捕获的帧上读描边带，再退回背景分割；无缝无边框平铺没有像素信号，会如实报错。
VSHOT_PIXEL_DEBUG=1 把每一级看到了什么打到 stderr。"#,
    ),
    (
        "window pick",
        "在屏幕上挑一扇窗：悬停高亮，点击截取",
        r#"实时桌面被显示出来，候选窗口逐个高亮、其余压暗；左键点击截取高亮的那扇，Esc 或右键取消。
点击之后会重新抓一帧，所以截到的是点击那一刻可见的窗口。候选来自合成器的窗口列表，除非给了
--pixel。

niri 上是它自己的挑窗：它的 IPC 报不出平铺窗口的位置，overlay 没有矩形可以高亮，所以改用
niri 画的一个十字（无高亮）。点中的窗口由 niri 自己渲染截图，因此之后同样没有标注编辑器；
--pixel 要回 overlay 与像素识别。半透明窗口从 niri 出来带着 alpha，vshot 会在该输出的一次新
捕获上定位并裁取；定位失败重试至多 3 次。`--no-blend` 跳过这整段定位，直接用 niri 交出来的
渲染。"#,
    ),
];

/// `(argument id, Chinese help)`.
///
/// The id is the field name clap derives (`--max-height` → `max_height`).  The
/// string is used for the short and the long form alike, and only where the
/// English side has one — an argument with no help stays without.  A flag that
/// appears on more than one command (`--fps` on `record` and `replay`, `--name`
/// on the monitors and the windows) gets one entry that has to read correctly
/// everywhere it shows up.
const ARGS: &[(&str, &str)] = &[
    ("cursor", "每次原生 screencopy 捕获时把合成器光标画进去。"),
    (
        "output",
        "把结果写到 PATH，展开 strftime（`-` 写到 stdout）：截图是 PNG 字节，写完后把文件 URI 复制进剪贴板。对 `record` 则是视频文件：`-` 被拒，没有 `.mp4` 后缀时补上。",
    ),
    ("clipboard", "把结果复制进剪贴板：截图是 PNG 字节，`vshot ocr` 是识别出的文字。"),
    ("pin", "把截到的图像 pin 到屏幕上，而不是写到任何地方。"),
    (
        "png_compression",
        "写到文件、stdout 或剪贴板的 PNG 压缩级别：`none`、`fastest`、`fast`（默认）、`balanced` 或 `high`，全部无损。`--pin` 不落盘。",
    ),
    ("geometry", "固定的全局矩形，格式为 `x,y 宽x高`。"),
    ("interactive", "明确要求用指针选区；不给 --geometry 时这就是默认行为。"),
    ("input", "改为读这个文件里的图片，而不是截屏；标注编辑器的取字按钮走的也是这条路。"),
    (
        "name",
        "输出名（`current` 是合成器说你在的那块，也是缺省值）；`record window` / `replay start window` 下是窗口的 app id 或标题，省略即焦点窗口。",
    ),
    ("notches", "捕获滚动时一次发送的滚轮格数。"),
    ("max_height", "拼接结果的高度上限，单位像素。"),
    ("max_frames", "单次捕获的帧数上限。"),
    ("timeout", "单次捕获的时限，单位秒。"),
    (
        "ignore_top",
        "每帧顶部不参与匹配的行数，用于吸顶表头和固定工具栏。",
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
        "循环瞄准的帧率，1-240；不写时跟随配置里的 `cli.record.fps` / `cli.replay.fps`（录制默认 60，回录默认 30）。",
    ),
    ("duration", "录制到这个秒数后自动停止。"),
    (
        "mic",
        "把麦克风收进来：不带值就是会话的默认输入设备，给名字则收另一个输入（音轨是 AAC），`record mics` 列出可选项。两个都不给时看配置里的 `cli.record.mic` / `cli.replay.mic`。",
    ),
    (
        "no_mic",
        "强制不收麦克风，即使配置里记着 `cli.record.mic` / `cli.replay.mic`。",
    ),
    (
        "app_audio",
        "把窗口自己的音频也收进来，与 `--mic` 合成同一条音轨。只有 `record window` / `replay start window` 有窗口可用，且不能与 --portal 同用。pid 来自合成器：Hyprland、niri、KWin 都报，Sway 与 labwc 没有，会明确说明。",
    ),
    (
        "follow",
        "焦点在这些窗口之间移动时换源：给若干窗口名（`--follow NAME`，可重复）。只有 `record window` 与 `replay start window`，且不能与窗口 NAME 或 `--pick` 同用；不写时跟随配置里 `cli.record.follow` / `cli.replay.follow` 记着的窗口。",
    ),
    (
        "no_follow",
        "强制不跟随焦点，即使配置里记着 `cli.record.follow` / `cli.replay.follow`。",
    ),
    (
        "encoder",
        "视频编码器：h264（默认）、hevc 或 av1，三者都跑 GPU 的媒体引擎；不写时跟随配置里的 `cli.record.encoder` / `cli.replay.encoder`。",
    ),
    (
        "encoder_backend",
        "硬件编码后端：auto（默认；VAAPI 优先，其次 Vulkan，最后 NVENC）、vaapi、vulkan 或 nvenc；不写时跟随配置里的 `cli.record.encoder-backend` / `cli.replay.encoder-backend`。三者都在 GPU 的媒体引擎上编码。",
    ),
    (
        "portal",
        "改走桌面 portal（org.freedesktop.portal.ScreenCast）而不是合成器自己的捕获协议；不写时跟随配置里的 `cli.record.portal` / `cli.replay.portal`。选择器决定录什么，所以 `record all --portal` 被拒。",
    ),
    (
        "no_portal",
        "强制不走 portal，即使配置里记着 `cli.record.portal` / `cli.replay.portal`。",
    ),
    (
        "window",
        "在内存里保留多少秒的历史，1-3600；不写时跟随配置里的 `cli.replay.window`，默认 30。",
    ),
    (
        "gop",
        "关键帧间隔的秒数（1-10）；不写时跟随配置里的 `cli.replay.gop`，默认 1。越小，一次保存越贴近你要的时间点。",
    ),
    (
        "save_dir",
        "`replay save` 不给路径时落盘的目录；展开 strftime。默认是视频目录。",
    ),
    ("background", "让回录会话脱离终端（只对 `replay start` 有效）。"),
    ("seconds", "从环里取多少秒，1-3600；不写就是整个 `--window`。"),
    (
        "path",
        "写到这个文件；不写时写到视频目录（或 `--save-dir`）下一个带时间戳的名字。",
    ),
    (
        "no_blend",
        "niri 专用：直接用它交出来的窗口渲染，不再合成到截下的背景上。半透明窗口于是带着 alpha 出来、背后空无一物，niri 的边框也不在内。这是「在屏幕上定位渲染」出问题时的备用路线。",
    ),
    (
        "pick",
        "点选一扇窗；窗口在隐藏工作区上也能录到，因为录的是它自己的像素。",
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
