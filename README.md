# vshot

**中文** | [English](README.en.md)

> **本项目是纯 vibe coding 项目**：需求由人提，代码与文档由 AI 写。

Rust 写的 Wayland 截图工具，带 Qt 交互界面与常驻 pin 浮层。捕获采用**严格冻结**：先把桌面拍成静态帧，之后所有选择与标注都在那一帧上做，选区期间屏幕上没有任何东西会动。已适配 Hyprland、niri、KWin/Plasma、Sway，以及任何提供 `wlr-screencopy` 的合成器（如 labwc）的基础截屏。

> **验证程度不一**：Hyprland 全部功能现场实测；niri 的平铺与浮窗截图、**窗口录制与回录**均已实机验证；KWin/Plasma 的 D-Bus 采集、窗口列表与长截图实测过，但 `--cursor`、滚动注入未验证；Sway 与 labwc **没有现场验证**。详见[「合成器适配与验证状态」](#合成器适配与验证状态)。

## 功能
- **区域截图**——冻结桌面后拖选，或直接给固定 geometry；8 向手柄调整，放大镜与尺寸读数跟随指针
- **标注编辑**——矩形、椭圆、箭头、涂鸦、文本、马赛克；撤销/重做、选中移动与缩放、颜色与线型、箭头头型、字号、系统字体选择
- **monitor / all**——按名字或指针位置截一块输出，或把整个桌面按逻辑位置拼合
- **window active / pick**——焦点窗口，或实时桌面上高亮点选；KWin 与 niri 直接交出窗口自己的像素
- **长截图**——框选会滚动的内容，自动发滚轮、逐帧抓取、按内容对齐拼成一张长图
- **录屏**——把屏幕、整个桌面、一块矩形或一扇窗自己的像素录成 MP4，GPU 硬编（VAAPI / Vulkan / NVENC 可选），音轨可选麦克风或**所录窗口自己的声音**
- **回录**——一直在编码但只留最近 N 秒在内存里，按键就把刚才那段拷成 MP4（流拷贝，不重编）
- **pin 浮层**——把图片或剪贴板内容钉在屏幕上：拖动、滚轮缩放、双击关闭、一键显隐、Space 进标注编辑
- **剪贴板贴图**——颜色、图片、复制的图片文件、纯文本（按 HTML / markdown / 代码 / 普通文本渲染成卡片）
- **OCR 取字**——框选一块区域把文字读出来（中英日），走 `vshot ocr`，编辑器工具栏里也有「取字」按钮，识别完弹一条桌面通知
- **输出目标**——文件（支持 strftime 路径）、stdout、剪贴板、屏幕 pin，四选一
- **中英双语**——界面与 `--help` 都跟随系统语言

## 安装（Arch Linux）
`PKGBUILD` 把 Rust CLI 和 Qt helper 打进同一个包，一次安装同时提供 `/usr/bin/vshot`、`/usr/bin/vshot-qt-ui`、给 KWin 授权用的 `/usr/share/applications/vshot.desktop`，以及一个带图标的启动入口 `/usr/share/applications/vshot-settings.desktop`（见[「应用菜单入口」](#应用菜单入口)）：

```sh
./scripts/build-arch-package.sh
sudo pacman -U dist/vshot-0.1.2-1-x86_64.pkg.tar.zst
```

脚本把当前工作树快照到临时目录再调 `makepkg`，产物写到 `dist/`（也可以直接 `makepkg -si`）。运行时依赖 `glibc`、`wayland`（通过 dlopen 使用 `libwayland-client`）、`qt6-base`、`layer-shell-qt`，以及 **`onnxruntime`**（OCR 的推理引擎）；文件输出、`--clipboard` 与 `vshot pin --clipboard` 需要可选依赖 `wl-clipboard`（写用 `wl-copy`，读用 `wl-paste`）。包同时装入约 30 MB 的 OCR 模型（`/usr/share/vshot/models/`，由 `makepkg` 下载并校验 SHA-256），其它发行版按下面的源码方式构建。模型缓存在 `$XDG_CACHE_HOME/vshot/makepkg-sources`（通常是 `~/.cache/vshot/makepkg-sources`），此后每次构建都从那里取，只有第一次要联网——设了 `SRCDEST` 就用你的，想重新下载删掉该目录即可。若本地 `models/` 已有这三个文件，直接拷进缓存可以省掉首次下载（校验和对得上）：

```sh
mkdir -p ~/.cache/vshot/makepkg-sources && cp models/* ~/.cache/vshot/makepkg-sources/
```

`onnxruntime` 在 Arch 上是**虚拟包**：`onnxruntime-cpu`、`onnxruntime-cuda`、`onnxruntime-rocm` 等六个变体都声明 `Provides: onnxruntime` 且互相冲突，所以系统里只会有一个。依赖写成虚拟包名而不是 `onnxruntime-cpu`，是为了让**已经装了某个 GPU 变体的人不必为 vshot 卸掉它**——卸它会连带带走 `rccl`、`migraphx`、`rocm-hip-sdk` 那一串。vshot 只用其中的共享库和 `.pc` 文件，也没有任何地方去选 GPU provider，所以 GPU 变体跑 OCR 时一样走 CPU。全新安装时 pacman 会让你挑一个，推荐 `onnxruntime-cpu`（连 cpuinfo、protobuf 一起约 46 MB，而 GPU 变体动辄上 GB）。

## 应用菜单入口
包会装一个 `vshot-settings.desktop`，在应用菜单里显示为 **VShot Settings**，点开就是 `vshot settings` 的图形设置界面；图标是 `icons/vshot.svg`，装在 `hicolor/scalable/apps/` 下（一个可缩放 SVG 而不是一整套尺寸）。Wayland **没有逐窗口的图标**：合成器拿客户端声明的 application id 去找 `<id>.desktop`，再画它 `Icon=` 指的东西，所以 `ui/main.cpp` 里声明的名字必须与安装的 desktop 文件名一致。这个 id 还会被 **Qt 注册给 host portal**，portal 找不到同名 desktop file 就往 stderr 打一句 `Failed to register with host portal ...`；因此 app id **只在 desktop file 确实装好时才声明**（用 `QStandardPaths::locate` 查 `ApplicationsLocation`），从源码树或 `build-qt/` 直接跑的副本不声明，也就不会触发那条报错。

给 KWin 授权用的那份 `vshot.desktop` 是**另一个文件**，不能合并进来：它的 `Exec=` 必须精确指向 `/usr/bin/vshot`（KWin 按 pid 取 `/proc/<pid>/exe` 比对 `Exec=` 第一个词），而且是 `NoDisplay=true`，因为不带子命令直接跑 `vshot` 只会报「没给输出目标」。

## 构建
```sh
cargo build --release --locked                                # Rust CLI
cmake -S . -B build-qt -DCMAKE_BUILD_TYPE=Release             # Qt helper
cmake --build build-qt --parallel
```

OCR 要 **`onnxruntime` 的开发文件**：`ort-sys` 通过 `pkg-config` 找 `libonnxruntime.pc`（Arch 上六个变体包都提供它），链接的是系统那一份而不是构建时另下一份；缺了它构建会失败并说明原因。模型放在源码树的 `models/`（`det.onnx` / `rec.onnx` / `dict.txt`，见[「OCR 取字」](#ocr-取字)），不装它们也能构建，只是 `vshot ocr` 会在运行时说找不到。交互功能会按顺序找 helper：`VSHOT_QT_HELPER` 环境变量、`vshot` 可执行文件同目录、其相对的 `../build-qt/` 与 `../../build-qt/`、最后 `PATH`（也可以显式指定）：

```sh
VSHOT_QT_HELPER="$PWD/build-qt/vshot-qt-ui" target/release/vshot region --output shot.png
```

运行时需要 Wayland 会话、`wl_compositor`、`wl_shm`、至少一个 `wl_output`、`zxdg_output_manager_v1`、`zwlr_layer_shell_v1`。**seat 只在真正读它时才要求**：`monitor <名字>`、`all`、`window active`、`region --geometry` 不碰 seat；`monitor current` 要 `seat pointer`，交互式选区要指针与键盘，缺哪个就以非零状态退出并指名。

## 用法
```sh
vshot region --output shot.png              # 区域截图：冻结后拖选
vshot region --clipboard
vshot region --geometry '100,200 800x600' --output shot.png

vshot monitor eDP-1 --output shot.png       # 输出：按名字
vshot monitor current --clipboard           # 指针所在的那块
vshot all --output desktop.png              # 整个桌面（输出间的空隙保持透明）

vshot window active --output window.png     # 窗口：焦点窗口
vshot window active --pixel                 # 跳过合成器元数据，用像素识别
vshot window pick --output window.png       # 实时桌面上点选
vshot window pick --pixel

vshot long --output long.png                # 长截图
vshot long --geometry '100,200 900x700' --output long.png
vshot long --ignore-top 48 --clipboard

vshot all --output - > desktop.png          # 输出到 stdout，日志只写 stderr

vshot region --pin                          # 截图直接 pin 到屏幕，不落盘

vshot pin shot.png another.png              # pin 管理
vshot pin --clipboard                       # pin 剪贴板内容
vshot pin --density 2 shot.png              # 手动指定来源屏倍率
vshot pin --toggle                          # 一键显隐
vshot pin --show
vshot pin --hide
vshot pin --close-all
vshot pin --list
vshot pin --quit

vshot settings                              # 设置：开窗口改编辑器样式与命令行默认值（写进 config.json）

vshot ocr                                   # OCR：框选，文字到 stdout
vshot ocr --clipboard                       # 同上，进剪贴板
vshot ocr --input shot.png                  # 读一个已有的图片文件

vshot record monitor eDP-1 --output clip.mp4    # 录屏：录一块屏
vshot record monitor --fps 30                   # 你当前所在的那块（NAME 省略即 current），30fps
vshot record all                                # 整个桌面，默认存视频目录
vshot record region --geometry '0,0 800x600'    # 一块矩形，桌面坐标
vshot record window                             # 焦点窗口自己的像素，盖住也录得完整
vshot record stop                               # 停止正在进行的录制
vshot record monitor current --duration 30      # 录 30 秒自动停

vshot replay start monitor --background         # 回录：后台挂着，默认留最近 30 秒
vshot replay save                               # 把这一段写成一个 MP4
vshot replay save /tmp/clip.mp4 --seconds 10    # 只取最近 10 秒
vshot replay status                             # 现在握着多少历史
vshot replay stop                               # 结束回录会话
```

全局参数对所有捕获生效：

| 参数 | 说明 |
| --- | --- |
| `-o, --output PATH` | 写 PNG 到 PATH，展开 `%Y%m%d` 这类 strftime 格式；写完后把文件的 `file://` URI 复制进剪贴板。`-` 表示写 stdout 且不复制 |
| `--clipboard` | 把 PNG 数据复制进剪贴板 |
| `--pin` | 把图像 pin 到屏幕，不落盘（daemon 读入内存后立即删除临时文件） |
| `-c, --cursor` | 请求合成器把光标画进每个输出帧。**`long` 不支持**（见[「已知不稳定点」](#已知不稳定点)）；截图里没有光标最常见的原因见[「光标（`--cursor`）」](#光标--cursor) |
| `--png-compression LEVEL` | `none` / `fastest` / `fast`（默认）/ `balanced` / `high`，全部无损，区别只在耗时与体积 |

每次捕获必须且只能给一个输出目标。`region --geometry` 与 `--interactive` 互斥，不给 geometry 时默认交互选择（`--interactive` 用于显式声明这一意图）。路径格式示例：

```sh
vshot region --output "$HOME/Pictures/vshot-%Y-%m-%d_%H-%M-%S.png"
vshot all --output 'shots/capture-%Y%m%d-%H%M%S.final.png'
```

`%Y` `%m` `%d` `%H` `%M` `%S` 分别为年、月、日、时、分、秒，`%%` 是字面 `%`。前缀后缀可任意组合。文件输出与剪贴板输出都需要 `wl-copy`。

## 区域截图与标注编辑
`vshot region` 不给 `--geometry` 时，冻结帧铺满每块输出，选区之外盖半透明暗色遮罩：

- 拖拽画矩形，四周 8 个手柄调整大小，拖选区内部移动位置，方向键微调（Shift 加速为 10 逻辑像素）；拖拽或调整时，光标旁显示 8x 放大镜与原生像素坐标，选区左上角显示 `宽 × 高`
- **Enter**、选区内双击或工具栏 OK 确认；**Esc** 或右键取消整次截图（文本框内的 Esc 只关闭文本框）
- 工具栏第一行为 Select、Rect、Ellipse、Arrow、Draw、Text、Mosaic 与 Undo、Redo、OK、Cancel；样式子面板按当前工具显隐，跟随选区移动
- 样式项：颜色色板（含自定义取色器：HSV 渐变 + 十六进制输入）、线型 Solid/Dash/Dot、箭头头型 Open V/Filled、粗细 1-64、箭头大小 1-8、字号 7-448（直接就是像素高）、马赛克形状 Rect/Ellip/Brush、马赛克程度 1-3、系统字体列表（每项按自身字形预览）。Arrow 是按下点到释放点的直线箭头；Draw 是自由绘制；Mosaic 的马赛克程度控制像素块大小与涂抹半径
- **Select** 工具可点选任意标注：单击选中，拖动移动（文本同样），形状/线条/马赛克可拖把手缩放，Delete/Backspace 删除；样式修改即时应用到选中标注；**Ctrl+Z / Ctrl+Y**（或 Ctrl+Shift+Z）撤销/重做。标注以全局逻辑坐标传回 Rust，最终 PNG 由内置软件渲染重绘，与预览一致
- **贴图 / 取字**：工具栏的「图片」按钮从磁盘挑一张，或 **Ctrl+V** 直接把剪贴板里的图贴进来——原尺寸落在选区正中，比选区大时等比缩小塞进去，贴完自动切到 Select 并选中它；「取字」按钮把选区里的文字读出来放进剪贴板（见[「OCR 取字」](#ocr-取字)，`cli.ocr.notify` 可关）

界面语言默认跟随系统（`QLocale::system()`），可用 `VSHOT_LANG` 覆盖：以 `zh` 开头选中文，其它非空值选英文。语言在 helper 启动时确定，切换需重新运行。Rust CLI 的 `--help` 走同一套判定，`VSHOT_LANG=zh vshot --help` 即中文。

## OCR 取字
`vshot ocr` 把屏幕上某块区域的文字读出来。不给参数就是框选：

```sh
vshot ocr                    # 框一块区域，文字到 stdout
vshot ocr --clipboard        # 同上，进剪贴板
vshot ocr --geometry '0,0 800x200'
vshot ocr --input shot.png   # 读一个已有的图片文件
```

识别完默认会弹一条**桌面通知**：成功给出识别到的文字（长了截断到 160 字），失败给出原因——`vshot ocr` 被快捷键拉起时，没有别的东西告诉你它跑完了。通知交给会话总线上 `org.freedesktop.Notifications` 的服务去画，**没有通知服务也照样能用**，只是少这条提示；开关在设置窗口的「文本识别」页，也可以手改 `cli.ocr.notify`。

剪贴板里放的是识别出的文字本身，**不会多出一个结尾换行**；只有输出到 stdout 时才补一个，免得终端的提示符挤在最后一行文字上。

识别用 **PaddleOCR 的 PP-OCR 模型**（官方模型转成的 ONNX 版本），跑在本进程的 ONNX Runtime 上，**用 CPU**。模型是 `PP-OCRv6_small` 这一档，约 30 MB：

| 文件 | 大小 | 作用 |
|---|---|---|
| `det.onnx` | 9.4 MB | 文本检测（语种无关） |
| `rec.onnx` | 20.3 MB | 文本识别 |
| `dict.txt` | 73 KB | 18708 字的字符表 |

装包后它们在 `/usr/share/vshot/models/`；源码树里放在 `models/`（`vshot ocr` 会从可执行文件逐级往上找，所以 `target/release/vshot` 和 `target/release/deps/vshot-*` 都找得到）。**这三个文件不进 git**，PKGBUILD 用 `source=()` 下载并校验 SHA-256。

### 用 GPU：外接引擎
要给 GPU 留口子，就指向一个**你自己的程序**。vshot 给它一张 PNG，它把文字按行写到 stdout，vshot 只负责启动它和读 stdout——**vshot 自己永远不链接 GPU 运行时**：

```json
{
  "cli": {
    "ocr": {
      "engine": "external",
      "external": {
        "command": ["/usr/bin/my-ocr", "--stdin"],
        "stdin": true,
        "timeout": 30
      }
    }
  }
}
```

- `command`：程序与参数，**数组**形式（不走 shell，空格不用转义）
- `stdin`：`true` 把 PNG 走 stdin 送过去；不给或 `false` 则把临时 PNG 的**路径**作为最后一个参数附上
- `timeout`：秒，默认 30，超时杀掉子进程、不挂住截图

这个程序可以是什么都行——用 ROCm wheel 的 Python `rapidocr`、`curl` 到另一台机器的服务、带 CUDA 的 ONNX Runtime。举一个 Python rapidocr 的例子：

```sh
#!/bin/sh
# /usr/local/bin/my-ocr —— 吃 stdin 的 PNG，吐文字
exec /path/to/venv/bin/python -c '
import sys
from rapidocr import RapidOCR
from PIL import Image
import io
img = Image.open(io.BytesIO(sys.stdin.buffer.read()))
result = RapidOCR()(img)
for text in (result.txts or []):
    print(text)
'
```

```json
{"cli": {"ocr": {"engine": "external",
                 "external": {"command": ["/usr/local/bin/my-ocr"], "stdin": true}}}}
```

**配置错了会直接报错，不会静默退回 CPU**：写了 `engine: "external"` 却没给 `command`、或者命令跑不起来、或者程序非零退出，都是明确的错误信息（外部程序写到 stderr 的内容会一并带上）。

## 截取窗口
### window active
按以下顺序取得焦点窗口：

- **KWin**（`CaptureActiveWindow`）与 **niri**（`niri msg action screenshot-window`）由合成器自己把这扇窗画出来，是唯一直接给出答案、不经过裁剪的一路：KWin 带装饰与阴影（阴影外圈透明），回复里的 `scale` 就是密度；niri 把窗口渲染成 PNG 写到临时路径再读回——niri 的 IPC 报不出平铺窗口的绝对位置，所以这是它上面唯一可行的路线。渲染不含边框（边框画在 tile 上），vshot 换算后补回；半透明窗口会对该输出再抓一帧、在帧上定位这张渲染并裁帧输出，让半透明处透出真实背景，定位失败时复核稳定性并至多重试 3 次。`--no-blend` 跳过整段定位，直接交出 niri 的渲染：绝不错位，但半透明处是空的、边框也不在内；
- 其余按顺序回落：Hyprland `hyprctl activewindow -j` → Sway `swaymsg -t get_tree` 中 focused node 的 `rect` → KDE Plasma 的 `kdotool` 或一次性 KWin scripting 探针（读 `workspace.activeWindow.frameGeometry`）→ **像素识别**（以上都不可用时，在捕获的帧上自动检测窗口）。

`--pixel` 跳过前面几路，直接在帧上做像素识别（用于测试检测器，也是只有矩形时的唯一选择）。识别**逐输出**用该输出自己的原生像素进行，候选按可信度分级（边框带 → 泛洪分割 → 闭合描边轮廓 → 整块输出），并且**把合成器画的那条边框包含在内**；合成器不给焦点窗口描有色的边时，候选按「指针所在输出 → 指针命中 → 面积」排序，给出的是你指着的那个窗口。无缝无边框平铺（无 gaps、无阴影）没有任何像素信号，此时如实报错。只问**当前会话自己的合成器**（按 `XDG_CURRENT_DESKTOP` / `XDG_SESSION_DESKTOP` 判定，两者都没写合成器名时才把所有探针都试一遍；niri 不看这两个变量，用 `NIRI_SOCKET` 的文件名 `niri.$WAYLAND_DISPLAY.$PID.sock` 认出来）。`VSHOT_PIXEL_DEBUG=1` 打印每一级的判定，`VSHOT_SESSION_DEBUG=1` 打印会话判定结果与依据。

### window pick
先在**实时桌面**上挑窗口：移动指针高亮指针下的窗口、其余压暗，左上角提示条给出窗口标题与尺寸，左键点击结束挑选，Esc 或右键取消；随后**重新捕获一帧**，把点击位置在当前的窗口列表上重新解析成窗口矩形，并以那一帧开一个与 `region` 相同的编辑会话——因此挑选期间切换工作区、移动窗口都不会让结果停在旧画面上。候选来自合成器的窗口列表（Hyprland `hyprctl clients`、Sway `swaymsg -t get_tree`、KDE KWin scripting 探针），按各自的叠放顺序自底向顶排列，指针命中的取**列表里最后一个包含指针的窗口**；候选跟着指针刷新，指针不动时每 300 ms 也刷一次；列表为空或查询失败时落到像素识别，`--pixel` 则跳过窗口列表、直接在冻结帧上做像素识别并把所有候选交给用户挑。

**niri 是例外**：它没有窗口列表可用，改用 niri 自己的十字选窗，像素由 niri 自己渲染——因此不画压暗 overlay，也没有标注编辑器。`--pixel` 时才回到 overlay + 像素识别。

## 长截图
`vshot long` 把一块会滚动的内容拼成一张长图。不给 `--geometry` 时先框选（与区域截图同一套选区交互，但确定后**不进入**标注编辑器）。之后：

1. 屏幕角落出现提示条，报告已拼接高度与帧数，接收 **Enter / Space / 左键点击**（保留结果）和 **Esc / 右键**（丢弃）；
2. vshot 按固定节拍发滚轮（默认每 120 ms 一批，`--notches` 决定每批发几档），同时以约 50 fps 的上限连续抓帧，每帧与上一帧对齐，位移大于 0 就把新增行接到长图底部；结束时按既有输出路径写出（`--output` / `--clipboard` / `--pin`）。结果不经过标注编辑器，想标注就先 pin 再用 pin 的编辑功能。

**停止条件**：连续 6 次滚轮都没有产生位移（页面到底）、`--max-height`、`--max-frames`、`--timeout`，或用户按键；一帧的位移超出可测范围时会把滚轮对半缩小（下限 1 档），连续多次仍对不上才停止并报告原因。对齐只取帧里**属于页面**的行：顶部与底部连续不动的行（标题栏、固定工具栏、状态栏）是 chrome，不进探针、也不会每帧重复；每帧只与上一帧比较，误差不累积。

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--geometry` | 交互框选 | 固定全局矩形，格式 `x,y 宽x高` |
| `--notches N` | 1 | 每次发送的滚轮档数 |
| `--max-height N` | 30000 | 拼接结果的高度上限（像素） |
| `--max-frames N` | 6000 | 单次捕获的帧数上限 |
| `--timeout N` | 120 | 单次捕获的时限（秒） |
| `--ignore-top N` | 0 | 每帧顶部 N 行不参与匹配，用于会动的吸顶表头 |
| `--inject M` | `auto` | 滚轮后端：`wlr`（合成器虚拟指针，Hyprland/sway/niri）、`portal`（XDG RemoteDesktop，KDE/GNOME 弹一次授权）、`uinput`（`/dev/uinput`，需要写权限） |

`auto` 按"打扰最少"的顺序挑。`VSHOT_LONG_DEBUG_DIR=<dir>` 把每次抓帧存成 `grab-NNNN.png`、每次拼接判定追加到 `steps.log`。

## 录屏
`vshot record` 把屏幕录成 MP4：帧来自截图用的同一套捕获后端（wlroots 会话走 `zwlr-screencopy`，Plasma 走 KWin 的 ScreenShot2），编码在 GPU 的媒体引擎上完成。

```sh
vshot record monitor eDP-1 --output clip.mp4    # 一块屏，NAME 同 `vshot monitor`；省略即 `current`（你当前所在的那块）
vshot record all                                # 多屏按逻辑位置拼合
vshot record region --geometry '0,0 800x600'    # 一块矩形（桌面坐标）；不给则在冻结桌面上拖出
vshot record window                             # 焦点窗口自己的像素（不是它所在的屏幕区域）
vshot record window --pick                      # 点选一扇窗来录，也可直接给 app id 或标题
vshot record monitor --encoder hevc             # 编码器：h264（默认）/ hevc / av1
vshot record monitor --encoder-backend nvenc    # 后端：auto（默认）/ vaapi / vulkan / nvenc（NVIDIA 硬编）
vshot record monitor --fps 120                  # 瞄准 120fps（1-240，默认 60）；`--duration 60` 则 60 秒后自动停
vshot record monitor --portal                   # 走桌面 portal：由合成器弹出选择器（`record window --portal` 列窗口）
vshot record monitor --mic                      # 录麦克风（会话的默认输入设备，也可给节点名）；`--no-mic` 强制不录
vshot record window --app-audio                 # 录这扇窗自己的声音（可与 --mic 同时给，混成一条音轨）
vshot record window --follow GameA --follow GameB   # 焦点在哪扇就录哪扇（`--no-follow` 强制不跟随）
vshot record mics                               # 列出这台机器上能录的音频输入
vshot record stop                               # 停止（读取 pid 文件发信号）
```

- **`current` 是哪块屏**：录制时 vshot 在屏幕上没有自己的面、收不到指针事件，所以 `current` 不问 seat 而是问合成器（`hyprctl` / `swaymsg` / `niri msg` / KWin 的 D-Bus，与 pin 判断落点是同一个查询）：能报指针位置时（Hyprland）就是指针所在的那块，否则是焦点所在的那块；都不报告时明确报错，让你用 `monitor NAME` 或 `all`。
- **区域录屏（`record region`）**：录一块输出上的矩形，桌面逻辑坐标与 `vshot region --geometry` 同格式（`x,y 宽x高`），必须落在单块输出内（没有合成器调用能复制跨屏区域，跨了会明确报错）。不给 `--geometry` 时在冻结桌面上拖出矩形（同一套选区器），确认前不打开麦克风、不创建文件。wlroots 会话上合成器把矩形直接渲染进 dma-buf，与 `record monitor` 一样零拷贝；编码尺寸是矩形的逻辑尺寸 × 该输出的 scale（2 倍屏上 400×300 录出 800×600）。`--portal` 与 `region` 互斥：portal 的选择器只提供整块屏幕/整扇窗口。
- **窗口录屏 ≠ 窗口区域录屏**：`record window` 录的是窗口**自己的像素**，由合成器把窗口本身复制出来（`ext_image_copy_capture_v1`）。所以被别的窗口盖住的、被拖到屏幕外一半的、在**别的工作区甚至当前屏幕上看不到**的窗口，录出来都是完整的，窗口背后有什么都不出现在视频里。窗口怎么指定：给 app id 或标题（先整名，再大小写不敏感的子串）、`--pick` 点选，不写就是焦点那扇；合成器没有这套协议时会明确说明，让你改录屏幕。
- **录制中缩放窗口**：窗口被**缩放**不会中断录制——新尺寸的画面会**等比缩放适配进录制自己的画布**（比画布大就缩小，小则原尺寸居中、两侧加黑边），所以一条 MP4 的帧尺寸始终不变，而文件是这扇窗完整的历史。窗口被**关闭**则在那一处结束，文件正常收尾并在 stderr 说明原因；窗口所在输出**关着/禁用/断开**时永远不会有帧，几秒后会报错而不是一直等。
- **录制怎么停 / 输出到哪 / 帧率**：`vshot record stop`（不需要显示器，可直接绑快捷键）或启动它的终端里按 Ctrl+C，两种方式都会先把文件正常收尾再退出，所以文件总是可寻址的 MP4；`--duration` 让它自己到点停。全局 `-o/--output` 展开 strftime，不给时写入视频目录下的 `vshot-%Y%m%d-%H%M%S.mp4`——视频目录先取 `$XDG_VIDEOS_DIR`，再取 `~/.config/user-dirs.dirs` 里 xdg-user-dirs 记的那个（中文桌面通常是 `~/视频`），最后才是 `~/Videos`；目录不存在时会建出来（你自己用 `-o` 指的路径不会被建）。名字没有 `.mp4` 后缀会自动补上，`-`（stdout）不接受。`--fps` 是循环"瞄准"的速率，每帧带着它在屏上的真实时长写入 MP4（可变帧率），所以编码跟不上时回放是"少几帧"而不是"变慢"；单块 4K 实测能稳定跑满 120fps。
- **编码器（`--encoder`）**：h264（默认）、hevc、av1 三选一，都跑 GPU 的媒体引擎。编码与封装走 ffmpeg 的库（`libavcodec` + `libavformat`，与 wf-recorder 同一条路线），运行时用 `dlopen` 加载，所以没有 ffmpeg 库的机器上截图照常工作，只是 `record` 会说明缺什么；需要 `ffmpeg` 与 `libva`（AMD/Intel 的 VAAPI）。码流每帧都是 IDR（全帧内），任意播放器可读，任何位置都能跳。
- **编码后端（`--encoder-backend`）**：`auto`（默认）、`vaapi`、`vulkan`、`nvenc` 四选一。`auto` 按**零拷贝优先**试：VAAPI（AMD/Intel）→ Vulkan → NVENC；写死某一个时只用那个，打不开就明确报错并说明是哪一层失败（例如 `no CUDA device for NVENC`）。**每条路都是硬件编码**（`h264/hevc/av1_vaapi`、`_vulkan` 或 `_nvenc`），vshot 里没有任何 x264/x265 之类的 CPU 编码路径；差别只在**帧怎么送到编码器**：VAAPI 与 Vulkan 都能导入合成器的 dma-buf，全程零拷贝，而 **NVENC 没有 dma-buf 导入**，它的帧要经 CPU 搬一趟（所以 CPU 占用略高，编码本身一样在 GPU 上）。**NVIDIA 上要零拷贝就得选 `vulkan`**：那里的 `*_vulkan` 底层就是同一颗 NVENC 硬件单元，而 `*_nvenc` 那条路永远拿不到 dma-buf。多显卡机器上 NVENC 默认用第一个 CUDA 设备，`VSHOT_NVENC_DEVICE=<索引>` 换一个；Vulkan 默认用第一个物理设备，`VSHOT_VULKAN_DEVICE=<索引>` 换一个。NVIDIA 机器还需要 `ffmpeg` 构建时带 `nvenc`（多数发行版默认带）。
- **麦克风（`--mic`）**：把麦克风录进同一个 MP4，编码是 AAC（ffmpeg 自己的编码器）。不带名字用会话的默认输入设备，给名字或节点序号录别的输入（`wpctl status` 列得出来，序号比如 `--mic 55`）。麦克风在视频编码器之前打开（采样率与声道数要写进 MP4 文件头），样本在每个视频帧后抽干一次，两条轨共用一个时钟。`--no-mic` 在配置文件记着 `cli.record.mic` 时也强制不录；两个都不给就是"按配置"（默认不录）。麦克风走 PipeWire，所以和 `--portal` 一样需要 libpipewire；没有默认输入设备的会话会明确报错并提示看 `wpctl status`。
- **逐应用音频（`--app-audio`）**：额外录一段所录那扇窗**自己在播的声音**——别的应用在响什么都不会进去。只在 `vshot record window` / `vshot replay start window` 上有意义（其它目标会被拒绝）。它可以**独立使用**，也可以**和 `--mic` 同时给**：麦克风是房间、应用音频是窗口，两者在 Rust 侧**逐样本相加**成 MP4 的那**一条**音轨（不是两条轨道）。窗口的 pid 由合成器报告（Hyprland、niri 会给，KWin 由 scripting 探针报），vshot 拿这个 pid 去 PipeWire 的客户端表里找到该进程的播放节点，只连那一个；Sway 与 labwc 不报窗口 pid，会明确说明做不到，而不是悄悄退回麦克风。窗口没在放声音时保留原有音源（首次录制则只录视频），而不是报错。`--portal` 与 `--app-audio` 互斥；音轨同样是 AAC。
- **跟随焦点（`--follow`）**：给若干窗口（`--follow NAME`，可重复），录制就跟着焦点在这些窗口之间移动——焦点落在其中哪扇就录哪扇，落在别处时**保持录上一扇**（不中断、不留空档，也不会把那扇窗录进来）。`--follow` 与窗口 `NAME`、`--pick` 互斥（它自己就是选窗方式），只对 `record window` 与 `replay start window` 有效。**不带 `--follow` 的 `record window` 会跟随配置里 `cli.record.follow` 记着的窗口**（回录对应 `cli.replay.follow`），`--no-follow` 则对那一次显式关掉。换源走的是**和窗口缩放同一条路**：新窗口的画面等比缩放进开录时的画布，所以**一条 MP4 只有一个帧尺寸**、时间线连续。`--app-audio` 时音轨也跟着换到新窗口的声音，而麦克风那一路**不动**；新窗口的应用若没在放声音，保留上一扇的，不退回麦克风。焦点查询每 250ms 一次，只在给了 `--follow` 时才问合成器；合成器报不出焦点时会**明确报错**而不是永不切换。注意跟随的是**焦点**，不是"正在玩的游戏"：切到不在白名单里的窗口时录制停在上一扇，但那一瞬间的操作不会被收录。
- **宽度上限 4096**：这是硬件 H.264 编码器的限制（本机 7900 XT 的 VCN 实测如此），所以两台 4K 屏拼合出的 `record all`（5760 宽）会被拒绝并说明原因——录单块屏即可，或改用 `--encoder hevc`（本机可编 7680 宽的全桌面）。单块 4K（3840）没问题。
- **记住的默认值**：`--encoder`、`--encoder-backend`、`--fps`、`--portal`、`--mic`、`--follow` 不写时，去配置文件的 `cli.record` 段取值（`replay` 对应 `cli.replay` 段，见 [`cli`——命令行默认值](#cli命令行默认值)）；`--no-portal`、`--no-mic` 与 `--no-follow` 是对**那一次**录制把记着的值关掉。`cli.record.follow` 只在**不带窗口名、也不给 `--pick`** 的 `record window` 上生效，别的目标会忽略它（而不是报错）。`vshot record mics` 列出这次会话里能录的音频输入，每行是 `节点序号	节点名	说明`（节点名就是 `--mic` 收的值），设置窗口的麦克风下拉读的正是它。`--app-audio` 只认命令行，不进配置文件。
- **`--portal`：走桌面 portal 录制**（`org.freedesktop.portal.ScreenCast`）。合成器自己的捕获协议各自只在部分桌面存在，而 portal 是每个桌面都有的那一套——代价是**由合成器决定录什么**：它会弹出自己的选择器，在那里选中的屏幕/窗口就是录下来的内容。`record monitor --portal` 让它列屏幕，`record window --portal` 让它列窗口，但名字与 `--pick` 决定不了具体是哪一个；每开一个会话都会弹一次选择器。一次只录一路流，所以 `record all --portal` 会被拒绝。屏幕投射是**按需出帧**的（画面变了才出一帧，所以静止画面在文件里是一帧长帧），`--fps` 是向合成器要的采样上限而不是文件帧率。帧以 dma-buf 送来时零拷贝进编码器，否则在 CPU 上转成 RGBA；`VSHOT_PORTAL_SHM=1` 强制走内存拷贝那条。需要 `xdg-desktop-portal` 与 `libpipewire`（编译要有头文件，库运行时 `dlopen`），缺了只是没有 `--portal`，其他功能照常。

## 回录
`vshot replay` 是「一直在录，但不落盘」的录屏：屏幕持续编码，编码后的包进一个**内存环**，`vshot replay save` 把环里现有的内容拷成一个 MP4 —— **流拷贝，不重新编码** —— 所以触发几乎不花时间，触发之前不写任何东西到磁盘。适合打游戏时挂着，出精彩操作按一下就把刚才那几十秒留下。

```sh
vshot replay start monitor --background     # 后台挂起，默认留最近 30 秒
vshot replay save                           # 落盘到视频目录，带时间戳
vshot replay save /tmp/clip.mp4 --seconds 10
vshot replay status                         # 环现在覆盖多少秒、已服务多少次保存
vshot replay stop
```

`start` 的目标和 `record` 完全一样：`monitor [NAME]`（不写即你当前所在的那块）、`all`、`region`（`--geometry` 或冻结桌面上拖出）、`window`（焦点窗口、按名字、或 `--pick`）。帧走相同的捕获后端、相同的 libavcodec GPU 编码器，`record` 用零拷贝 dma-buf 的地方回录也用。

回录用 `--gop` 秒的关键帧间隔（默认 1）而不是录制那样的全帧内编码，环因此只是全帧内码流的一小部分；保存从「不晚于 `now - seconds` 的最后一个关键帧」起头，所以文件至少有你要求的秒数、并且从第一个字节就能解码（要的比环里有的还多就给全部）。环因此要多留一个 GOP，否则会正好把要用的关键帧丢掉。

### 参数

| 参数 | 说明 |
| --- | --- |
| `--window N` | 内存里保留多少秒（1–3600，默认 30）。不写时跟随配置里的 `cli.replay.window` |
| `--fps N` | 回录默认 **30**（录制默认 60）：回录会长时间挂着，30 fps 把编码量减半 |
| `--gop N` | 关键帧间隔秒数（1–10，默认 1）。越小，一次保存越贴近目标时间点，代价是环更大 |
| `--encoder` | h264（默认）、hevc 或 av1，同 `record` |
| `--encoder-backend` | `auto`（默认）/ `vaapi` / `nvenc`，同 `record` |
| `--mic [DEVICE]` | 把麦克风一起留在环里，同 `record --mic`；`--no-mic` 强制不收 |
| `--app-audio` | 额外留所录窗口自己的声音（`replay start window`），同 `record --app-audio`；可与 `--mic` 同时给，混成一条 |
| `--follow NAME` | 焦点在这些窗口之间移动时换源（`replay start window`），同 `record --follow`；不带 `--follow` 时跟随配置里的 `cli.replay.follow` |
| `--no-follow` | 强制不跟随焦点，即使配置里记着 `cli.replay.follow` |
| `--save-dir DIR` | `replay save` 不给路径时的落盘目录（展开 strftime，默认视频目录，不写时跟随配置里的 `cli.replay.save-dir`） |
| `--background` | 只对 `replay start`：让会话脱离终端，活得比启动它的 shell 久 |

### 快捷键

控制走 `$XDG_RUNTIME_DIR/vshot-replay-<uid>.sock`，`save`/`status`/`stop` 都不需要显示器，可直接在合成器里绑：

```
bind = SUPER, R, exec, vshot replay start monitor --background
bind = SUPER SHIFT, R, exec, vshot replay save
bind = SUPER ALT, R, exec, vshot replay stop
```

按一下 `save` 会发一条桌面通知，告诉你文件落在哪、多长。

### 配置

默认值取自配置文件的 `cli.replay` 段（见 [`cli`——命令行默认值](#cli命令行默认值)，也可以跑 `vshot settings` 在「录制」页的回录一栏里改）；命令行永远压过文件，一个写坏的值（未知编码器名、越界的秒数）回落到内置默认，而不是让整个回录失败。回录一次只跑一个会话（pid 文件挡着第二个），`VSHOT_REPLAY_SOCKET` 覆盖控制 socket，`VSHOT_REPLAY_PIDFILE` 覆盖 `replay stop` 读的 pid 文件。窗口内容不变时合成器不会送新帧，回录按会话的帧率把手里那一帧再送一次，让环的时间线始终跟着真实时间走——静止十秒后按保存，拿到的仍是最后十秒；**录制不这么做**，一条录制里的静止画面就是一帧长帧，长度由收尾补上，不必重复编码，静止时也没有编码开销。

## pin 浮层
`vshot pin` 把图片作为浮层钉在屏幕上，由**常驻 daemon** 持有：

- **拖拽**移动（可跨显示器，跨屏时另一块屏上的副本同步跟随）；**滚轮**以图片中心缩放（0.1x–8x），倍率短暂显示在图片右下角；**双击**关闭该图；**左键点击**把该图提到最前，重叠时被点到的一定压在其余之上。鼠标停在哪张图上，哪张图的描边就是纯黑，其余是浅灰（2 逻辑像素粗，不遮挡图像本身）
- **外观可配**：圆角、阴影、边框宽度与两种状态的边框颜色都在配置文件里（默认直角 + 阴影），见[「`pin`——pin 浮层外观」](#pinpin-浮层外观)
- 指针在某张 pin 上且该屏持有键盘时按 **Space** 进入与 `vshot region` 相同的标注编辑器
- 右键点击**任意** pin 弹出菜单：色卡在前几行列出它的各种格式，点哪一项就把那个值抄回剪贴板（↑/↓ 选行、回车抄走、Esc 关闭；复制成功后右下角闪一个徽标）；最后一行 `另存为…` 对所有 pin 都在，选了就弹出保存对话框把这张图写成 PNG，存完在右下角报结果

新 pin 落在**激活的输出**上：指针所在的那块屏优先，指针读不到时退回键盘焦点所在的输出，都没有则回退主输出。pin 的对象：

```sh
vshot pin a.png b.png          # 图片文件
vshot pin --clipboard          # 剪贴板当前内容
vshot pin a.png --clipboard    # 可以混用
```

`--clipboard` 的内容按以下顺序解析：

1. **颜色**——剪贴板带 `application/x-color` 时 pin 出一张**色卡**（这一步排在图像之前，因为色板类程序常把颜色同时放成一张 1×1 色块图，pin 那个像素只会得到看不见的点）；
2. **内嵌图像数据**——`image/png`、`image/jpeg` 等；
3. **复制的文件**——`text/uri-list` 里第一张能解码的本地图片；
4. **纯文本**——内容为一个存在的本地图片路径；
5. **纯文本**——整段文字恰好是一个颜色字面量时 pin 出色卡；
6. **纯文本**——其余文字渲染成文字卡片，按内容自动选格式：带 `text/html` → 按 HTML 渲染并保留语法高亮配色；看起来是 markdown → GitHub 风格渲染；看起来是代码 → 等宽深色编辑器风格；其余 → 普通文本卡片（跟随系统亮暗主题，自动换行）。

色卡左边是颜色本身的色块，右边按 `HEX` / `RGB` / `HSL` / `HSV` / `CMYK` 逐行写出（半透明时多 `HEX8` 与 `RGBA`，有具名色时多 `NAME`），色块底下铺棋盘格表示半透明。文字卡片与色卡都按所在输出的像素密度渲染，HiDPI 下不模糊。剪贴板为空、有内容但既无图像也无可用文字、或缺少 `wl-paste` 时都会给出明确提示，且不影响已有的 pin。

### pin 的尺寸
pin 的尺寸按**图片的来源密度**决定，默认不需要任何参数。判定顺序：

1. **手动指定**——`--density N`（1–4）或环境变量 `VSHOT_PIN_DENSITY=N`，优先级最高；
2. **vshot 自己的截图**——直接带上来源输出的缩放；
3. **图片自己声明**——PNG 的 `pHYs` 块（192 DPI = 2 倍；96 DPI 就是 1 倍）。判定看**这个块在不在**而不是看 Qt 报回的数值（一张没有 `pHYs` 的 PNG 也会被 Qt 读成 96 DPI 左右），vshot 自己写出的 PNG 都会带上它；
4. **截图工具留下的记录**——`<图片路径>.scale` 文件里单独一个数字，或 `$VSHOT_PIN_SOURCE_FILE`（默认 `/tmp/screenshot-path`）里的一行 `<图片路径> <缩放>`（路径需与正在 pin 的图一致）。截图脚本保存图片后写一行即可：

   ```sh
   echo 2 > "$shot.png.scale"
   ```

5. **按图片尺寸与落点输出推断**——图片像素数放得进落点输出的原生分辨率时按 1:1；放不进时取"能容纳它的最小的那块屏"的缩放，最后再按输出宽度收一次上限（不放大）。因此同一张图 pin 到 2 倍缩放的 4K 屏上时占的逻辑尺寸是 1080p 屏上的一半，**两块屏上的物理大小一致**；`--density` 是最后的手动兜底。`VSHOT_PIN_DEBUG=1` 打印每张 pin 的判定（像素数、最终密度、来源、落点屏的 `devicePixelRatio`），`VSHOT_PIN_FOCUS_DEBUG=1` 打印每个渲染面每一次焦点变化（用来查描边颜色为什么不对）。

### pin 编辑模式
聚焦某个 pin 后按 **Space**，daemon 导出该图并拉起完整标注编辑器（工具栏、文字、马赛克、撤销/重做），与区域截图一致：

- 编辑器覆盖 pin 所在的整块屏幕，但**不自己绘制图片**——画面上的图就是那个真实的 pin，编辑器只在其上叠加标注，超出图片的标注会被裁掉；选中工具在图片上拖动时通过 daemon socket 复用 pin 自身的移动逻辑，**不产生副本**，方向键可微调（Shift 加速为 10px），图片不会被拖到屏幕外；
- **Esc 取消**：标注被丢弃、像素保持原样，但位置不回退——拖到哪儿就留在哪儿；**Enter 或 OK 确认**则连同标注一起写回；一次只能有一个 pin 处于编辑会话。

### pin daemon
layer-shell 浮层 surface 由创建它的进程拥有，因此需要一个常驻进程：

- 复用 Qt 二进制：`vshot-qt-ui --pin-server <socket>` 即 daemon，首次 `vshot pin` 连不上 socket 时自动分离式拉起（不占终端）；CLI 是瘦客户端，通过 Unix socket 发送单行 JSON 请求，socket 默认 `$XDG_RUNTIME_DIR/vshot-pin-<uid>.sock`，可用 `VSHOT_PIN_SOCKET` 覆盖。关闭最后一张 pin（或 `--close-all`）约 0.5 s 后 daemon 自动退出，下次 pin 命令自动重新拉起；`vshot pin --quit` 可随时手动退出；
- **不要用 `pkill` / `kill -9` 结束 daemon**：它持有 layer-shell surface，被强杀时部分合成器（实测 Hyprland 0.56）会残留该 surface 与其截屏会话，导致**所有输出的 screencopy 永久阻塞**（`vshot` / `grim` 全部超时，`hyprctl reload` 也无法恢复，只能重启会话）。请始终用 `vshot pin --quit`，它会在退出前 unmap 全部浮层；daemon 也已处理 `SIGTERM` / `SIGINT` 走同样的优雅路径。

Wayland 客户端收不到全局按键，所以"一键显隐"需要自己绑到合成器快捷键，例如 Hyprland：

```
bind = SUPER, P, exec, vshot pin --toggle
bind = SUPER SHIFT, P, exec, vshot pin --close-all
```

## 图像与输出映射
**落在单个输出内的矩形一律从那块输出自己那份原生帧裁剪，并按那块屏的 scale 写密度**：`region` 的 `--geometry` 与交互选择、`window active`、`window pick` 以及像素识别给出的矩形都走这条路，只有**跨接缝**的矩形才回落到合成场景；`all` 是整块桌面，只能由场景给出。内部帧统一为 RGBA8、top-left origin，多输出合成支持负 logical origin 和输出间空隙（场景画布用最高输出 scale，较低 scale 的输出用 nearest-neighbor 放大）。当前要求正整数 scale、`transform=normal` 以及可安全证明的 logical/pixel 映射；fractional scale、旋转和无法证明的映射会清晰失败，而不是生成疑似错误的截图。这个校验只在**需要把输出合成为场景**的路径上生效，所以 KWin 与 niri 直接给窗口像素的两条路在旋转/翻转输出上仍然可用。

## 截图后端与 KDE 授权
启动时探测一次：先连 `wlr-screencopy-unstable-v1`，只有它以「缺少 `zwlr_screencopy_manager_v1`」失败时才说明这个合成器不提供该协议；再试 **KWin ScreenShot2**——KWin 的私有会话总线服务 `org.kde.KWin.ScreenShot2`（KWin 既没有 screencopy，也没有 `ext-image-copy-capture`）。vshot 传一根管道的写端，KWin 把像素写进管道并在回复里给出 `width` / `height` / `stride` / `format` / `scale`；像素是预乘 alpha 的 BGRA，输出截图把 alpha 归一为 255，窗口截图原样保留。

两个后端都不可用时，错误信息会同时说明两边的具体原因。两者都没有实现 PipeWire、Portal ScreenCast 或 DMA-BUF，GNOME/Mutter 三者都不提供，连 layer-shell 也没有，因此**不支持 GNOME**。

### KDE 授权
KWin 的 `ScreenShot2` 是**受限 D-Bus 接口，且不弹任何对话框**——没有可点的"允许"。KWin 由调用方 pid 取 `/proc/<pid>/exe`，在已安装的 desktop file 里找 `Exec=` 第一个词与该路径一致的那一份，然后要求它声明：

```ini
X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2
```

- **装包安装的 vshot 不需要任何额外操作**（包里的 `/usr/share/applications/vshot.desktop` 已经声明）；若装完包 KWin 仍拒绝，跑一次 `kbuildsycoca6 --noincremental`，新建的 desktop file 可能还有几秒延迟，隔几秒重试即可。**直接从 `target/release/vshot` 跑的开发构建拿不到授权**——没有任何 desktop file 的 `Exec=` 指向那个路径，加上一份即可（`Exec=` 写当前二进制的绝对路径，vshot 报错时会把这个路径打出来）：

  ```sh
  printf '[Desktop Entry]\nType=Application\nName=VShot\nExec=/path/to/vshot\nNoDisplay=true\nX-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2\n' \
    > ~/.local/share/applications/vshot.desktop
  kbuildsycoca6 --noincremental
  ```

- 合成器侧的 `KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1` 跳过整个检查，**仅供开发/测试**。

## 合成器适配与验证状态
### 各功能在用什么机制

| 功能 | Hyprland | niri | KWin/Plasma | Sway | labwc |
| --- | --- | --- | --- | --- | --- |
| 截屏 | wlr-screencopy | wlr-screencopy | KWin ScreenShot2 | wlr-screencopy | wlr-screencopy |
| `window active` | `hyprctl activewindow -j` | niri 自己的 `screenshot-window` | KWin `CaptureActiveWindow` | `swaymsg -t get_tree` | ❌ 无 |
| `window pick` | `hyprctl clients -j` | niri 自己的十字选窗 | KWin scripting 探针 / `kdotool` | `swaymsg -t get_tree` | ❌ 无 |
| **录制/回录窗口**（`record window`、`replay start window`） | `ext_image_copy_capture_v1`（窗口自己的 dma-buf） | **合成器自己的 ScreenCast 服务**（`org.gnome.Mutter.ScreenCast`，按窗口 id 抓那扇窗自己的像素，PipeWire 流） | **KWin `ScreenShot2.CaptureWindow`**（按窗口 `QUuid` 抓，CPU 像素） | ❌ 无 | ❌ 无 |
| 跟随焦点（`--follow`） | ✅ `hyprctl activewindow` | ⚠️ 焦点查询有（`niri msg focused-window`），但走 ScreenCast 服务的窗口录制无法中途换窗 | ✅ scripting 探针 / `kdotool` | ⚠️ 查询有，但窗口录制本身不支持 | ❌ 无 |
| `--app-audio` 取窗口 pid | `hyprctl clients -j` | ✅ `niri msg windows` 报 pid | ✅ scripting 探针的 `pid` 字段 | ❌ 无 | ❌ 无 |
| pin 落在哪块屏 | `hyprctl cursorpos` + `monitors -j` | `focused-output`（只跟键盘焦点） | `org.kde.KWin.activeOutputName` | `swaymsg -t get_outputs` | ❌ 无 |
| 滚动注入 | wlr 虚拟指针 | wlr 虚拟指针 | portal / uinput | wlr 虚拟指针 | uinput |

没有适配的地方会**自动降级**而不是报错：没有窗口列表就落到像素识别，问不到指针在哪块屏就落到 Qt 报的主屏。

### 验证到哪一步了
- **Hyprland**——本机会话就是 Hyprland，也是主要开发与验证环境：截图、选区标注、窗口、长截图、pin 与滚动注入都在这里跑过。**录屏编码后端**（本机 7900 XT）的 VAAPI 与 Vulkan 两条路都实测过（零拷贝 dma-buf、h264/hevc/av1、4K 跑满 60fps、窗口中途缩放、portal、回录）；NVENC 的**选路与失败路径**已验证（无 NVIDIA 的机器上干净失败并点名原因），但**真实的 NVENC 编码从未在 NVIDIA 硬件上跑过**。
- **逐应用音频 / 麦克风**——在 Hyprland 上实测过隔离（两个 mpv 分别播 880 Hz 与 220 Hz，`--app-audio` 只录到所录窗口的那一路）与混音（`--mic --app-audio` 出**单条** AAC 音轨，两路同时检出）；niri 与 KWin 的 pid 路径只有单元测试与无头实测，未在真实音频会话上验证隔离；Sway 与 labwc 没有窗口 pid 来源，会明确拒绝。**跟随焦点（`--follow`）** 在 Hyprland 上实测过（两个不同尺寸/音调的 mpv，切焦点得到一个连续文件）；**报不出焦点的合成器未实测**。
- **niri**——平铺与浮窗两条**截图**路径已实机验证（`--no-blend` 可绕开浮窗定位）；**窗口录制/回录走合成器自己的 ScreenCast 服务**（`org.gnome.Mutter.ScreenCast`，不需要 portal、没有 picker、没有授权弹窗），4K 窗口录制、中途改尺寸、窗口关闭收尾、回录的 `status`/`save`/`stop` 都实测过；`--follow` 在这条路上不可用（服务 cast 的是启动时那一扇窗）。**注意一个 niri 侧的限流**：它的 cast 是在**输出渲染循环**里画的，所以会话不活动时渲染基本停摆（实测掉到约 1.3fps）；要让 niri 以正常帧率录制，它的 VT 必须是当前活动 VT。**KWin/Plasma** 的 D-Bus 采集、窗口列表、长截图、窗口录制与回录都已在**无头 `--virtual` KWin** 上实测（含授权被 ksycoca 重写临时拒绝时的重试规则）；**仍未验证**：`--cursor`、窗口点选时的 `--pick` 整段交互、以及滚动注入（需要 KDE 上验收）。
- **Sway / labwc / GNOME**——Sway 只有探针实现与单元测试，**没有现场验证**；**labwc 等其它提供 wlr-screencopy 的合成器**理论上基础截屏可用，但没有窗口列表与 pin 落点探针，且**没有现场验证**；**GNOME** 不支持，没有实现计划（Mutter 不提供 wlr-screencopy、KWin 那套 D-Bus 服务，也没有 layer-shell）。

### 光标（`--cursor`）
vshot 自己从不画光标，`--cursor` 只是给合成器的捕获请求置一个"叠加指针"标志（`wlr-screencopy` 的 `overlay_cursor`，KWin 是 `include-cursor`），画不画、画在哪、什么时候画，全由合成器决定；它对 `long` 无效（见下一条），KWin 上是否真的画出光标也未验证。

- **终端里敲命令后截图没有光标，不是 bug**。终端（kitty 实测，`mouse_hide_wait` 默认 3.0 秒）在鼠标不动几秒后会把指针藏掉——对合成器执行 `set_cursor(null)`，此后合成器层面**真的没有光标可画**，任何走 screencopy 的工具都一样。回车后动一下鼠标再去截，或者给 kitty 设 `mouse_hide_wait 0`。
- **niri 把指针画进的是窗口截图，不是输出帧**。niri 上 `window active` / `window pick` 走它自己的 `screenshot-window`，`--cursor` 映射为该调用的 `--show-pointer`；该参数 25.11 之后才有，旧版 niri 会拒绝整个请求，vshot 检测到后自动去掉它重试并提示。输出级路径（`monitor`、`all`、`region`）仍是 screencopy 的 `overlay_cursor`。
- **Hyprland 上开着 `hypr-dynamic-cursors` 时，指针会被烤进帧里，vshot 自己绕开**：该插件在光标被放大期间会锁住软件光标，此后合成器把指针画进它合成的帧里，而 screencopy 交出的正是这张帧——所以**不传 `--cursor` 也照样有光标**。vshot 在读整屏前会临时关掉插件并把指针 warp 回原位，读完帧立刻开回来；插件没装或本来就关着时什么都不做。

### 已知不稳定点
- **`--cursor` 在 `long` 上无效**——`src/longshot.rs` 的抓帧调用把光标参数写死为 `false`，所以 `vshot long --cursor` 会被接受但静默忽略。其余捕获路径（`region`、`monitor`、`all`、`window active`）的 `--cursor` 在本机 Hyprland 上实测有效；KWin 上 `include-cursor` 虽然传了，但**是否真的画出光标未验证**。
- **抓取正好卡在放大过程中时，帧里可能仍有一个指针**——`hypr-dynamic-cursors` 在放大期间持有软件光标锁，而关掉插件不会让它立刻释放，所以这一瞬间抓的帧会带一个**正常大小**的指针。放大结束后插件自己解锁，问题自愈。
- **像素识别（`--pixel`）**——无缝无边框平铺（无 gaps、无阴影）与完全均匀的桌面上没有任何像素信号，此时如实报错而不是猜。合成器不给焦点窗口描有色的边时，它答的是**指针下的窗口**，可能与合成器报的焦点窗口不一致。`VSHOT_PIXEL_DEBUG=1` 查每一级的判定。
- **niri 的三处细节**——半透明窗口定位：模板匹配要求窗口内容在渲染与抓帧之间不变，视频、动画会让模板过期（会带新渲染重试至多 3 次），几乎全透明或悬在输出边缘之外的窗口定位不到（退回 niri 原样的透明渲染）；会话判定改用 `NIRI_SOCKET` 文件名，判错时 `window pick` 会打开压暗 overlay 而不是 niri 的十字选窗，用 `VSHOT_SESSION_DEBUG=1` 看判成了谁；pin 落点只能问"焦点窗口所在的输出"，所以**不跟随指针，只跟随键盘焦点**。
- **KDE 窗口列表探针**——依赖 journald 收到 KWin 的 `console.info`。KWin 从 tty 起、日志只进那台 tty 时，无论等多久都取不到行；装了 `kdotool` 时优先走它，绕开这个依赖。
- **pin daemon 与 screencopy**——不要用 `pkill`/`kill -9` 结束 daemon（见[「pin daemon」](#pin-daemon)），否则部分合成器会残留 layer surface 与其截屏会话，导致所有输出的 screencopy 永久阻塞。

## 配置文件
`vshot` 把两样东西记在 `$XDG_CONFIG_HOME/vshot/config.json`（缺省 `~/.config/vshot/config.json`）：**标注编辑器的样式**，以及**部分命令行参数的默认值**；文件是可选的，没有它、读不了它、或者内容坏了，都退回内置默认值，不会影响截图。改它有两种方式：**手改文件**，或者跑 **`vshot settings`** 开一个窗口改。**截图会话中的任何改动都不写回**——会话里选的颜色、调的线宽、切的工具都是这一次的工作状态，配置是每次会话的**重置起点**。

```bash
vshot settings
```

窗口只是普通窗口，不截图、不需要任何合成器协议，所以在一个 vshot 本来截不了图的合成器上也能用；装包后也可以直接从**应用菜单**里的「VShot Settings」打开（见[「应用菜单入口」](#应用菜单入口)）。`cli` 段的数值留空/留 0 表示「不设，用内置默认」，而不是把 0 存进去，**设置窗口里这些框直接显示内置默认值**（默认值留在原处不动、保存时照样不写进文件，所以哪天内置默认改了，没动过它的人会自动跟上），`editor` 段则总是整段写出；保存是**合并写入**，本版不认识的键原样保留，不会因为存一次就被抹掉。窗口分七页，左边栏切换，**一页一个功能**：**标注编辑器**、**输出**、**滚动截图**、**文本识别**、**录制**（`record` 与 `replay` 的全部默认值）、**文件对话框**与 **Pin 浮层**；除**录制**页外，各页在默认窗口尺寸下都**不需要滚动**。保存之后窗口不关，左下角显示「已保存。」；关窗口按「取消」——那时它的意思就只剩「关闭」。

```json
{
  "editor": {
    "tool": "arrow", "color": "#ff8800ff", "width": 4, "textPixels": 28,
    "dash": "dotted", "arrowSize": 3, "arrowStyle": "filled",
    "mosaicShape": "brush", "mosaicStrength": 3, "font": "Noto Sans"
  },
  "cli": {
    "png-compression": "high",
    "monitor": "DP-2",
    "long": { "notches": 2, "max-height": 20000, "timeout": 60 },
    "pin": { "density": 2 },
    "ocr": { "engine": "builtin" }
  },
  "dialog": {
    "radius": 12, "borderWidth": 1, "borderColor": "#5a626e",
    "shadow": true, "shadowSize": 14, "shadowOffset": 3, "shadowOpacity": 120
  },
  "pin": {
    "radius": 0, "borderWidth": 2, "borderColor": "#c0c0c0",
    "shadow": true, "shadowSize": 14, "shadowOffset": 3, "shadowOpacity": 120
  }
}
```

### `editor`——编辑器样式
这个段描述的是**每次会话的起始样式**。会话中怎么改都不会写回——这里存的是重置值，不是上次的遗留。

| 键 | 取值 | 默认 |
| --- | --- | --- |
| `tool` | `select` / `rectangle` / `ellipse` / `arrow` / `pen` / `text` / `mosaic` | `select` |
| `color` | `#rrggbb` 或 `#rrggbbaa` | `#ff4040ff` |
| `width` | 1–64 | `2` |
| `textPixels` | 7–448（像素高） | `14` |
| `dash` | `solid` / `dashed` / `dotted` | `solid` |
| `arrowSize` | 1–8 | `1` |
| `arrowStyle` | `open` / `filled` | `open` |
| `mosaicShape` | `rect` / `ellipse` / `brush` | `rect` |
| `mosaicStrength` | 1–3 | `2` |
| `font` | 字体族名；空串用系统默认 | `""` |

`tool` 是**编辑状态**的起始工具，和其他样式一样**只在配置里**。区域截图总是从 Select 打开——它的第一步是拖出选区，直接进绘图工具（比如 Text）会让第一次点击变成放置文本；`window pick` 与滚动截图同理永远从 Select 开始。取值超出范围的整数会被夹到范围内，不认识的名字按默认值处理，手改文件写错了不会报错，只是那一项不生效。**`textPixels` 的单位是像素**：你填 14，字就是 14 像素高，跟编辑器里那个数字框完全一致；旧协议里那个「字符格整数倍」的刻度（1–64，一格 7 像素）只在把结果写出去时换算一次。早期版本这里叫 `textSize`，存的是那个刻度，**旧文件会被自动迁移**（`2` → 14 px、`3` → 21 px），下次保存时写成 `textPixels`、旧键删掉；两个键同时存在时以 `textPixels` 为准。

### `cli`——命令行默认值
给**没有在命令行上给出**的参数提供默认值。优先级是：

```
命令行参数 > 环境变量 > 配置文件 > 内置默认
```

| 键 | 对应参数 | 内置默认 |
| --- | --- | --- |
| `png-compression` | `--png-compression` | `fast` |
| `monitor` | `monitor [NAME]` 的输出名 | `current` |
| `long.notches` | `long --notches` | `1` |
| `long.max-height` | `long --max-height` | `30000` |
| `long.max-frames` | `long --max-frames` | `6000` |
| `long.timeout` | `long --timeout` | `120` |
| `long.ignore-top` | `long --ignore-top` | `0` |
| `long.inject` | `long --inject` | `auto` |
| `pin.density` | `pin --density` | 自动推断 |
| `record.encoder` | `record --encoder` | `h264` |
| `record.encoder-backend` | `record --encoder-backend` | `auto` |
| `record.fps` | `record --fps` | `60` |
| `record.portal` | `record --portal` | `false` |
| `record.mic` | `record --mic` | 不录 |
| `record.follow` | 不带 `--follow` 的 `record window` 跟随的窗口（数组） | 不跟随 |
| `record.notify` | 录制写盘后弹通知 | `true` |
| `replay.window` | `replay --window` | `30` |
| `replay.fps` | `replay --fps` | `30` |
| `replay.encoder` | `replay --encoder` | `h264` |
| `replay.encoder-backend` | `replay --encoder-backend` | `auto` |
| `replay.gop` | `replay --gop` | `1` |
| `replay.mic` | `replay --mic` | 不录 |
| `replay.follow` | 不带 `--follow` 的 `replay start window` 跟随的窗口（数组） | 不跟随 |
| `replay.portal` | `replay --portal` | `false` |
| `replay.save-dir` | `replay --save-dir` | 视频目录 |
| `replay.notify` | `replay save` 落盘后弹通知 | `true` |
| `ocr.engine` | `vshot ocr` 用哪个引擎 | `builtin` |
| `ocr.external.command` | `engine: "external"` 时跑的程序（数组） | 无 |
| `ocr.external.stdin` | 把 PNG 走 stdin 而不是给路径 | `false` |
| `ocr.external.timeout` | 外部程序的超时（秒） | `30` |
| `ocr.notify` | 识别结束时弹桌面通知 | `true` |

`pin.density` 的优先级同样是 `--density` > `VSHOT_PIN_DENSITY` > 配置文件；`cli` 段里不认识的键会被忽略，不会让整个文件失效。`ocr.engine` 只认 `builtin` 与 `external` 两个值，写了别的名字会**报错**而不是当默认值处理，因为把 `external` 拼错会让人以为自己配的 GPU 引擎生效了（`engine: "external"` 而没有 `command`、或命令跑不起来，同样明确报错，详见[「用 GPU：外接引擎」](#用-gpu外接引擎)）。设置窗口覆盖 `editor`、常用的 `cli` 项、`ocr.notify` 这个开关，以及 `record` 与 `replay` 两段；`ocr.engine`、`ocr.external` 要手改文件。**两个 `notify` 开关只写「关」**，因为键不存在就是「开」；麦克风那两项是**从当前会话检测出来的**；`follow` 那两项在窗口里是**一行逗号分隔的窗口名**，在文件里是一个数组——手改时写成 `["game", "chat"]`。

`record.follow` / `replay.follow` 只在**不带窗口名、也不给 `--pick`** 的 `record window` / `replay start window` 上生效；其它目标（`monitor`、`all`、`region`，或命令行上点了名的窗口）会**忽略**记着的跟随列表，而不是因为它在而报错。`--no-follow` 是对那一次录制/回录把记着的列表关掉，正如 `--no-mic` 对记着的麦克风那样。`color` 用的是 CSS 那套写法：`#rrggbb`，带透明度时写 `#rrggbbaa`（alpha 在**最后**）——注意这跟 Qt 自己的八位写法 `#aarrggbb` 不同，`vshot settings` 与配置文件都按 CSS 那套来。

### `dialog`——文件对话框外观
保存与打开窗口（pin 的「另存为…」、编辑器里贴本地图片）是 **layer surface**，合成器不给它们画任何装饰，所以外框和阴影是它们与背后内容之间唯一的东西。

| 键 | 取值 | 默认 |
| --- | --- | --- |
| `radius` | 0–48，逻辑像素 | `12` |
| `borderWidth` | 0–8，逻辑像素；0 完全不画 | `1` |
| `borderColor` | `#rrggbb`；**不写**表示按配色方案推导 | 无 |
| `shadow` | `true` / `false` | `true` |
| `shadowSize` | 0–64，逻辑像素 | `14` |
| `shadowOffset` | -32–32，逻辑像素 | `3` |
| `shadowOpacity` | 0–255 | `120` |

`borderColor` 不写（或删掉）时会按对话框自己的配色推导出一道比底色略深的线，所以在亮/暗主题下都读得出是条边，写了就用你写的。阴影的四个键与 `pin` 段**是同一套**（含义、范围和默认值都相同，见下一节）。对话框是 layer surface，只能有它自己申请的那个大小，所以阴影画在窗口内部留出的一圈里，`shadowSize` 因此也决定了对话框本体比窗口小多少——它是「一圈」的宽度，不是叠加在外面的。

### `pin`——pin 浮层外观
pin 同样是 layer surface，里面只有图片，所以圆角、身下的阴影、外面那道线都得 vshot 自己画。这一段和 `cli.pin` 是**两回事**：那个是 pin 的**尺寸**（来源密度，属于命令行默认值），这个是 pin 的**样子**。

| 键 | 取值 | 默认 |
| --- | --- | --- |
| `radius` | 0–512，逻辑像素；0 是直角 | `0` |
| `shadow` | `true` / `false` | `true` |
| `shadowSize` | 0–64，逻辑像素；0 等于不模糊 | `14` |
| `shadowOffset` | -32–32，逻辑像素；负值把阴影抬到上方 | `3` |
| `shadowOpacity` | 0–255 | `120` |
| `borderWidth` | 0–8，逻辑像素；0 完全不画 | `2` |
| `borderColor` | 未激活时的边框色；不写用内置浅灰 `#c0c0c0` | 无 |
| `activeBorderColor` | 激活时（鼠标停在那张 pin 上、且该屏持有键盘）的边框色；不写用内置黑色 | 无 |

默认**不圆角但有阴影**：截图是一张窗口的像，圆角会切掉它自己的内容；而一张截图钉在一模一样颜色的窗口上时，没有阴影就完全看不出边界在哪。阴影的三个数值键：`shadowSize` 是模糊向外扩多远（也就是阴影视觉上的「软硬」），`shadowOffset` 是把整个阴影往下压多少——光从上面来，所以默认 3，填负数它就跑到 pin 上方去；`shadowOpacity` 是阴影的浓度，模糊只是把它摊开、不会加深；`shadow` 是总开关，**关掉不会丢失数值**，所以可以临时关一下再打开，尺寸还在。`radius` 是**上限而不是承诺**：真正画的时候会夹到图片短边的一半——再大就不是圆角而是胶囊了，而 pin 的尺寸随滚轮每滚一格都在变，所以这件事只能画的时候算。边框**居中在图像边缘**上，一半在外一半在内，宽度改变时圆角 pin 和直角 pin 的外沿位置一致。阴影是一次性算好缓存的，拖动时不会重算，缩放或改配置会；**改完配置要生效，下次 `vshot pin` 会重新读**——daemon 在还有 pin 的时候不会退出，但每加一张 pin 都会重读一次文件，所以不用手动重启 daemon。

## 环境变量

| 变量 | 作用 |
| --- | --- |
| `VSHOT_LANG` | 界面与 `--help` 语言：以 `zh` 开头选中文，其它非空值选英文，缺省跟随系统 |
| `VSHOT_QT_HELPER` | 指定 `vshot-qt-ui` 路径 |
| `VSHOT_PIXEL_DEBUG=1` | 窗口像素识别每一级看到了什么 |
| `VSHOT_SESSION_DEBUG=1` | 本次会话被判成了哪个合成器、依据是什么 |
| `VSHOT_LONG_DEBUG_DIR=<dir>` | 长截图落盘每一帧与每次拼接决定 |
| `VSHOT_NVENC_DEVICE=N` | 指定 NVENC 用第 N 个 CUDA 设备（多显卡机器；默认第一个） |
| `VSHOT_VULKAN_DEVICE=N` | 指定 Vulkan 编码用第 N 个物理设备（多显卡机器；默认第一个） |
| `VSHOT_OCR_MODELS=<dir>` | OCR 模型目录，覆盖 `/usr/share/vshot/models` 与可执行文件旁的查找 |
| `VSHOT_PIN_SOCKET` | pin daemon 监听的 socket 路径 |
| `VSHOT_PIN_DENSITY=N` | 每张 pin 图的来源密度，等同 `--density` |
| `VSHOT_PIN_DEBUG=1` | daemon 打印每张 pin 的密度判定 |
| `VSHOT_PIN_FOCUS_DEBUG=1` | daemon 打印 pin 渲染面每一次焦点变化 |
| `VSHOT_PIN_SOURCE_FILE` | 覆盖截图工具记录的路径（默认 `/tmp/screenshot-path`） |
| `VSHOT_RECORD_PIDFILE` | `vshot record stop` 读取的 pid 文件路径（默认 `$XDG_RUNTIME_DIR/vshot-record-<uid>.pid`） |
| `VSHOT_RECORD_DEBUG=1` | 录制/回录循环打印每帧的阶段（抓取/编码/入封装）与所用 libavcodec 版本 |
| `VSHOT_PORTAL_SHM=1` | `record --portal` 改为要内存帧而不是 dma-buf（合成器的缓冲导入不了时的备用路线） |
| `VSHOT_REPLAY_SOCKET` | 回录控制 socket 的路径（默认 `$XDG_RUNTIME_DIR/vshot-replay-<uid>.sock`） |
| `VSHOT_REPLAY_PIDFILE` | `vshot replay stop` 读取的 pid 文件路径（默认 `$XDG_RUNTIME_DIR/vshot-replay-<uid>.pid`） |

> 只要给 daemon 开了任一 `VSHOT_PIN_*_DEBUG`，它就不再把自己的 stderr 丢给 `/dev/null`，踪迹因此可读。变量必须在 daemon 启动时就位；已经在常驻的那个要先 `vshot pin --quit`。

## 已知限制
- **KWin 的冻结 overlay 仍依赖 `zwlr_layer_shell_v1`**（KWin 提供它）。授权不是对话框，只认 desktop file，所以从 `target/` 直接跑的构建在 Plasma 下会一直拿到 `NoAuthorized`；
- **同一个 `XDG_RUNTIME_DIR` 下可以同时存在多个合成器**，而 `WAYLAND_DISPLAY` 未设置时 libwayland 会用默认的 `wayland-0`，于是命令可能连到"另一个合成器"上。凡跟合成器有关的失败，vshot 都会额外打印这次连的是哪个 display、本机还有哪些 display；
- **长截图**：选区必须完全落在一块屏幕内；滚动到某处才出现的固定条（浮出的工具条）仍会被当成页面内容，用 `--ignore-top N` 排除；懒加载页面可能重复或缺失少量行；KDE 上帧率明显低于别的合成器；抓帧与对齐的耗时随区域面积线性增长，**长截图请用 release 构建**；
- **像素识别**：无缝无边框平铺与完全均匀的桌面没有像素信号，此时如实报错而不是猜；合成器不给焦点窗口描色的边时，`window active --pixel` 答的是"指针下的窗口"；
- **文本**：Qt 文本框接受任意 Unicode（含输入法提交的 CJK）；未携带位图的旧 helper 结果回退到 Rust 内置 5x7 字体，该回退路径仅支持可打印 ASCII；
- **`monitor current`** 依赖 overlay 上收到 pointer enter/motion；通用 Wayland 没有可读取的全局鼠标坐标，因此不会用第一个 output 猜测；
- **回录**：`replay start --portal` 尚不支持（portal 的出帧循环还没接到内存环，会明确拒绝）；一次只跑一个会话；
- 原生 screencopy 等待合成器返回帧最多 10 秒，超时返回错误而不是永久阻塞。

## 验证
```sh
cargo fmt --check
cargo test --locked
cargo clippy --locked --all-targets --all-features -- -D warnings
cargo build --release --locked
```

Qt helper 侧没有测试框架，只有**不需要合成器的离屏检查**（默认不构建，加 `-DVSHOT_BUILD_CHECKS=ON`），覆盖配置读写与设置窗口、字号换算、剪贴板颜色解析与色卡渲染、pin 的图片自述密度与描边、文字卡片留白、色卡右键菜单、贴图的导出格式：

```sh
cmake -S . -B build-qt -DVSHOT_BUILD_CHECKS=ON && cmake --build build-qt
build-qt/vshot-config-check
QT_QPA_PLATFORM=offscreen build-qt/vshot-settings-check
build-qt/vshot-text-size-check
QT_QPA_PLATFORM=offscreen build-qt/vshot-color-check
build-qt/vshot-pin-density-check
QT_QPA_PLATFORM=offscreen build-qt/vshot-pin-outline-check
QT_QPA_PLATFORM=offscreen build-qt/vshot-text-card-check
QT_QPA_PLATFORM=offscreen build-qt/vshot-pin-menu-check
QT_QPA_PLATFORM=offscreen build-qt/vshot-paste-check
```

另有 5 个默认**不执行**（`#[ignore]`）的集成测试，需要真实环境：KWin 的 D-Bus 采集与后端选择（见 `src/capture/kwin.rs` 的注释，起无头 KWin 即可：虚拟输出名 `Virtual-0`、1024x768、无 pointer capability，只覆盖到 D-Bus 采集这一层）、活跃输出探针（需要任一真实会话）、`/dev/uinput` 滚动注入（需要写权限）、以及**内置 OCR 引擎读一张画出来的文字**（需要那 30 MB 模型在盘上，`cargo test` 没地方去下）。跑法：

```sh
cargo test -- --ignored              # 全部
cargo test --release ocr:: -- --ignored --nocapture   # 只跑 OCR
```

OCR 那一项默认在源码树的 `models/` 里找模型，也可以用 `VSHOT_OCR_MODELS=<dir>` 指到别处。无头 KWin 的启动方式：

```sh
mkdir -p /tmp/kwin-e2e/{cfg,data,cache}
KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1 XDG_CONFIG_HOME=/tmp/kwin-e2e/cfg \
  XDG_DATA_HOME=/tmp/kwin-e2e/data XDG_CACHE_HOME=/tmp/kwin-e2e/cache \
  kwin_wayland --virtual --socket wayland-ke2e --no-lockscreen \
  --no-global-shortcuts --no-kactivities &
XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=wayland-ke2e \
  cargo test -- --ignored --nocapture
```

`VSHOT_KWIN_E2E_OUTPUT` / `_WIDTH` / `_HEIGHT` / `_COLOR=R,G,B` 可覆盖默认的输出名、尺寸与中心像素颜色断言。

## 许可证
**GPL-3.0-or-later**（见 `LICENSE`）：vshot 按 GNU 通用公共许可证第 3 版或任何更新版本发布，再分发（含修改版）须以同一许可提供完整对应源码。此前的版本（含 v0.1.0–v0.1.2）按 MIT 发布；改用 GPL 是为了让整条依赖链没有灰区：

- **Qt 6 / LayerShellQt**——动态链接，按这两者提供的 GPL 选项使用（Qt 另有 LGPL-3.0 选项，LayerShellQt 另有 LGPL-2.0-or-later 选项）。Qt 库可自行替换：`vshot` 主程序根本不链接 Qt，界面在独立的 `vshot-qt-ui` 里。
- **FFmpeg**（录屏，可选依赖）——Arch 的构建是 GPL-3.0，运行时以 dlopen 加载。MIT 时代「dlopen 是否构成结合」是个灰区；vshot 自身改为 GPL 后，无论怎么认定都兼容。
- **Rust 依赖**——MIT / Apache-2.0 / BSD 等宽松许可，均与 GPL 兼容。
- **OCR 模型**（`/usr/share/vshot/models`）——来自 RapidOCR / PaddleOCR，Apache-2.0；字典来自 oar-ocr，Apache-2.0。

`LICENSE` 是 GPLv3 全文，`NOTICE` 是第三方组件声明，两者都随包安装到 `/usr/share/licenses/vshot/`。对使用者：使用、修改、再分发照旧自由；修改版再分发须一并提供源码，且不能把 vshot 的代码并进闭源产品。
