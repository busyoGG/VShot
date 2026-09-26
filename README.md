# vshot

**中文** | [English](README.en.md)

> **本项目是纯 vibe coding 项目**：需求由人提，代码与文档由 AI 写。

Rust 写的 Wayland 截图工具，带 Qt 交互界面与常驻 pin 浮层。捕获采用**严格冻结**：先把桌面拍成静态帧，之后所有选择与标注都在那一帧上做，选区期间屏幕上没有任何东西会动。

已适配 Hyprland、niri、KWin/Plasma、Sway，以及任何提供 `wlr-screencopy` 的合成器（如 labwc）的基础截屏。

> **验证程度不一**：Hyprland 全部功能现场实测；niri 的平铺与浮窗截图路径、以及**窗口录制与回录**均已实机验证；KWin/Plasma 的 D-Bus 采集、窗口列表与长截图实测过，但 `--cursor`、滚动注入未验证；Sway 与 labwc **没有现场验证**。详见[「合成器适配与验证状态」](#合成器适配与验证状态)。

## 功能

- **区域截图**——冻结桌面后拖选，或直接给固定 geometry；8 向手柄调整，放大镜与尺寸读数跟随指针
- **标注编辑**——矩形、椭圆、箭头、涂鸦、文本、马赛克；撤销/重做、选中移动与缩放、颜色与线型、箭头头型、字号、系统字体选择
- **monitor / all**——按名字或指针位置截一块输出，或把整个桌面按逻辑位置拼合
- **window active / pick**——焦点窗口，或实时桌面上高亮点选；KWin 与 niri 直接交出窗口自己的像素
- **长截图**——框选会滚动的内容，自动发滚轮、逐帧抓取、按内容对齐拼成一张长图
- **录屏**——把屏幕、整个桌面、一块矩形或一扇窗自己的像素录成 MP4，GPU 硬编（VAAPI 或 NVENC 可选），音轨可选麦克风或**所录窗口自己的声音**
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

脚本把当前工作树（含未提交改动）快照到临时目录再调 `makepkg`，产物写到 `dist/`；也可以直接 `makepkg -si`。运行时依赖 `glibc`、`wayland`（通过 dlopen 使用 `libwayland-client`）、`qt6-base`、`layer-shell-qt`，以及 **`onnxruntime`**（OCR 的推理引擎）；文件输出、`--clipboard` 与 `vshot pin --clipboard` 需要可选依赖 `wl-clipboard`（写用 `wl-copy`，读用 `wl-paste`）。包同时装入约 30 MB 的 OCR 模型（`/usr/share/vshot/models/`，由 `makepkg` 下载并校验 SHA-256）。其它发行版按下面的源码方式构建。

那 30 MB 只在**第一次**构建时下载。模型缓存在 `$XDG_CACHE_HOME/vshot/makepkg-sources`（通常是 `~/.cache/vshot/makepkg-sources`），此后每次构建都从那里取，联网与否都不影响。之所以要专门给一个固定的缓存目录：`makepkg` 默认的源码缓存就在它构建的目录里，而这个脚本每次都在一个全新的 `mktemp -d` 里构建，于是默认缓存随上一次的临时目录一起消失——每次都得重下。设了 `SRCDEST` 环境变量就用你的，没有就用上面那个。想重新下载删掉该目录即可（下一次构建会重新取）。若本地 `models/` 已经有了这三个文件，也可以直接拷进缓存来省掉首次下载，文件校验和对得上：

```sh
mkdir -p ~/.cache/vshot/makepkg-sources && cp models/* ~/.cache/vshot/makepkg-sources/
```

`onnxruntime` 在 Arch 上是**虚拟包**：`onnxruntime-cpu`、`onnxruntime-cuda`、`onnxruntime-rocm` 等六个变体都声明 `Provides: onnxruntime` 且互相冲突，所以系统里只会有一个。依赖写成虚拟包名而不是 `onnxruntime-cpu`，是为了让**已经装了某个 GPU 变体的人不必为 vshot 卸掉它**——卸它会连带带走 `rccl`、`migraphx`、`rocm-hip-sdk` 那一串。vshot 只用其中的共享库和 `.pc` 文件（每个变体都有），也没有任何地方去选 GPU provider，所以 GPU 变体跑 OCR 时一样走 CPU。全新安装时 pacman 会让你挑一个，推荐 `onnxruntime-cpu`（连 cpuinfo、protobuf 一起约 46 MB，而 GPU 变体动辄上 GB）。

## 应用菜单入口

包会装一个 `vshot-settings.desktop`，在应用菜单里显示为 **VShot Settings**，点开就是 `vshot settings` 的图形设置界面。图标是 `icons/vshot.svg`，装在 `hicolor/scalable/apps/` 下——一个可缩放 SVG 而不是一整套尺寸，桌面外壳（KDE、GNOME、wlroots 系的启动器）自己渲染并按需取尺寸。

Wayland **没有逐窗口的图标**：合成器拿客户端声明的 application id，去找 `<id>.desktop`，再画它 `Icon=` 指的东西。所以 `ui/main.cpp` 里声明的那个名字必须与安装的 desktop 文件名一致，改一处就得改另一处，否则窗口只会拿到一个通用占位图标。

但这个 id 不只是图标查找用的：**Qt 还会把它注册给 host portal**，portal 在同样的目录里找同名 desktop file，找不到就往 stderr 打一句 `Failed to register with host portal ... App info not found for 'vshot-settings'`。从源码树或 `build-qt/` 直接跑的副本没有装 desktop file，所以会一直看到这句话——那正是它该说的：这份副本没有桌面条目。因此 app id **只在 desktop file 确实装好时才声明**（用 `QStandardPaths::locate` 查 `ApplicationsLocation`），没装就不声明，窗口退回通用占位图标，也不再触发 portal 那条报错。

`vshot-settings-check` 里有一节盯这条链：app id 与文件名是否一致、`Icon=` 指向的文件在不在源码树里、PKGBUILD 是否都装、以及**那句声明有没有那个「文件存在才声明」的防护**——最后一条是我加了防护后又回头补上的，因为原来的检查只看字面量，去掉防护照样绿。

给 KWin 授权用的那份 `vshot.desktop` 是**另一个文件**，不能合并进来：它的 `Exec=` 必须精确指向 `/usr/bin/vshot`（KWin 按 pid 取 `/proc/<pid>/exe` 比对 `Exec=` 第一个词），而且是 `NoDisplay=true`，因为不带子命令直接跑 `vshot` 只会报「没给输出目标」。

## 构建

```sh
cargo build --release --locked                                # Rust CLI
cmake -S . -B build-qt -DCMAKE_BUILD_TYPE=Release             # Qt helper
cmake --build build-qt --parallel
```

OCR 要 **`onnxruntime` 的开发文件**：`ort-sys` 通过 `pkg-config` 找 `libonnxruntime.pc`（Arch 上六个变体包都提供它——库、头文件与 `.pc` 在一起），链接的是系统那一份而不是构建时另下一份。缺了它构建会失败并说明原因。模型放在源码树的 `models/`（`det.onnx` / `rec.onnx` / `dict.txt`，见[「OCR 取字」](#ocr-取字)）；不装它们也能构建，只是 `vshot ocr` 会在运行时说找不到。

交互功能会按顺序找 helper：`VSHOT_QT_HELPER` 环境变量、`vshot` 可执行文件同目录、其相对的 `../build-qt/` 与 `../../build-qt/`、最后 `PATH`。也可以显式指定：

```sh
VSHOT_QT_HELPER="$PWD/build-qt/vshot-qt-ui" target/release/vshot region --output shot.png
```

运行时需要 Wayland 会话、`wl_compositor`、`wl_shm`、至少一个 `wl_output`、`zxdg_output_manager_v1`、`zwlr_layer_shell_v1`。**seat 只在真正读它时才要求**：`monitor <名字>`、`all`、`window active`、`region --geometry` 不碰 seat；`monitor current` 要 `seat pointer`，交互式选区要指针与键盘，缺哪个就以非零状态退出并指名。

## 用法

```sh
# 区域截图
vshot region --output shot.png              # 冻结后拖选
vshot region --clipboard
vshot region --geometry '100,200 800x600' --output shot.png

# 输出
vshot monitor eDP-1 --output shot.png       # 按名字
vshot monitor current --clipboard           # 指针所在的那块
vshot all --output desktop.png              # 整个桌面（输出间的空隙保持透明）

# 窗口
vshot window active --output window.png     # 焦点窗口
vshot window active --pixel                 # 跳过合成器元数据，用像素识别
vshot window pick --output window.png       # 实时桌面上点选
vshot window pick --pixel

# 长截图
vshot long --output long.png
vshot long --geometry '100,200 900x700' --output long.png
vshot long --ignore-top 48 --clipboard

# 输出到 stdout（日志只写 stderr）
vshot all --output - > desktop.png

# 截图直接 pin 到屏幕，不落盘
vshot region --pin

# pin 管理
vshot pin shot.png another.png
vshot pin --clipboard                       # pin 剪贴板内容
vshot pin --density 2 shot.png              # 手动指定来源屏倍率
vshot pin --toggle                          # 一键显隐
vshot pin --show
vshot pin --hide
vshot pin --close-all
vshot pin --list
vshot pin --quit

# 设置：开窗口改编辑器样式与命令行默认值（写进 config.json）
vshot settings

# OCR：把屏幕上某块区域的文字读出来
vshot ocr                                   # 框选，文字到 stdout
vshot ocr --clipboard                       # 同上，进剪贴板
vshot ocr --input shot.png                  # 读一个已有的图片文件

# 录屏：把屏幕录成 MP4（H.264，GPU 编码）
vshot record monitor eDP-1 --output clip.mp4    # 录一块屏
vshot record monitor --fps 30                   # 你当前所在的那块（NAME 省略即 current），30fps
vshot record all                                # 整个桌面，默认存视频目录
vshot record region --geometry '0,0 800x600'    # 一块矩形，桌面坐标
vshot record window                             # 焦点窗口自己的像素，盖住也录得完整
vshot record stop                               # 停止正在进行的录制
vshot record monitor current --duration 30      # 录 30 秒自动停

# 回录：把最近一段留在内存里，需要时随时落盘
vshot replay start monitor --background         # 后台挂着，默认留最近 30 秒
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

每次捕获必须且只能给一个输出目标。`region --geometry` 与 `--interactive` 互斥，不给 geometry 时默认交互选择（`--interactive` 用于显式声明这一意图）。

路径格式示例：

```sh
vshot region --output "$HOME/Pictures/vshot-%Y-%m-%d_%H-%M-%S.png"
vshot all --output 'shots/capture-%Y%m%d-%H%M%S.final.png'
```

`%Y` `%m` `%d` `%H` `%M` `%S` 分别为年、月、日、时、分、秒，`%%` 是字面 `%`。前缀后缀可任意组合。文件输出与剪贴板输出都需要 `wl-copy`。

## 区域截图与标注编辑

`vshot region` 不给 `--geometry` 时，冻结帧铺满每块输出，选区之外盖半透明暗色遮罩：

- 拖拽画矩形，四周 8 个手柄调整大小，拖选区内部移动位置，方向键微调（Shift 加速为 10 逻辑像素）
- 拖拽或调整时，光标旁显示 8x 放大镜与原生像素坐标，选区左上角显示 `宽 × 高`
- **Enter**、选区内双击或工具栏 OK 确认；**Esc** 或右键取消整次截图（文本框内的 Esc 只关闭文本框）
- 工具栏是磨砂浮动面板（背景为冻结画面的实时模糊采样），第一行为工具 Select、Rect、Ellipse、Arrow、Draw、Text、Mosaic 与 Undo、Redo、OK、Cancel；样式子面板按当前工具显隐，始终弹在命令栏背向选区的一侧，可以拖动固定位置，跟随选区跨屏移动
- 样式项：颜色色板（含自定义取色器：HSV 渐变 + 十六进制输入）、线型 Solid/Dash/Dot、箭头头型 Open V/Filled、粗细 1-64、箭头大小 1-8、字号 7-448（直接就是像素高，和用户输入一致，内部换算成旧协议的整数刻度只在写结果时做一次）、马赛克形状 Rect/Ellip/Brush、马赛克程度 1-3、系统字体列表（每项按自身字形预览）
- Arrow 是按下点到释放点的直线箭头；Draw 是自由绘制；Mosaic 的马赛克程度控制像素块大小与涂抹半径，选中已有马赛克后可直接改程度
- **Select** 工具可点选任意标注：单击选中，拖动移动（文本同样），形状/线条/马赛克可拖把手缩放，Delete/Backspace 删除；样式修改即时应用到选中标注；双击文本重新编辑
- **Ctrl+Z / Ctrl+Y**（或 Ctrl+Shift+Z）撤销/重做
- **贴图**：工具栏的「图片」按钮从磁盘挑一张，或 **Ctrl+V** 直接把剪贴板里的图贴进来——原尺寸落在选区正中，比选区大时等比缩小塞进去，贴完自动切到 Select 并选中它，接着就能拖动、用把手缩放，Ctrl+Z 一样能撤销
- **取字**：工具栏的「取字」按钮把选区里的文字读出来放进剪贴板，按钮自己会闪一下「已复制」或「失败」，识别结束还会弹一条桌面通知（见[「OCR 取字」](#ocr-取字)，`cli.ocr.notify` 可关）；读取本身跑在 `vshot ocr --input` 子进程里
- 标注以全局逻辑坐标传回 Rust，最终 PNG 由内置软件渲染重绘，与预览一致；文本由 Qt 按所选字体栅格化为位图后合成，因此字形完全一致

界面语言默认跟随系统（`QLocale::system()`），可用 `VSHOT_LANG` 覆盖：以 `zh` 开头选中文，其它非空值选英文。语言在 helper 启动时确定，切换需重新运行。Rust CLI 的 `--help` 走同一套判定，`VSHOT_LANG=zh vshot --help` 即中文。

## OCR 取字

`vshot ocr` 把屏幕上某块区域的文字读出来。不给参数就是框选：

```sh
vshot ocr                    # 框一块区域，文字到 stdout
vshot ocr --clipboard        # 同上，进剪贴板
vshot ocr --geometry '0,0 800x200'
vshot ocr --input shot.png   # 读一个已有的图片文件
```

识别完默认会弹一条**桌面通知**：成功给出识别到的文字（长了截断到 160 字），失败给出原因——`vshot ocr` 被快捷键拉起时，没有别的东西告诉你它跑完了。通知交给会话总线上 `org.freedesktop.Notifications` 的服务去画，**没有通知服务也照样能用**，只是少这条提示（这时 stderr 会说一句，文字本身照给）。开关在设置窗口的「文本识别」页，也可以手改：

```json
{"cli": {"ocr": {"notify": false}}}
```

剪贴板里放的是识别出的文字本身，**不会多出一个结尾换行**；只有输出到 stdout 时才补一个，免得终端的提示符挤在最后一行文字上。

识别用 **PaddleOCR 的 PP-OCR 模型**（官方模型转成的 ONNX 版本），跑在本进程的 ONNX Runtime 上，**用 CPU**。实测一张 720p 代码截图端到端约 **240 ms**，13px 的小字也认得准。

模型是 `PP-OCRv6_small` 这一档，约 30 MB：

| 文件 | 大小 | 作用 |
|---|---|---|
| `det.onnx` | 9.4 MB | 文本检测（语种无关） |
| `rec.onnx` | 20.3 MB | 文本识别 |
| `dict.txt` | 73 KB | 18708 字的字符表 |

装包后它们在 `/usr/share/vshot/models/`；源码树里放在 `models/`（`vshot ocr` 会从可执行文件逐级往上找，所以 `target/release/vshot` 和 `target/release/deps/vshot-*` 都找得到）。**这三个文件不进 git**，PKGBUILD 用 `source=()` 下载并校验 SHA-256。

### 为什么默认是 CPU

因为它够快，而 GPU 的代价不成比例：

- 实测识别模型 **CPU 4.0 ms vs GPU 2.2 ms**——在 240 ms 的总耗时里看不出来
- 而 Arch 上 `onnxruntime-opt-rocm` 的依赖链是 `rocm-hip-sdk` + `rccl`（446 MB）+ `migraphx`（787 MB），**光这两个就 1.2 GB**，还没算 rocm-hip-sdk 展开的二十几个包。把一个截图工具的依赖撑到 2 GB 换 1.8 ms，不划算。

所以 `depends` 里写的是虚拟包 `onnxruntime`，而不是钉死 `onnxruntime-cpu`（连 cpuinfo、protobuf 一起约 46 MB）——理由见[「安装」](#安装arch-linux)：钉死会逼已经装了 GPU 变体的人卸掉它，连带带走一整串 ROCm 包，而 vshot 从那个变体里用到的只是共享库和 `.pc`。

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

- `command`：程序与参数，**数组**形式（不走 shell，所以参数里有空格也不用转义）
- `stdin`：`true` 就把 PNG 走 stdin 送过去；不给或 `false` 则把临时 PNG 的**路径**作为最后一个参数附上
- `timeout`：秒，默认 30；超时会杀掉子进程，不会挂住截图

这个程序可以是什么都行——一个用 ROCm wheel 的 Python `rapidocr`、一个 `curl` 到另一台机器上的服务、一份带 CUDA 的 ONNX Runtime。举一个用 Python rapidocr 的例子：

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

**配置错了会直接报错，不会静默退回 CPU**：写了 `engine: "external"` 却没给 `command`、或者命令跑不起来、或者程序非零退出，都是明确的错误信息（外部程序写到 stderr 的内容会一并带上）。一个配了 GPU 引擎的人想知道它没跑起来，而不是拿到一份自己没要的 CPU 结果。

## 截取窗口

### window active

按以下顺序取得焦点窗口：

1. **合成器自己画这扇窗**（唯一直接给出答案、不经过裁剪的一路）：
   - **KWin**：`CaptureActiveWindow`，带装饰与阴影（阴影外圈透明），回复里的 `scale` 就是密度；
   - **niri**：`niri msg action screenshot-window`，niri 把窗口渲染成 PNG 写到临时路径再读回。niri 的 IPC 报不出平铺窗口的绝对位置，所以它在 niri 上是唯一可行的路线。渲染不含边框（niri 把边框画在 tile 上），vshot 用 `tile_size` / `window_size` / `window_offset_in_tile` 换算后补回。半透明窗口会对该输出再抓一帧、在帧上定位这张渲染并裁帧输出，半透明处透出真实背景；定位失败会先复核稳定性（内容未变 → 退回 niri 原样的透明渲染；内容在变 → 带新渲染重试，至多 3 次）。`--no-blend` 跳过整段定位，直接交出 niri 的渲染：绝不错位，但半透明处是空的、边框也不在内；
2. Hyprland：`hyprctl activewindow -j`；
3. Sway：`swaymsg -t get_tree` 中 focused node 的 `rect`；
4. KDE Plasma 兜底：`kdotool`（装了就用）→ 一次性 KWin scripting 探针（读 `workspace.activeWindow.frameGeometry`，经 journal 取回）；
5. **像素识别**：以上都不可用时，在捕获的帧上自动检测窗口。

`--pixel` 跳过 1-4，直接在帧上做像素识别（用于测试检测器，也是只有矩形时的唯一选择）。识别**逐输出**用该输出自己的原生像素进行，候选按可信度分级（边框带 → 泛洪分割 → 闭合描边轮廓 → 整块输出），并且**把合成器画的那条边框包含在内**。合成器不给焦点窗口描有色的边时，画面里没有任何东西能区分"焦点窗口"与"指针下的窗口"，此时候选按「指针所在输出 → 指针命中 → 面积」排序，给出的是你指着的那个窗口。无缝无边框平铺（无 gaps、无阴影）没有任何像素信号，此时如实报错。`VSHOT_PIXEL_DEBUG=1` 打印每一级的判定。

只问**当前会话自己的合成器**（按 `XDG_CURRENT_DESKTOP` / `XDG_SESSION_DESKTOP` 判定；两者都没写合成器名时才把所有探针都试一遍）。niri 不看这两个变量（它对 niri 不可靠，从 TTY 手动起会话时是空的），而是用 `NIRI_SOCKET` 的文件名 `niri.$WAYLAND_DISPLAY.$PID.sock` 认出来。同时跑着两个合成器时这条判定是必需的，否则会截到另一个会话里的窗口。`VSHOT_SESSION_DEBUG=1` 打印判定结果与依据。

### window pick

先在**实时桌面**上挑窗口：移动指针高亮指针下的窗口，其余压暗，左上角提示条给出窗口标题与尺寸；左键点击结束挑选，Esc 或右键取消。随后**重新捕获一帧**，把点击位置在当前的窗口列表上重新解析成窗口矩形，并以那一帧开一个与 `region` 相同的编辑会话。因此挑选期间切换工作区、移动窗口都不会让结果停在旧画面上。

候选来自合成器的窗口列表（Hyprland `hyprctl clients`、Sway `swaymsg -t get_tree`、KDE KWin scripting 探针），按各自的叠放顺序自底向顶排列，指针命中的取**列表里最后一个包含指针的窗口**（即最上层那个）。候选跟着指针刷新，指针不动时每 300 ms 也刷一次。列表为空或查询失败时自动落到像素识别。

`--pixel` 跳过窗口列表，直接在冻结帧上做像素识别并把所有候选交给用户挑。

**niri 是例外**：它没有窗口列表可用，改用 niri 自己的十字选窗，像素由 niri 自己渲染——因此不画压暗 overlay，也没有标注编辑器。`--pixel` 时才回到 overlay + 像素识别。

## 长截图

`vshot long` 把一块会滚动的内容拼成一张长图。不给 `--geometry` 时先框选（与区域截图同一套选区交互，但确定后**不进入**标注编辑器）。之后：

1. 屏幕角落出现提示条，报告已拼接高度与帧数，接收 **Enter / Space / 左键点击**（保留结果）和 **Esc / 右键**（丢弃）；
2. vshot 按固定节拍发滚轮（默认每 120 ms 一批，`--notches` 决定每批发几档），同时以约 50 fps 的上限连续抓帧，每帧与上一帧对齐，位移大于 0 就把新增行接到长图底部；
3. 结束时按既有输出路径写出（`--output` / `--clipboard` / `--pin`）。结果不经过标注编辑器，想标注就先 pin 再用 pin 的编辑功能。

**停止条件**：连续 6 次滚轮都没有产生位移（页面到底）、`--max-height`、`--max-frames`、`--timeout`，或用户按键。一帧的位移超出可测范围时会把滚轮对半缩小（下限 1 档）；连续多次仍对不上才停止并报告原因。

对齐只取帧里**属于页面**的行：顶部与底部连续不动的行（标题栏、固定工具栏、状态栏）是 chrome，不进探针，也不会每帧重复——顶部 chrome 只出现在长图最上面，底部只出现在最下面。新增行从页面底边往上数，而这条边跨帧保留、只降不升。每帧只与上一帧比较，误差不累积。

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

`vshot record` 把屏幕录成 MP4：帧来自截图用的同一套捕获后端（wlroots 会话走
`zwlr-screencopy`，Plasma 走 KWin 的 ScreenShot2），编码在 GPU 的媒体引擎上完成。

```sh
vshot record monitor eDP-1 --output clip.mp4    # 一块屏；NAME 同 `vshot monitor`
vshot record monitor                            # NAME 省略即 `current`：你当前所在的那块
vshot record all                                # 多屏按逻辑位置拼合
vshot record region --geometry '0,0 800x600'    # 一块矩形（桌面坐标，同 `vshot region --geometry`）
vshot record region                             # 在冻结桌面上拖出矩形
vshot record window                             # 焦点窗口自己的像素（不是它所在的屏幕区域）
vshot record window firefox                     # 按 app id 或标题选窗口
vshot record window --pick                      # 点选一扇窗来录
vshot record monitor --encoder hevc             # 编码器：h264（默认）/ hevc / av1
vshot record monitor --fps 120                  # 瞄准 120fps（1-240，默认 60）
vshot record monitor --duration 60              # 60 秒后自动停
vshot record monitor --portal                   # 走桌面 portal：由合成器弹出选择器
vshot record window --portal                    # 同上，但选择器列的是窗口
vshot record monitor --mic                      # 同时录麦克风（会话的默认输入设备）
vshot record monitor --mic alsa_input.pci-0000_2f_00.4.analog-stereo
vshot record monitor --no-mic                   # 配置里记着麦克风时强制不录
vshot record window --app-audio                 # 录这扇窗自己的声音（可与 --mic 同时给，混成一条）
vshot record window --mic --app-audio           # 麦克风 + 窗口自己声音，混成一条音轨
vshot record window --follow GameA --follow GameB   # 焦点在哪扇就录哪扇，只跟这两扇
vshot record window --no-follow                 # 配置里记着跟随列表时强制不跟随
vshot record monitor --encoder-backend nvenc    # NVIDIA 硬编（默认 auto：先 VAAPI，再 NVENC）
vshot record mics                               # 列出这台机器上能录的音频输入
vshot record stop                               # 停止（读取 pid 文件发信号）
```

- **`current` 是哪块屏**：录制时 vshot 在屏幕上没有自己的面，而 Wayland 客户端只有在自己
  有面被指针覆盖时才会收到指针事件，所以 `current` 不能像截图那样问 seat——它问合成器
  （`hyprctl` / `swaymsg` / `niri msg` / KWin 的 D-Bus，与 pin 判断落点用的是同一个查询）：
  合成器能报指针位置时（Hyprland）就是指针所在的那块，否则是焦点所在的那块。都不报告时
  会明确报错，让你用 `monitor NAME` 或 `all`。

- **区域录屏（`record region`）**：录一块输出上的矩形，桌面逻辑坐标与 `vshot region
  --geometry` 同格式（`x,y 宽x高`），必须落在单块输出内（没有合成器调用能复制跨屏区域；跨了
  会明确报错）。不给 `--geometry` 时在冻结桌面上拖出矩形（与 `vshot region` 同一套选区器），
  确认前不打开麦克风、不创建文件，取消选区什么都不会留下。wlroots 会话上合成器把矩形直接
  渲染进 dma-buf，与 `record monitor` 一样零拷贝（实测 800×600 @120fps 稳定 241 帧/2 秒）；
  编码尺寸是矩形的逻辑尺寸 × 该输出的 scale（2 倍屏上 400×300 录出 800×600）。录制跟随矩形
  内的内容变化，但矩形本身固定在你画的位置。`--portal` 与 `region` 互斥：portal 的选择器只
  提供整块屏幕/整扇窗口，给不了矩形。
- **窗口录屏 ≠ 窗口区域录屏**：`record window` 录的是窗口**自己的像素**，由合成器把窗口本身
  复制出来（`ext_image_copy_capture_v1`，源是窗口的 `ext_foreign_toplevel_handle_v1`）。
  所以：被别的窗口盖住的窗口录出来是完整的；被拖到屏幕外一半的窗口也是完整的；窗口背后有
  什么都不出现在视频里；窗口在**别的工作区、甚至当前屏幕上看不到**时照样录得到（实测：录一个
  在隐藏工作区上的 Discord 窗口，OCR 出的是 Discord 自己的内容，而同位置录屏得到的是别的
  窗口）。窗口怎么指定：给 app id 或标题（先整名，再大小写不敏感的子串），`--pick` 点选，
  不写就是焦点那扇。合成器没有这套协议时会明确说明，让你改录屏幕。
- **窗口录屏中缩放窗口**：录制中窗口被**缩放**不会中断录制——合成器会重发新尺寸的缓冲约束，
  vshot 按新约束重建捕获缓冲池，并把新尺寸的画面**等比缩放适配进录制自己的画布**
  （比画布大就缩小，小就原尺寸居中，两侧加黑边），所以一条 MP4 的帧尺寸从第一包到 trailer
  始终不变，而文件是窗口完整的历史。适配在 VAAPI（dma-buf）路上由 GPU 的 `scale_vaapi`
  滤镜链完成；在 NVENC 那条路上由 CPU 做同样的等比缩放（那条路本来就要把帧搬过 CPU，编码仍在
  GPU 的 NVENC 单元上）。窗口被**关闭**则在那里结束——文件正常收尾（trailer 完整），stderr
  说明原因。
  窗口所在输出如果**关着/禁用/断开**，永远不会有帧送过来，这种情况几秒后会报错而不是一直等。
  实测：一扇浮窗在录制中被依次拉到 500×400、900×700、320×240，逐次被适配进开录时的画布，
  文件尺寸始终不变、帧数与时长相符、录制不中断。

- **录制怎么停**：`vshot record stop`（不需要显示器，可直接绑快捷键），或启动它的终端里
  按 Ctrl+C。两种方式都会先把文件正常收尾（libavformat 写完 trailer、采样表与索引）再退出，
  所以文件总是可寻址的 MP4。`--duration` 让它自己到点停。
- **输出路径**：全局 `-o/--output`，`%Y%m%d` 这类 strftime 会展开；不给时写入视频目录下的
  `vshot-%Y%m%d-%H%M%S.mp4`——视频目录先取 `$XDG_VIDEOS_DIR`，再取
  `~/.config/user-dirs.dirs` 里 xdg-user-dirs 记的那个（中文桌面通常是 `~/视频`），最后才
  是 `~/Videos`；这个目录不存在时会建出来，因为那是 vshot 自己挑的位置。你自己用 `-o`
  指的路径不会被建，目录不存在时会明确报出来。名字没有 `.mp4` 后缀会自动补上；`-`
  （stdout）不接受——一段视频不是终端能承载的东西。
- **帧率**：`--fps` 是循环"瞄准"的速率，每帧带着它在屏上的真实时长写入 MP4
  （可变帧率），所以编码跟不上时回放是"少几帧"而不是"变慢"。单块 4K 实测能稳定跑满
  120fps（本机 7900 XT 的上限约 149fps），远超 60fps。
- **编码器**：`--encoder` 在 h264（默认）、hevc、av1 之间选，三者都跑 GPU 的媒体引擎。
  编码与封装都走 ffmpeg 的库——`libavcodec` 出码流、`libavformat` 写 MP4 盒子，与
  wf-recorder 同一条路线——运行时用 `dlopen` 加载，所以没有 ffmpeg 库的机器上截图照常
  工作，只是 `record` 会说明缺什么。需要 `ffmpeg` 与 `libva`（AMD/Intel 的 VAAPI）。
  码流每帧都是 IDR（全帧内），任意播放器可读，且任何位置都能跳。
- **编码后端（`--encoder-backend`）**：`auto`（默认）、`vaapi`、`nvenc` 三选一。`auto`
  先试 VAAPI（AMD/Intel），打不开再试 NVENC（NVIDIA）；写死某一个时只用那个，打不开就
  明确报错并说明是哪一层失败（例如 `no CUDA device for NVENC`）。两条路的编码器名、
  像素格式与私有选项各自独立设置，`--encoder h264/hevc/av1` 在两条路上都能选。
  **两条路都是硬件编码**：跑的是 GPU 自己的媒体引擎（`h264/hevc/av1_vaapi` 或
  `h264/hevc/av1_nvenc`），vshot 里没有任何 x264/x265 之类的 CPU 编码路径。差别只在
  **帧怎么送到编码器**：VAAPI 能导入合成器的 dma-buf，全程零拷贝；**NVENC 没有 dma-buf
  导入**，它的帧要经 CPU 搬一趟（dma-buf 读回内存 → 转 NV12 → 上传显存），所以 NVENC 的
  CPU 占用略高，编码本身一样在 GPU 上。多显卡机器上 NVENC 默认用第一个 CUDA 设备，
  `VSHOT_NVENC_DEVICE=<索引>` 换一个。
  NVIDIA 机器还需要 `ffmpeg` 构建时带 `nvenc`（多数发行版默认带）。
- **麦克风（`--mic`）**：把麦克风录进同一个 MP4，编码是 AAC（ffmpeg 自己的编码器，和
  视频同一条 libavcodec 路线）。不带名字用会话的默认输入设备，给名字或节点序号录别的输入
  （`wpctl status` 列得出来；序号比如 `--mic 55` 就是那个 monitor）。麦克风在视频编码器之前
  打开——它的采样率与声道数要写进 MP4 文件头——样本在每个视频帧后抽干一次，两条轨共用一个
  时钟；文件里因此是一个 h264/hevc/av1 视频流加一个 AAC 音频流。`--no-mic` 在配置文件记着
  `cli.record.mic` 时也强制不录；两个都不给就是"按配置"（默认不录）。麦克风走 PipeWire，
  所以和 `--portal` 一样需要 libpipewire。实测（48 kHz 立体声）：录屏同时录本机 sink 的
  monitor，回放音量与参考 `pw-cat` 抓到的完全一致（-24.1 dB 对 -24.3 dB）。
  没有默认输入设备的会话会明确报错并提示看 `wpctl status`，而不是丢一句 PipeWire 的
  "no target node available"。
- **逐应用音频（`--app-audio`）**：额外录一段所录那扇窗**自己在播的声音**——别的应用在响
  什么都不会进去。只在 `vshot record window` / `vshot replay start window` 上有意义（其它
  目标没有"窗口"可挂，会被拒绝）。它可以**独立使用**（只要窗口自己的声音），也可以**和
  `--mic` 同时给**：麦克风是房间，应用音频是窗口，两者在 Rust 侧**逐样本相加**成 MP4 的那
  **一条**音轨（不是两条轨道，避免多数播放器只放第一条）。窗口的 pid 由合成器报告
  （Hyprland、niri 会给，KWin 由 scripting 探针报），vshot 拿这个 pid 去 PipeWire 的客户端
  表里找到该进程的播放节点，只连那一个。实测（Hyprland）：两个 mpv 分别播 880 Hz 与 220 Hz，
  单独 `--app-audio` 录出的文件主频是 880 Hz——隔离正确。Sway 与 labwc 不报窗口 pid，那两处
  会明确说明做不到，而不是悄悄退回麦克风。窗口没在放声音时保留原有音源（首次录制则只录视频），
  而不是报错。`--portal` 与 `--app-audio` 互斥（portal 由合成器决定录哪个窗口，vshot 拿不到
  它的 pid 对应关系）。音轨同样是 AAC，与麦克风那条路共用编码与封装。
- **跟随焦点（`--follow`）**：给若干窗口（`--follow NAME`，可重复），录制就跟着焦点在这些
  窗口之间移动——焦点落在其中哪扇就录哪扇，落在别处时**保持录上一扇**（不中断、不留空档、
  它不会把那扇窗录进来）。`--follow` 与窗口 `NAME`、`--pick` 互斥（它自己就是选窗方式），
  只对 `record window` 与 `replay start window` 有效。**不带 `--follow` 的 `record window`
  会跟随配置里 `cli.record.follow` 记着的窗口**（回录对应 `cli.replay.follow`），`--no-follow`
  则对那一次显式关掉——正如 `--no-mic` 之于记着的麦克风。换源走的是**和窗口缩放同一条路**：
  新窗口的画面等比缩放进文件开录时的画布，所以**一条 MP4 只有一个帧尺寸**、时间线连续。
  `--app-audio` 时音轨也跟着换到新窗口自己应用的声音——而麦克风那一路**不动**，切源只换
  应用那一路（两个应用都是 48kHz 立体声，格式不变，所以 MP4 头的音频声明始终有效；
  新窗口的应用若没在放声音，保留上一扇的，不退回麦克风）。焦点查询每 250ms 一次，
  只在给了 `--follow` 时才问合成器。
  实测（Hyprland）：A(900×500/880Hz) → B(600×340/220Hz) → 再回 A，14 秒录制得到 14.0s 的
  文件，宽度始终 900，音轨按时间窗 Goertzel 分析确认为 880Hz→220Hz→880Hz。
  合成器报不出焦点时会**明确报错**而不是永不切换（`--follow` 就是为拿焦点而给的）。
  注意跟随的是**焦点**，不是"正在玩的游戏"：切到不在白名单里的窗口时录制停在上一扇，
  但那一瞬间你的操作不会被收录。
- **宽度上限 4096**：这是硬件 H.264 编码器的限制（本机 7900 XT 的 VCN 实测如此），
  所以两台 4K 屏拼合出的 `record all`（5760 宽）会被拒绝并说明原因——录单块屏即可，
  或改用 `--encoder hevc`（本机可编 7680 宽的全桌面）。单块 4K（3840）没问题。
- **记住的默认值**：`--encoder`、`--encoder-backend`、`--fps`、`--portal`、`--mic`、
  `--follow` 不写时，去配置文件的 `cli.record` 段取值（见
  [`cli`——命令行默认值](#cli命令行默认值)）；`replay` 对应 `cli.replay` 段。`--no-portal`、
  `--no-mic` 与 `--no-follow` 是对**那一次**录制把记着的值关掉——有了它们，配置里记着
  `portal: true`、某个麦克风或一份跟随列表时也不必每次先改配置。`cli.record.follow` 只在
  不带窗口名的 `record window` 上生效，别的目标会忽略它（而不是报错），所以记着跟随列表也
  不影响 `record monitor`。
  `vshot record mics` 列出这次会话里能录的音频输入，每行是 `节点序号	节点名	说明`
  （节点名就是 `--mic` 收的值），设置窗口的麦克风下拉读的正是它；没有输入设备的会话会说明
  一句，而不是报错。（`--app-audio` 只认命令行，不进配置文件——它录的是"这一扇窗的声音"，
  换个窗口就不是同一个意思了。）

- **`--portal`：走桌面 portal 录制**（`org.freedesktop.portal.ScreenCast`）。合成器自己的捕获
  协议（wlr-screencopy、KWin 的 ScreenShot2、`ext_image_copy_capture_v1`）各自只在部分桌面
  存在，而 portal 是每个桌面都有的那一套——代价是**由合成器决定录什么**：它会弹出自己的
  选择器，在那里选中的屏幕/窗口就是录下来的内容。`record monitor --portal` 让它列屏幕，
  `record window --portal` 让它列窗口，但名字与 `--pick` 决定不了具体是哪一个；每开一个会话
  都会弹一次选择器。一次只录一路流，所以 `record all --portal` 会被拒绝。
  屏幕投射是**按需出帧**的：画面变了才出一帧，不变时一帧都不出，所以静止画面在文件里是一帧
  长帧；`--fps` 是向合成器要的采样上限，而不是文件帧率。帧以 dma-buf 送来时零拷贝进编码器，
  否则在 CPU 上转成 RGBA；`VSHOT_PORTAL_SHM=1` 强制走内存拷贝那条（合成器的缓冲编码器导入
  不了时的备用）。需要 `xdg-desktop-portal` 与 `libpipewire`：编译时要有它的头文件，库在
  运行时用 `dlopen` 加载，所以缺了只是没有 `--portal`，其他功能照常。

> 为什么不用 libva 直接编码？我们试过，而且最初的实现就是它。这台机器上
> mesa-git 26.3.0-devel 的 radeonsi 编码器会在 `vaEndPicture` 内部解引用空指针
> （驱动里 `mov 0xb0(%rdi),%rax`、rdi=NULL）——复现率约 50%，且与调用形态无关
> （复用/每帧新建 coded buffer、`vaSyncSurface`/`vaSyncBuffer`、单线程转换全都一样崩）。
> 同一台机器上 libavcodec 的 `h264_vaapi` 跑几百帧零故障，所以编码边界交给
> libavcodec；这是工程决定，不是审美。

## 回录

`vshot replay` 是「一直在录，但不落盘」的录屏：屏幕持续编码，编码后的包进一个**内存环**，
`vshot replay save` 把环里现有的内容拷成一个 MP4 —— **流拷贝，不重新编码** —— 所以触发几乎不
花时间，触发之前不写任何东西到磁盘。适合打游戏时挂着，出精彩操作按一下就把刚才那几十秒留下。

```sh
vshot replay start monitor --background     # 后台挂起，默认留最近 30 秒
vshot replay save                           # 落盘到视频目录，带时间戳
vshot replay save /tmp/clip.mp4 --seconds 10
vshot replay status                         # 环现在覆盖多少秒、已服务多少次保存
vshot replay stop
```

`start` 的目标和 `record` 完全一样：`monitor [NAME]`（不写即你当前所在的那块）、`all`、
`region`（`--geometry` 或冻结桌面上拖出）、`window`（焦点窗口、按名字、或 `--pick`）。帧走相同的
捕获后端、相同的 libavcodec GPU 编码器，`record` 用零拷贝 dma-buf 的地方回录也用。

### 为什么是「边采边编」而不是「触发时再编」

回录要能拿到**已经过去**的几十秒，这些帧在你按键之前就产生了。若把它们以原始帧留在内存：
4K60 的 NV12 每帧约 12 MB，30 秒约 22 GB，显存和内存都扛不住。而同样 30 秒、30 Mbps 的**编码后**
数据只有约 110 MB。所以回录必须一直在编码——这是「能回看过去」的物理代价，无法绕开。能省的是
编码本身的成本：硬编、按需降帧率、有界 GOP。

### 内存环与关键帧对齐

- **有界 GOP**：编码器用 `--gop` 秒的关键帧间隔（默认 1），而不是录制那样的全帧内编码。环因此
  只是全帧内码流的一小部分，而且每个 GOP 边界都是一次保存可以起头的地方。
- **环保留 `--window` + 一个 GOP**：一次保存从「不晚于 `now - seconds` 的最后一个关键帧」开始，
  那个关键帧可能比目标时间点早最多一个 GOP，所以环要多留这一截，否则会正好把要用的关键帧丢掉。
- **保存从关键帧起头**：MP4 的第一个视频包不是关键帧的话，播放器在下一个关键帧之前什么都显示不
  出来。因此保存对齐到关键帧，文件至少有你要求的秒数、并且从第一个字节就能解码。要的比环里有的
  还多就给全部。

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

控制走 `$XDG_RUNTIME_DIR/vshot-replay-<uid>.sock`，`save`/`status`/`stop` 都不需要显示器，可直接
在合成器里绑：

```
bind = SUPER, R, exec, vshot replay start monitor --background
bind = SUPER SHIFT, R, exec, vshot replay save
bind = SUPER ALT, R, exec, vshot replay stop
```

按一下 `save` 会发一条桌面通知，告诉你文件落在哪、多长。

### 配置

配置文件的 `cli.replay` 段给出默认值（也可以跑 `vshot settings`，在「录制」页的回录一栏里改）：

```jsonc
"cli": {
  "replay": {
    "window": 30,            // 保留秒数
    "fps": 30,
    "encoder": "h264",       // h264 / hevc / av1
    "encoder-backend": null, // auto / vaapi / nvenc；null 即 auto
    "gop": 1,                // 关键帧间隔秒数（1-10）
    "mic": null,             // "" 是默认输入设备，名字是某个 PipeWire 节点
    "follow": null,          // 跟随的窗口名数组，如 ["game", "chat"]；null 即不跟随
    "portal": false,         // 回录暂不支持 portal
    "save-dir": null,        // 落盘目录，null 用视频目录
    "notify": true           // save 是否发通知
  }
}
```

命令行永远压过文件。一个写坏的值（未知编码器名、越界的秒数）回落到内置默认，而不是让整个回录
失败。回录一次只跑一个会话（pid 文件挡着第二个），`VSHOT_REPLAY_SOCKET` 覆盖控制 socket，
`VSHOT_REPLAY_PIDFILE` 覆盖 `replay stop` 读的 pid 文件。

### 回录当前不支持

- **portal**：portal 自己的出帧循环还没接到内存环上，`replay start --portal` 会明确拒绝，让你改用
  `record ... --portal`。
- **窗口回录的空闲语义**：`window` 走的是 `ext_image_copy_capture_v1`，合成器在窗口内容不变时
  不复制帧（与 `record window` 相同）。所以一个静止窗口在环里可能长时间只有很少的帧；动起来的
  窗口则正常。这是协议本身的按需出帧，不是回录的缺陷。

## pin 浮层

`vshot pin` 把图片作为浮层钉在屏幕上，由**常驻 daemon** 持有：

- **拖拽**移动（可跨显示器，跨屏时另一块屏上的副本同步跟随）
- **滚轮**以图片中心缩放（0.1x–8x），倍率短暂显示在图片右下角
- **双击**关闭该图
- **左键点击**把该图提到最前，重叠时被点到的一定压在其余之上
- 鼠标停在哪张图上，哪张图的描边就是纯黑；其余是浅灰（2 逻辑像素粗，不遮挡图像本身）
- **外观可配**：圆角、阴影、边框宽度与两种状态的边框颜色都在配置文件里（默认直角 + 阴影），见[「`pin`——pin 浮层外观」](#pinpin-浮层外观)
- 指针在某张 pin 上且该屏持有键盘时按 **Space** 进入与 `vshot region` 相同的标注编辑器
- 右键点击**任意** pin 弹出菜单：色卡在前几行列出它的各种格式，点哪一项就把那个值抄回剪贴板（↑/↓ 选行、回车抄走、Esc 关闭；复制成功后右下角闪一个徽标）；最后一行 `另存为…` 对所有 pin 都在，选了就弹出保存对话框把这张图写成 PNG，存完在右下角报结果

新 pin 落在**激活的输出**上：指针所在的那块屏优先，指针读不到时退回键盘焦点所在的输出，都没有则回退主输出。

pin 的对象：

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

色卡左边是颜色本身的色块，右边按 `HEX` / `RGB` / `HSL` / `HSV` / `CMYK` 逐行写出（半透明时多 `HEX8` 与 `RGBA`，有具名色时多 `NAME`），色块底下铺棋盘格表示半透明。文字卡片与色卡都按所在输出的像素密度渲染，HiDPI 下不模糊。

剪贴板为空、有内容但既无图像也无可用文字、或缺少 `wl-paste` 时都会给出明确提示，且不影响已有的 pin。

### pin 的尺寸

pin 的尺寸按**图片的来源密度**决定，默认不需要任何参数。判定顺序：

1. **手动指定**——`--density N`（1–4）或环境变量 `VSHOT_PIN_DENSITY=N`，优先级最高；
2. **vshot 自己的截图**——直接带上来源输出的缩放；
3. **图片自己声明**——PNG 的 `pHYs` 块（192 DPI = 2 倍；96 DPI 就是 1 倍）。判定看**这个块在不在**而不是看 Qt 报回的数值，因为一张没有 `pHYs` 的 PNG 也会被 Qt 读成 96 DPI 左右。vshot 自己写出的 PNG 都会带上这个块；
4. **截图工具留下的记录**——`<图片路径>.scale` 文件里单独一个数字，或 `$VSHOT_PIN_SOURCE_FILE`（默认 `/tmp/screenshot-path`）里的一行 `<图片路径> <缩放>`（路径需与正在 pin 的图一致）。截图脚本保存图片后写一行即可：

   ```sh
   echo 2 > "$shot.png.scale"
   ```

5. **按图片尺寸与落点输出推断**——图片像素数放得进落点输出的原生分辨率时按 1:1；放不进时取"能容纳它的最小的那块屏"的缩放，最后再按输出宽度收一次上限（不放大）。

因此同一张图 pin 到 2 倍缩放的 4K 屏上时占的逻辑尺寸是 1080p 屏上的一半，**两块屏上的物理大小一致**。`--density` 是最后的手动兜底。`VSHOT_PIN_DEBUG=1` 打印每张 pin 的判定（像素数、最终密度、来源、落点屏的 `devicePixelRatio`），`VSHOT_PIN_FOCUS_DEBUG=1` 打印每个渲染面每一次焦点变化（用来查描边颜色为什么不对）。

### pin 编辑模式

聚焦某个 pin 后按 **Space**，daemon 导出该图并拉起完整标注编辑器（工具栏、文字、马赛克、撤销/重做），与区域截图一致：

- 编辑器覆盖 pin 所在的整块屏幕，但**不自己绘制图片**——画面上的图就是那个真实的 pin，编辑器只在其上叠加标注；标注超出图片的部分会被裁掉；
- 选中工具在图片上拖动时，编辑器通过 daemon socket 直接复用 pin 自身的移动逻辑，**不产生副本**；方向键可微调，Shift 加速为 10px。图片不会被拖到屏幕外；
- **Esc 取消**：标注被丢弃、像素保持原样，但位置不回退——拖到哪儿就留在哪儿；**Enter 或 OK 确认**则连同标注一起写回；
- 一次只能有一个 pin 处于编辑会话。

### pin daemon

layer-shell 浮层 surface 由创建它的进程拥有，因此需要一个常驻进程：

- 复用 Qt 二进制：`vshot-qt-ui --pin-server <socket>` 即 daemon，首次 `vshot pin` 连不上 socket 时自动分离式拉起（不占终端）；
- CLI 是瘦客户端，通过 Unix socket 发送单行 JSON 请求；socket 默认 `$XDG_RUNTIME_DIR/vshot-pin-<uid>.sock`，可用 `VSHOT_PIN_SOCKET` 覆盖
- 关闭最后一张 pin（或 `--close-all`）约 0.5 s 后 daemon 自动退出；下次 pin 命令自动重新拉起。`vshot pin --quit` 可随时手动退出；
- **不要用 `pkill` / `kill -9` 结束 daemon**：它持有 layer-shell surface，被强杀时部分合成器（实测 Hyprland 0.56）会残留该 surface 与其截屏会话，导致**所有输出的 screencopy 永久阻塞**（`vshot` / `grim` 全部超时，`hyprctl reload` 也无法恢复，只能重启会话）。请始终用 `vshot pin --quit`，它会在退出前 unmap 全部浮层；daemon 也已处理 `SIGTERM` / `SIGINT` 走同样的优雅路径。

Wayland 客户端收不到全局按键，所以"一键显隐"需要自己绑到合成器快捷键，例如 Hyprland：

```
bind = SUPER, P, exec, vshot pin --toggle
bind = SUPER SHIFT, P, exec, vshot pin --close-all
```

## 图像与输出映射

内部帧统一为 RGBA8、top-left origin。多输出合成支持负 logical origin 和输出间空隙；场景画布使用最高输出 scale，较低 scale 的输出用 nearest-neighbor 放大。

**落在单个输出内的矩形一律从那块输出自己那份原生帧裁剪，并按那块屏的 scale 写密度**：`region` 的 `--geometry` 与交互选择、`window active`、`window pick` 以及像素识别给出的矩形都走这条路，只有**跨接缝**的矩形才回落到合成场景；`all` 是整块桌面，只能由场景给出。这条规则是必需的：从合成场景裁一块 1080p 屏上的矩形会得到两倍大且发虚的图。

当前要求正整数 scale、`transform=normal` 以及可安全证明的 logical/pixel 映射；fractional scale、旋转和无法证明的映射会清晰失败，而不是生成疑似错误的截图。这个校验只在**需要把输出合成为场景**的路径上生效，所以 KWin 与 niri 直接给窗口像素的两条路在旋转/翻转输出上仍然可用。

## 截图后端与 KDE 授权

启动时探测一次：

1. 先连 `wlr-screencopy-unstable-v1`。只有它以「缺少 `zwlr_screencopy_manager_v1`」失败时才说明这个合成器不提供该协议；
2. **KWin ScreenShot2**——KWin 的私有会话总线服务 `org.kde.KWin.ScreenShot2`（KWin 既没有 screencopy，也没有 `ext-image-copy-capture`）。vshot 传一根管道的写端，KWin 把像素写进管道并在回复里给出 `width` / `height` / `stride` / `format` / `scale`。像素是预乘 alpha 的 BGRA，输出截图把 alpha 归一为 255，窗口截图原样保留。

两个后端都不可用时，错误信息会同时说明两边的具体原因。两者都没有实现 PipeWire、Portal ScreenCast 或 DMA-BUF，GNOME/Mutter 三者都不提供，连 layer-shell 也没有，因此**不支持 GNOME**。

### KDE 授权

KWin 的 `ScreenShot2` 是**受限 D-Bus 接口，且不弹任何对话框**——没有可点的"允许"。KWin 由调用方 pid 取 `/proc/<pid>/exe`，在已安装的 desktop file 里找 `Exec=` 第一个词与该路径一致的那一份，然后要求它声明：

```ini
X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2
```

- **装包安装的 vshot 不需要任何额外操作**，包里的 `/usr/share/applications/vshot.desktop` 已经声明。若装完包 KWin 仍拒绝，跑一次 `kbuildsycoca6 --noincremental`；新建的 desktop file 可能还有几秒延迟，隔几秒重试即可；
- **直接从 `target/release/vshot` 跑的开发构建拿不到授权**——没有任何 desktop file 的 `Exec=` 指向那个路径。加上一份即可（`Exec=` 写当前二进制的绝对路径，vshot 报错时会把这个路径打出来）：

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

- **Hyprland**——本机会话就是 Hyprland，也是主要开发与验证环境：截图、选区标注、窗口、长截图、pin 与滚动注入都在这里跑过。
- **录屏编码后端**——本机（7900 XT）是 VAAPI：零拷贝 dma-buf、h264/hevc/av1、`--fps`、窗口中途缩放、portal 与回录都在这条路上实测过。NVENC 的**选路与失败路径**已验证（`--encoder-backend nvenc` 在无 NVIDIA 的机器上干净失败，消息带具体原因如 `no CUDA device for NVENC`），但**真实的 NVENC 编码从未在 NVIDIA 硬件上跑过**——CPU 搬运那条路（dma-buf 读回 → 转 NV12 → 上传，编码仍由 NVENC 单元做）与其中的窗口缩放适配是按代码审查实现的，没有现场数据。
- **逐应用音频**——在 Hyprland 上实测：两个 mpv 分别播 880 Hz / 220 Hz，`record window --app-audio` 录出的文件主频为 880 Hz，隔离正确；无声窗口降级为只录视频。窗口 pid 的来源在**能录窗口**的合成器上都有（Hyprland、niri 由合成器直报，KWin 走 scripting 探针的 `pid` 字段），因此 `--app-audio` 在 Plasma 上也能按 pid 找到应用音频；niri 也报了 pid，所以走 ScreenCast 服务的窗口录制同样能按 pid 找到应用音频（实机未验证隔离）；Sway 与 labwc 没有窗口 pid 来源，会明确拒绝而不是静默退回麦克风。niri 与 KWin 的 pid 路径只有单元测试与无头实测，未在真实音频会话上验证隔离。
- **麦克风 + 应用音频共存**——在 Hyprland 上实测：一路虚拟麦克风源播 440 Hz、mpv 窗口播 880 Hz，`record window --mic --app-audio` 录出**单条** AAC 音轨，Goertzel 分析在整个时长内同时检出 440 Hz 与 880 Hz，长度与视频一致（8.000s 对 8.000s）。`--follow` 切窗时麦克风那一路**全程不断**（每秒 440 Hz 幅度恒为 0.212），只有应用那一路换到新窗口（880 Hz → 660 Hz）。回录路径（`replay start window --mic --app-audio`）同样含两路。
- **跟随焦点（`--follow`）**——在 Hyprland 上用两个不同尺寸/音调的 mpv 实测：焦点 A → B → A 得到一个 14.0 s 的文件，宽度始终 900（B 的 600×340 被缩放进 A 的 900×500 画布），音轨按时间窗分析为 880 Hz → 220 Hz → 880 Hz。焦点查询、白名单匹配与"保持在原窗"三类判断都有单元测试；**报不出焦点的合成器未实测**（Hyprland 能报）。
- **niri**——平铺与浮窗两条**截图**路径都已实机验证：平铺窗口拿两个并排 kitty 测（残差 0.45/0.51 每通道），浮窗走 niri 的 `tile_pos_in_workspace_view` 坐标加 `matches_at_position` 验证，实机结果正确；浮窗截图若出现错位，用 `--no-blend` 绕开定位。niri 的焦点查询（`niri msg --json focused-window`）与窗口列表已接进统一的窗口表，供选窗与像素检测的候选名单使用。**窗口录制/回录走的是合成器自己的 ScreenCast 服务**（`org.gnome.Mutter.ScreenCast`），不是 wlroots 那条协议：实测 niri 26.04（`v26.04-347-gcd434f86`）的 Wayland 全局里已有 `ext_image_copy_capture_manager_v1`、`ext_output_image_capture_source_manager_v1` 与 `ext_foreign_toplevel_list_v1`，但**没有** `ext_foreign_toplevel_image_capture_source_manager_v1`（窗口的 capture source），所以 wlroots 那条路仍然在连接阶段明确报错（旧版 25.11 连前三个都没有）。niri 自己实现了 GNOME 的 ScreenCast 接口（`src/dbus/mutter_screen_cast.rs`，`xdp-gnome-screencast` 特性默认开启），`Session.RecordWindow(window-id)` 直接建流、`Stream.PipeWireStreamAdded` 给出 PipeWire 节点号，vshot 连到会话的 PipeWire 守护进程读帧——**不需要 portal、没有 picker、没有授权弹窗**，而且 vshot 直接连 D-Bus 名字，不受"多个合成器共享一个会话总线、xdg-desktop-portal 绑在别的后端"这类环境问题的影响。窗口 id 来自 `ext_foreign_toplevel_handle_v1` 的 identifier：niri 明确把它编码成窗口 id 的十进制字符串（`MappedId::to_protocol_identifier`，注释里写明就是为了让客户端能把 toplevel handle 对上 IPC 窗口 id），所以按名字或 `--pick` 选中的窗口能直接对上服务要的 id。这条服务只在 **session 实例**（display manager 或 `niri-session`）里注册；从 TTY 手动起的非主实例要加 `debug { dbus-interfaces-in-non-session-instances }` 才会注册（判断在 niri 的 `src/dbus/mod.rs:72`）。`--follow` 在这条路上不可用：服务 cast 的是启动时那一扇窗。顺带更正一条旧说法——niri 的 **portal 是实现了 ScreenCast 的**：`niri-portals.conf` 不列它是因为 `default=gnome;gtk;` 已经覆盖，provider 就是 gnome portal，niri 文档也明写 `xdg-desktop-portal-gnome` 是 screencasting 的必需项。**实测（本机 niri 26.04，`v26.04-347-gcd434f86`）**：`record window` 录一个 4K 窗口（3792×2024，dma-buf 零拷贝、AV1/VAAPI）正常出片；窗口中途全屏改尺寸（940×2022 → 1882×2038）时 shim 重建 fit 链、录制继续到完整时长，抽帧验证 letterbox 几何正确（内容高 1018、上边距 502，与按 0.4995 缩放居中吻合）；窗口关闭时录制**结束并说明原因**；`replay start window` 的环形缓冲、`status`、`save`、`stop` 全部跑通。活动会话下实测 **5 秒 240 帧（48fps，受窗口自身重绘率限制）**，每帧 mux 约 0.2ms。**注意一个 niri 侧的限流**：它的 cast 是在**输出渲染循环**里画的（`niri.rs:4861` 的 `render_windows_for_screen_cast`），所以会话不活动（`loginctl` 报 `Active=no`）时渲染基本停摆，实测掉到约 1.3fps；要让 niri 以正常帧率录制，它的 VT 必须是当前活动 VT。
- **KWin/Plasma**——D-Bus 采集（`CaptureScreen` 的尺寸与不透明性、`native-resolution`、格式字段）、窗口列表与长截图均已实测，其中采集与窗口列表的自动化测试是在**无头 `--virtual` KWin** 上跑的。此外**窗口录制/回录（`record window`、`replay start window`）已在本机无头 `--virtual` KWin 6.7.5 上实机验证**：
  - 路由是 `org.kde.KWin.ScreenShot2.CaptureWindow`，按窗口的 `QUuid`（scripting 探针报的 `internalId`）抓那扇窗自己的像素，装饰含在内；
  - 这条接口是 KWin 的**受限接口**，而且授权是**每次调用**都查一遍：KWin 把调用方的 pid 换成可执行文件路径，再去桌面文件数据库里找 `Exec=` 指向它、并声明了 `X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2` 的那个 `.desktop`。这个查找走 KService/ksycoca，而 ksycoca 在被重写期间会查出空结果，于是**刚授权过的客户端也会被临时拒绝**。实测：本机 KDE 会话里 `~/.cache/ksycoca6_*` 在**没有任何 vshot 进程**时也被每秒重写约 2.5 次，一次 42 秒的窗口录制因此在连续 10 次拒绝后整个失败。所以这类拒绝**不再按丢帧处理**：vshot 按帧率重试最多 10 秒，日志每秒最多一行（完整解释只在第一次打印），期间恢复就接着录，超过 10 秒才带着 KWin 自己的解释放弃。两个方向都在真机上验证过：拒绝约 5.5 秒后恢复 → 录制继续并正常收尾（90 秒的录制不受影响）；拒绝持续超过 10 秒 → 按预期中止并把原因写清楚。窗口、整屏两条录制/回录回路都按这个规则走；
  - scripting 探针已扩展成 8 个制表符分隔字段（`x y width height pid class title handle`），所以 KDE 上**也有了窗口 pid**——`--app-audio` 在 Plasma 上不再被拒绝，而是照常按 pid 找到应用音频；
  - 窗口捕获**关掉了阴影**（`include-shadow=false`）：录像是 NV12、没有 alpha，带阴影的透明外圈会变成一圈黑边并把画布撑大（实测一个 941×768 的窗口带阴影回来是 1072×898）；
  - 画布尺寸**向上取偶**：`ScreenShot2` 给的是窗口的客户区几何，可能是奇数宽/高，而 NV12 没有奇数形式（奇数会让 libavutil 的转换直接断言崩溃），所以录制/回录打开的画布是偶数，多出的一列/一行由 fit 路径补齐。实测：941×768 的窗口录成 942×768 的 h264 文件；
  - `--follow` 已实测：两个窗口间切焦点，录制跟着切，一次会话一个文件，时长连续；
  - 回录（`replay start window`）已实测：环形缓冲、保存、状态查询都跑通；
  - **仍未验证**：`--cursor`（无头输出没有指针可画）、窗口被点选时 `--pick` 的整段交互、以及 Scroll injection（见下）。
  - **长截图已验证可用**。KWin 的 `CaptureArea` 是私有 API（参数顺序未核实），所以 KDE 上抓整屏再裁，每帧开销明显高于别的合成器；
  - **滚动注入未验证**。`portal` 走 KWin 自带的 EIS 服务端，但本机 Hyprland 的 portal 不实现 RemoteDesktop，**这条从未在真机上跑通过**，需要 KDE 上验收；`uinput` 依赖 `/dev/uinput` 写权限。
- **Sway**——只有探针实现与单元测试，**没有现场验证**。
- **labwc 等其它提供 wlr-screencopy 的合成器**——理论上基础截屏可用，但没有窗口列表与 pin 落点探针，且**没有现场验证**。只有读像素的操作（`region --geometry`、`monitor <名字>`、`all`）可用；`monitor current` 与交互式选区要指针/键盘 seat，通常也行。
- **GNOME**——不支持，没有实现计划。Mutter 不提供 wlr-screencopy、也不提供 KWin 那套 D-Bus 服务，连 layer-shell 都没有。

### 光标（`--cursor`）

vshot 自己从不画光标，`--cursor` 只是给合成器的捕获请求置一个"叠加指针"标志（`wlr-screencopy` 的 `overlay_cursor`，KWin 是 `include-cursor`），画不画、画在哪、什么时候画，全由合成器决定。由此有三条实测结论：

- **终端里敲命令后截图没有光标，不是 bug**。终端（kitty 实测，`mouse_hide_wait` 默认 3.0 秒）在鼠标不动几秒后会把指针藏掉——对合成器执行 `set_cursor(null)`，此后合成器层面**真的没有光标可画**，任何走 screencopy 的工具都一样（`grim -c` 同时刻也抓不到）。回车后动一下鼠标再去截，或者给 kitty 设 `mouse_hide_wait 0`（打字时指针不再自动隐藏）。其它终端/程序的同类"打字后藏指针"行为同理。
- **niri 把指针画进的是窗口截图，不是输出帧**。niri 上 `window active` / `window pick` 走它自己的 `screenshot-window`，`--cursor` 映射为该调用的 `--show-pointer`，光标画在窗口图里；`--show-pointer` 是 25.11 之后才有的参数，旧版 niri 会拒绝整个请求，vshot 检测到后自动去掉该参数重试并提示"指针画不进窗口"。输出级路径（`monitor`、`all`、`region`）仍是 screencopy 的 `overlay_cursor`。
- **Hyprland 上开着 `hypr-dynamic-cursors` 时，指针会被烤进帧里，vshot 自己绕开**。该插件在光标被放大期间（摇晃找光标，或它自带的 magnify dispatcher）会锁住软件光标，此后合成器把指针画进它合成的帧里，而 screencopy 交出的正是这张帧——所以**不传 `--cursor` 也照样有光标**；放大结束后那块残留像素还会一直留在屏上（同时刻 `grim` 抓到的残留与 vshot 完全一致，可见是合成器侧行为，不是 vshot 特有）。vshot 在读整屏前会临时关掉插件，并把指针 warp 到它原来就在的位置——这个指针事件正是合成器切回硬件光标、并重绘那块残留矩形的触发点——读完帧立刻开回来；插件没装、或本来就关着时什么都不做。截图里因此不会出现残留光标，屏上的残留也顺带被清掉。

`--cursor` 对 `long` 无效（见下一条）；KWin 上是否真的画出光标未验证。

### 已知不稳定点

- **`--cursor` 在 `long` 上无效**——`src/longshot.rs` 的抓帧调用把光标参数写死为 `false`（`longshot::run` 的签名里也没有这个形参），所以 `vshot long --cursor` 会被接受但静默忽略。其余捕获路径（`region`、`monitor`、`all`、`window active`）的 `--cursor` 在本机 Hyprland 上实测有效。KWin 上 `include-cursor` 虽然传了，但**是否真的画出光标未验证**。
- **抓取正好卡在放大过程中时，帧里可能仍有一个指针**——`hypr-dynamic-cursors` 在放大期间持有软件光标锁，而关掉插件不会让它立刻释放，所以这一瞬间抓的帧会带一个**正常大小**的指针（放大后的那个不会进帧）。放大结束后插件自己解锁，问题自愈：之后任何时候再截都是干净的。
- **像素识别（`--pixel`）**——无缝无边框平铺（无 gaps、无阴影）与完全均匀的桌面上没有任何像素信号，此时如实报错而不是猜。合成器不给焦点窗口描有色的边时，它答的是**指针下的窗口**，可能与合成器报的焦点窗口不一致。`VSHOT_PIXEL_DEBUG=1` 查每一级的判定。
- **niri 半透明窗口定位**——模板匹配要求窗口内容在渲染与抓帧之间不变；视频、动画会让模板过期（会带新渲染重试至多 3 次），几乎全透明或悬在输出边缘之外的窗口定位不到（退回 niri 原样的透明渲染）。
- **KDE 窗口列表探针**——依赖 journald 收到 KWin 的 `console.info`。KWin 从 tty 起、日志只进那台 tty 时，无论等多久都取不到行。装了 `kdotool` 时优先走它（结果经 D-Bus 回给自己，不经 journal），绕开这个依赖。
- **niri 的会话判定**——niri 不设 `XDG_CURRENT_DESKTOP`/`XDG_SESSION_DESKTOP`（从 TTY 手动起会话时是空的），所以判定改用 `NIRI_SOCKET` 文件名。判定出错时表现为 `window pick` 打开压暗 overlay 而不是 niri 的十字选窗，用 `VSHOT_SESSION_DEBUG=1` 看判成了谁。
- **niri 的 pin 落点**——只能问"焦点窗口所在的输出"（niri 的 IPC 没有指针位置查询），所以**不跟随指针，只跟随键盘焦点**。
- **pin daemon 与 screencopy**——不要用 `pkill`/`kill -9` 结束 daemon（见[「pin daemon」](#pin-daemon)），否则部分合成器会残留 layer surface 与其截屏会话，导致所有输出的 screencopy 永久阻塞。

## 配置文件

`vshot` 把两样东西记在 `$XDG_CONFIG_HOME/vshot/config.json`（缺省 `~/.config/vshot/config.json`）：**标注编辑器的样式**，以及**部分命令行参数的默认值**。文件是可选的——没有它、读不了它、或者内容坏了，都退回内置默认值，不会影响截图。

改它有两种方式：**手改文件**，或者跑 **`vshot settings`** 开一个窗口改——窗口里两段都能改，保存即写盘。**截图会话中的任何改动都不写回**：会话里选的颜色、调的线宽、切的工具都是这一次的工作状态，配置是每次会话的**重置起点**，只在你明确操作（设置窗口保存、手改文件）时变化。

```bash
vshot settings
```

窗口只是普通窗口，不截图、不需要任何合成器协议，所以在一个 vshot 本来截不了图的合成器上也能用。装包后也可以直接从**应用菜单**里的「VShot Settings」打开（见[「应用菜单入口」](#应用菜单入口)）。`cli` 段的数值留空/留 0 表示「不设，用内置默认」，而不是把 0 存进去；**设置窗口里这些框直接显示内置默认值**——数字框显示 `60`、`30000` 这样的真实数字，下拉框的第一项写成 `fast（内置默认）`——所以不必去翻上面那张表就知道会生效什么。把默认值留在原处不动，保存时照样不写进文件（判断的就是「值还等于内置默认」），所以哪天内置默认改了，没动过它的人会自动跟上。`editor` 段则总是整段写出。保存是**合并写入**：本版不认识的键（新版 vshot 写的、或你自己加的）原样保留，不会因为存一次就被抹掉。

**保存之后窗口不关**，左下角显示「已保存。」。这样一改一存一看、再回来接着改都行；关窗口按「取消」——那时它的意思就只剩「关闭」。

窗口分七页，左边栏切换，**一页一个功能**：**标注编辑器**（工具、颜色、线宽、线型、箭头、文本、马赛克）、**输出**（PNG 压缩、默认显示器）、**滚动截图**（滚动格数、高度/帧数上限、超时、忽略顶部、滚动后端）、**文本识别**（识别完成时的通知开关）、**录制**（`record` 与 `replay` 的全部默认值：编码器、硬件后端、帧率、portal、麦克风、跟随列表、通知开关，以及回录的保留时长、关键帧间隔与保存目录）、**文件对话框**（圆角、边框宽度与颜色、阴影）与 **Pin 浮层**（尺寸即 pin 密度、圆角、阴影、边框宽度与两种状态的边框颜色）。早先这些命令行默认值是挤在同一页里的，改一个录制选项要滚过三张不相干的卡片，所以按功能拆开了。每页是一列卡片，一行一项，标签在左、控件在右；除**录制**页外，各页在默认窗口尺寸下都**不需要滚动**——录制页装着 `record` 与 `replay` 两段共十四行，比视口高两百来像素，要滚一点，这是拆页时明确的取舍（拆成「录制」「回录」两页会让 `replay` 的默认值离 `record` 太远）。下拉框与数字框的箭头是自绘的（原生那套是带斜面的老式三角），所以控件外观与工具栏一致。画这些箭头时注意：**`paintEvent` 里 `QPainter` 的坐标系已经是逻辑像素**，Qt 早把输出缩放折进去了，再除一次 `devicePixelRatioF()` 会让坐标在 2 倍屏上全缩小一半、箭头挤到右上角——这个问题在 1 倍屏上完全看不出来，所以两种缩放都值得看一眼。

```json
{
  "editor": {
    "tool": "arrow",
    "color": "#ff8800ff",
    "width": 4,
    "textPixels": 28,
    "dash": "dotted",
    "arrowSize": 3,
    "arrowStyle": "filled",
    "mosaicShape": "brush",
    "mosaicStrength": 3,
    "font": "Noto Sans"
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

这个段描述的是**每次会话的起始样式**。会话中怎么改都不会写回——这里存的是重置值，不是上次的遗留；改它请用 `vshot settings` 或手改文件。

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

`tool` 是**编辑状态**的起始工具，和其他样式一样**只在配置里**：会话中怎么切换都不写回。区域截图总是从 Select 打开——它的第一步是拖出选区，直接进绘图工具（比如 Text）会让第一次点击变成放置文本；`window pick` 与滚动截图同理永远从 Select 开始。取值超出范围的整数会被夹到范围内，不认识的名字按默认值处理——手改文件写错了不会报错，只是那一项不生效。

**`textPixels` 的单位是像素**：你填 14，字就是 14 像素高，跟编辑器里那个数字框完全一致。旧协议里那个「字符格整数倍」的刻度（1–64，一格 7 像素）只在把结果写出去时换算一次，界面上看不到它。

早期版本这里叫 `textSize`，存的是那个刻度。**旧文件会被自动迁移**：读到 `textSize` 就按刻度换算成像素（`2` → 14 px、`3` → 21 px），下次保存时写成 `textPixels`，旧键删掉。两个键同时存在时以 `textPixels` 为准。换键名而不是沿用，是因为 7–64 这一段两种解释都合法，靠猜会出错。

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

`pin.density` 的优先级同样是 `--density` > `VSHOT_PIN_DENSITY` > 配置文件。`cli` 段里不认识的键会被忽略，不会让整个文件失效——一个键写错只损失那一个键，其余照常生效。

`ocr.engine` 只认 `builtin` 与 `external` 两个值；写了别的名字会**报错**而不是当默认值处理，因为把 `external` 拼错会让人以为自己配的 GPU 引擎生效了。同样，`engine: "external"` 而没有 `command`、或者命令跑不起来，都是明确报错（详见[「用 GPU：外接引擎」](#用-gpu外接引擎)）。设置窗口覆盖 `editor`、常用的 `cli` 项、`ocr.notify` 这个开关，以及 `record` 与 `replay` 两段（各自的编码器、硬件编码器、帧率、portal、麦克风、跟随窗口，回录还有 `window`、`gop`、`save-dir`，两条 `notify`）；`ocr.engine`、`ocr.external` 要手改文件——**两个 `notify` 开关只写「关」**，因为键不存在就是「开」，写一个 `true` 进去等于什么都没说。麦克风那两项是**从当前会话检测出来的**，旁边那个按钮重新检测：检测不到也不报错，只是行里只剩「不录音」和「会话默认输入」两个选项。`follow` 那两项在窗口里是**一行逗号分隔的窗口名**，在文件里是一个数组——手改时写成 `["game", "chat"]`。

`record.follow` / `replay.follow` 只在**不带窗口名、也不给 `--pick`** 的 `record window` / `replay start window` 上生效：那是「跟随」这一概念唯一有意义的地方。其它目标（`monitor`、`all`、`region`，或命令行上点了名的窗口）会**忽略**记着的跟随列表，而不是因为它在而报错——否则配置里记着一个跟随列表，`record monitor` 就再也用不了了。`--no-follow` 是对那一次录制/回录把记着的列表关掉，正如 `--no-mic` 对记着的麦克风那样。

`color` 用的是 CSS 那套写法：`#rrggbb`，带透明度时写 `#rrggbbaa`（alpha 在**最后**）。注意这跟 Qt 自己的八位写法 `#aarrggbb` 不同，`vshot settings` 与配置文件都按 CSS 那套来。

### `dialog`——文件对话框外观

保存与打开窗口（pin 的「另存为…」、编辑器里贴本地图片）是 **layer surface**，合成器不给它们画任何装饰，所以外框和阴影是它们与背后内容之间唯一的东西。这一段描述的就是这两样。

| 键 | 取值 | 默认 |
| --- | --- | --- |
| `radius` | 0–48，逻辑像素 | `12` |
| `borderWidth` | 0–8，逻辑像素；0 完全不画 | `1` |
| `borderColor` | `#rrggbb`；**不写**表示按配色方案推导 | 无 |
| `shadow` | `true` / `false` | `true` |
| `shadowSize` | 0–64，逻辑像素 | `14` |
| `shadowOffset` | -32–32，逻辑像素 | `3` |
| `shadowOpacity` | 0–255 | `120` |

`borderColor` 不写（或删掉）时会按对话框自己的配色推导出一道比底色略深的线，所以在亮/暗主题下都读得出是条边，不用为两套主题各写一个颜色。写了就用你写的。

阴影的四个键与 `pin` 段**是同一套**，含义、范围和默认值都相同（见下一节）。对话框是 layer surface，surface 只能有它自己申请的那个大小，所以阴影画在窗口内部留出的一圈里：窗口比对话框本体大一圈，内容内缩同样多，阴影落在那圈空处。`shadowSize` 因此也决定了对话框本体比窗口小多少——它是「一圈」的宽度，不是叠加在外面的。

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

默认**不圆角但有阴影**：截图是一张窗口的像，圆角会切掉它自己的内容；而一张截图钉在一模一样颜色的窗口上时，没有阴影就完全看不出边界在哪。

阴影的三个数值键：`shadowSize` 是模糊向外扩多远（也就是阴影视觉上的「软硬」），`shadowOffset` 是把整个阴影往下压多少——光从上面来，所以默认 3；填负数它就跑到 pin 上方去。`shadowOpacity` 是阴影的浓度，模糊只是把它摊开、不会加深。`shadow` 是总开关：**关掉不会丢失数值**，所以可以临时关一下再打开，尺寸还在。`shadowSize` 填 0 等于不做模糊，同样什么都不画。

`radius` 是**上限而不是承诺**：真正画的时候会夹到图片短边的一半——再大就不是圆角而是胶囊了，而 pin 的尺寸随滚轮每滚一格都在变，所以这件事只能画的时候算。边框**居中在图像边缘**上，一半在外一半在内，宽度改变时圆角 pin 和直角 pin 的外沿位置一致。

阴影是一次性算好缓存的（在 1/3 尺寸上模糊再放大，软阴影没有细节可丢，而 4K 的 pin 每帧全尺寸模糊比画图本身还贵），拖动时不会重算；缩放或改配置会。**改完配置要生效，下次 `vshot pin` 会重新读**——daemon 在还有 pin 的时候不会退出，但每加一张 pin 都会重读一次文件，所以不用手动重启 daemon。

## 环境变量

| 变量 | 作用 |
| --- | --- |
| `VSHOT_LANG` | 界面与 `--help` 语言：以 `zh` 开头选中文，其它非空值选英文，缺省跟随系统 |
| `VSHOT_QT_HELPER` | 指定 `vshot-qt-ui` 路径 |
| `VSHOT_PIXEL_DEBUG=1` | 窗口像素识别每一级看到了什么 |
| `VSHOT_SESSION_DEBUG=1` | 本次会话被判成了哪个合成器、依据是什么 |
| `VSHOT_LONG_DEBUG_DIR=<dir>` | 长截图落盘每一帧与每次拼接决定 |
| `VSHOT_NVENC_DEVICE=N` | 指定 NVENC 用第 N 个 CUDA 设备（多显卡机器；默认第一个） |
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
- **同一个 `XDG_RUNTIME_DIR` 下可以同时存在多个合成器**，而 `WAYLAND_DISPLAY` 未设置时 libwayland 会用默认的 `wayland-0`（tty 或 ssh 里的 shell 就是这种情况），于是命令可能连到"另一个合成器"上。凡跟合成器有关的失败，vshot 都会额外打印这次连的是哪个 display、本机还有哪些 display；
- **长截图**：选区必须完全落在一块屏幕内；滚动到某处才出现的固定条（浮出的工具条）仍会被当成页面内容，用 `--ignore-top N` 排除；懒加载页面可能重复或缺失少量行；KDE 上帧率明显低于别的合成器（走 D-Bus 抓整屏再裁，且这条路未在现场验证）；抓帧与对齐的耗时随区域面积线性增长，**长截图请用 release 构建**；
- **像素识别**：无缝无边框平铺与完全均匀的桌面没有像素信号，此时如实报错而不是猜；合成器不给焦点窗口描色的边时，`window active --pixel` 答的是"指针下的窗口"；
- **文本**：Qt 文本框接受任意 Unicode（含输入法提交的 CJK）；未携带位图的旧 helper 结果回退到 Rust 内置 5x7 字体，该回退路径仅支持可打印 ASCII；
- **`monitor current`** 依赖 overlay 上收到 pointer enter/motion；通用 Wayland 没有可读取的全局鼠标坐标，因此不会用第一个 output 猜测；
- **回录**：`replay start --portal` 尚不支持（portal 的出帧循环还没接到内存环，会明确拒绝）；`window` 回录受协议按需出帧影响，静止窗口在环里可能长时间只有很少的帧；一次只跑一个会话；
- 原生 screencopy 等待合成器返回帧最多 10 秒，超时返回错误而不是永久阻塞。

## 验证

```sh
cargo fmt --check
cargo test --locked
cargo clippy --locked --all-targets --all-features -- -D warnings
cargo build --release --locked
```

Qt helper 侧没有测试框架，只有**不需要合成器的离屏检查**（默认不构建，加 `-DVSHOT_BUILD_CHECKS=ON`），覆盖配置文件读写与设置窗口、字号换算、剪贴板颜色解析与色卡渲染、pin 的图片自述密度、pin 描边、文字卡片留白、色卡右键菜单、贴图的导出格式：

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

其中 `vshot-paste-check` 盯的是贴图那条**跨语言的文件通道**：helper 把图片写成 `image-N.rgba` 原始 RGBA 并只在 JSON 里留路径，Rust 侧按路径读回来。通道两端是两种语言、两套类型，所以真正值得锁死的是字节本身——通道顺序（直通 alpha 的 RGBA8888，不是 BGRA 也不是预乘）、声明的宽高与文件长度是否一致（Rust 读的时候会按这个对照检查）、以及 JSON 里的 rect 是否就是像素被栅格化时用的那个 rect。任何一条错了在 helper 这边都看不出来，只会在最终 PNG 里表现为颜色错位或图像拉变形。摆位规则也一并测了：比选区大的图等比缩小后居中、小的保持原尺寸、贴完是选中态（把手可用），以及撤销/重做不会把像素弄丢——粘贴是一步历史，撤回来再重做的标注仍然带着自己的像素和 rect。最后一节走的是工具栏：真建出一块区域 overlay（只 create，不 show，layer surface 一次都不碰），走 `beginPresetEdit` 把命令栏立起来，再按 objectName 找到那个「图片」按钮。没有这一节的话，功能只剩 Ctrl+V 可达，而这里什么都不会红。

其中 `vshot-text-size-check` 盯的是字号那一套换算：面板上的数字就是文字像素高，而写进旧协议时投影回 5x7 回退字体能画的整数刻度（`ui/text_size.hpp`）。两端要对齐（7–448 正好是刻度 1–64），整倍数要能往返，中间值要**向最近刻度取整**而不是截断——截断会让 15px 的字在回退渲染下缩成 14px。

其中 `vshot-config-check` 覆盖的是最容易静默出错的一块：保存时**合并写入**是否真的保住了本版不认识的键、清空一个值是否真的把它删掉、以及 `#rrggbbaa` 是否按 CSS 那套解析（Qt 自己会把它读成 `#aarrggbb`，于是「不透明橙色」变成紫色）。`vshot-settings-check` 则把设置窗口真建出来、逐个驱动它的控件，再回读配置文件——某一个字段接错了线，只有这样才看得出来。改窗口布局时这一项尤其值得跑：它靠控件名找控件，所以重排、换控件类都不会漏掉，只有真的把某个字段接错了才会红。

其中 `vshot-pin-outline-check` 把 pin 的**像素**锁死：描边是纯色实心（不是带半透明外沿的双色环）、两种状态共用同一套几何（所以聚焦变化只能换颜色，不能挪位置或改粗细），以及配置能改的那些——圆角真的把角切掉了、4 像素描边正好里外各 2 像素、阴影只画在 pin 之外。阴影那一项两边都测：关掉时 pin 外面什么都不画，打开时下方确实有东西。**这是加阴影时唯一会红的地方**：另外两个 pin 的检查把「pin 外面画的东西」当成右键菜单来测，所以它们显式把阴影关掉——不这么做，一个默认开启的阴影会让那两项以完全无关的理由变红（这次就是这么发现的）。

其中 `vshot-pin-menu-check` 走的是右键菜单那套交互（见[「pin 浮层」](#pin-浮层)）：菜单是从 pin 的坐标算出来的，所以它把「pin 之外被画出来的那一块」当成菜单本身来量——这也是它必须关掉阴影的原因。

另有 5 个默认**不执行**（`#[ignore]`）的集成测试，需要真实环境：KWin 的 D-Bus 采集与后端选择（需要跑着的 KWin，起无头 KWin 即可，见 `src/capture/kwin.rs` 的注释，虚拟输出名 `Virtual-0`、1024x768、无 pointer capability，所以只覆盖到 D-Bus 采集这一层）、活跃输出探针（需要任一真实会话）、`/dev/uinput` 滚动注入（需要写权限）、以及**内置 OCR 引擎读一张画出来的文字**（需要那 30 MB 模型在盘上，`cargo test` 没地方去下）。跑法：

```sh
cargo test -- --ignored              # 全部
cargo test --release ocr:: -- --ignored --nocapture   # 只跑 OCR
```

OCR 那一项默认在源码树的 `models/` 里找模型，也可以用 `VSHOT_OCR_MODELS=<dir>` 指到别处。它验的是单元测试碰不到的那一段：模型能不能加载、整条流水线的方向对不对、字符回来时是不是完整的。文字是用编辑器那套软件渲染器**画**出来的（不是打进去的），所以走的正是截图里文字的路径。

无头 KWin 的启动方式：

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
