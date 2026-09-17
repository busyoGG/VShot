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
--toggle/--show/--hide 控制所有 pin 的显隐。右键点击 pin 出来的色卡会弹出格式菜单，点哪一项就
把那个值抄回剪贴板。

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
涂鸦、文本和马赛克标注这一帧：Ctrl+Z / Ctrl+Y 撤销与重做，Delete 删除选中的标注，最终 PNG
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
    ("clipboard", "把 PNG 复制进 Wayland 剪贴板。"),
    ("pin", "把截到的图像 pin 到屏幕上，而不是写到任何地方。"),
    (
        "png_compression",
        "写到文件、stdout 或剪贴板的 PNG 压缩级别：`none`、`fastest`、`fast`（默认）、`balanced` 或 `high`。全部无损；越慢换来越小的文件。`fast` 与 `fastest` 用 fdeflate，一张 4K 帧几十毫秒就能编完；`balanced`（多数 PNG 写入器的默认档）与 `high` 可能要一秒以上。pin 到屏幕不落盘，所以对 `--pin` 不生效。",
    ),
    ("geometry", "固定的全局矩形，格式为 `x,y 宽x高`。"),
    ("interactive", "明确要求用指针选区。不给 --geometry 时这就是默认行为。"),
    ("name", "输出名；`current` 表示指针所在的那块输出。"),
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
        "no_blend",
        "niri 专用：直接用它交出来的窗口渲染，不再合成到截下的背景上。半透明窗口于是带着 alpha 出来、背后空无一物，niri 的边框也不在内（边框画在 tile 上，不在窗口上）。这是「在屏幕上定位渲染」出问题时的备用路线。",
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
