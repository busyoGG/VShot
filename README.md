# vshot

**中文** | [English](README.en.md)

> **本项目是纯 vibe coding 项目**：需求由人提，代码与文档由 AI 写。

Rust 写的 Wayland 截图工具，带 Qt 交互界面与常驻 pin 浮层。捕获采用**严格冻结**：先把桌面拍成静态帧，之后所有选择与标注都在那一帧上做，选区期间屏幕上没有任何东西会动。

已适配 Hyprland、niri、KWin/Plasma、Sway，以及任何提供 `wlr-screencopy` 的合成器（如 labwc）的基础截屏。

> **验证程度不一**：Hyprland 全部功能现场实测；niri 的平铺与浮窗截图路径均已实机验证；KWin/Plasma 的 D-Bus 采集、窗口列表与长截图实测过，但 `--cursor`、滚动注入未验证；Sway 与 labwc **没有现场验证**。详见[「合成器适配与验证状态」](#合成器适配与验证状态)。

## 功能

- **区域截图**——冻结桌面后拖选，或直接给固定 geometry；8 向手柄调整，放大镜与尺寸读数跟随指针
- **标注编辑**——矩形、椭圆、箭头、涂鸦、文本、马赛克；撤销/重做、选中移动与缩放、颜色与线型、箭头头型、字号、系统字体选择
- **monitor / all**——按名字或指针位置截一块输出，或把整个桌面按逻辑位置拼合
- **window active / pick**——焦点窗口，或实时桌面上高亮点选；KWin 与 niri 直接交出窗口自己的像素
- **长截图**——框选会滚动的内容，自动发滚轮、逐帧抓取、按内容对齐拼成一张长图
- **pin 浮层**——把图片或剪贴板内容钉在屏幕上：拖动、滚轮缩放、双击关闭、一键显隐、Space 进标注编辑
- **剪贴板贴图**——颜色、图片、复制的图片文件、纯文本（按 HTML / markdown / 代码 / 普通文本渲染成卡片）
- **输出目标**——文件（支持 strftime 路径）、stdout、剪贴板、屏幕 pin，四选一
- **中英双语**——界面与 `--help` 都跟随系统语言

## 安装（Arch Linux）

`PKGBUILD` 把 Rust CLI 和 Qt helper 打进同一个包，一次安装同时提供 `/usr/bin/vshot`、`/usr/bin/vshot-qt-ui`、给 KWin 授权用的 `/usr/share/applications/vshot.desktop`，以及一个带图标的启动入口 `/usr/share/applications/vshot-settings.desktop`（见[「应用菜单入口」](#应用菜单入口)）：

```sh
./scripts/build-arch-package.sh
sudo pacman -U dist/vshot-0.1.1-1-x86_64.pkg.tar.zst
```

脚本把当前工作树（含未提交改动）快照到临时目录再调 `makepkg`，产物写到 `dist/`；也可以直接 `makepkg -si`。运行时依赖 `glibc`、`wayland`（通过 dlopen 使用 `libwayland-client`）、`qt6-base`、`layer-shell-qt`；文件输出、`--clipboard` 与 `vshot pin --clipboard` 需要可选依赖 `wl-clipboard`（写用 `wl-copy`，读用 `wl-paste`）。其它发行版按下面的源码方式构建。

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
- 标注以全局逻辑坐标传回 Rust，最终 PNG 由内置软件渲染重绘，与预览一致；文本由 Qt 按所选字体栅格化为位图后合成，因此字形完全一致

界面语言默认跟随系统（`QLocale::system()`），可用 `VSHOT_LANG` 覆盖：以 `zh` 开头选中文，其它非空值选英文。语言在 helper 启动时确定，切换需重新运行。Rust CLI 的 `--help` 走同一套判定，`VSHOT_LANG=zh vshot --help` 即中文。

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

## pin 浮层

`vshot pin` 把图片作为浮层钉在屏幕上，由**常驻 daemon** 持有：

- **拖拽**移动（可跨显示器，跨屏时另一块屏上的副本同步跟随）
- **滚轮**以图片中心缩放（0.1x–8x），倍率短暂显示在图片右下角
- **双击**关闭该图
- **左键点击**把该图提到最前，重叠时被点到的一定压在其余之上
- 鼠标停在哪张图上，哪张图的描边就是纯黑；其余是浅灰（2 逻辑像素粗，不遮挡图像本身）
- 指针在某张 pin 上且该屏持有键盘时按 **Space** 进入与 `vshot region` 相同的标注编辑器
- 右键点击 pin 出的**色卡**弹出格式菜单，点哪一项就把那个值抄回剪贴板（↑/↓ 选行、回车抄走、Esc 关闭；复制成功后右下角闪一个徽标）

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
- CLI 是瘦客户端，通过 Unix socket 发送单行 JSON 请求；socket 默认 `$XDG_RUNTIME_DIR/vshot-pin-<uid>.sock`，可用 `VSHOT_PIN_SOCKET` 覆盖；
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
| pin 落在哪块屏 | `hyprctl cursorpos` + `monitors -j` | `focused-output`（只跟键盘焦点） | `org.kde.KWin.activeOutputName` | `swaymsg -t get_outputs` | ❌ 无 |
| 滚动注入 | wlr 虚拟指针 | wlr 虚拟指针 | portal / uinput | wlr 虚拟指针 | uinput |

没有适配的地方会**自动降级**而不是报错：没有窗口列表就落到像素识别，问不到指针在哪块屏就落到 Qt 报的主屏。

### 验证到哪一步了

- **Hyprland**——本机会话就是 Hyprland，也是主要开发与验证环境：截图、选区标注、窗口、长截图、pin 与滚动注入都在这里跑过。
- **niri**——平铺与浮窗两条截图路径都已实机验证：平铺窗口拿两个并排 kitty 测（残差 0.45/0.51 每通道），浮窗走 niri 的 `tile_pos_in_workspace_view` 坐标加 `matches_at_position` 验证，实机结果正确。浮窗截图若出现错位，用 `--no-blend` 绕开定位。
- **KWin/Plasma**——D-Bus 采集（`CaptureScreen` 的尺寸与不透明性、`native-resolution`、格式字段）、窗口列表与长截图均已实测，其中采集与窗口列表的自动化测试是在**无头 `--virtual` KWin** 上跑的，因此：
  - `--cursor` 传了 `include-cursor` 但**是否真的画出光标未验证**（无头输出上没有指针可画）；
  - **长截图已验证可用**。KWin 的 `CaptureArea` 是私有 API（参数顺序未核实），所以 KDE 上抓整屏再裁，每帧开销明显高于别的合成器；
  - **滚动注入未验证**。`portal` 走 KWin 自带的 EIS 服务端，但本机 Hyprland 的 portal 不实现 RemoteDesktop，**这条从未在真机上跑通过**，需要 KDE 上验收；`uinput` 依赖 `/dev/uinput` 写权限。
- **Sway**——只有探针实现与单元测试，**没有现场验证**。
- **labwc 等其它提供 wlr-screencopy 的合成器**——理论上基础截屏可用，但没有窗口列表与 pin 落点探针，且**没有现场验证**。只有读像素的操作（`region --geometry`、`monitor <名字>`、`all`）可用；`monitor current` 与交互式选区要指针/键盘 seat，通常也行。
- **GNOME**——不支持，没有实现计划。Mutter 不提供 wlr-screencopy、也不提供 KWin 那套 D-Bus 服务，连 layer-shell 都没有。

### 光标（`--cursor`）

vshot 自己从不画光标，`--cursor` 只是给合成器的捕获请求置一个"叠加指针"标志（`wlr-screencopy` 的 `overlay_cursor`，KWin 是 `include-cursor`），画不画、画在哪、什么时候画，全由合成器决定。由此有两条实测结论：

- **终端里敲命令后截图没有光标，不是 bug**。终端（kitty 实测，`mouse_hide_wait` 默认 3.0 秒）在鼠标不动几秒后会把指针藏掉——对合成器执行 `set_cursor(null)`，此后合成器层面**真的没有光标可画**，任何走 screencopy 的工具都一样（`grim -c` 同时刻也抓不到）。回车后动一下鼠标再去截，或者给 kitty 设 `mouse_hide_wait 0`（打字时指针不再自动隐藏）。其它终端/程序的同类"打字后藏指针"行为同理。
- **niri 把指针画进的是窗口截图，不是输出帧**。niri 上 `window active` / `window pick` 走它自己的 `screenshot-window`，`--cursor` 映射为该调用的 `--show-pointer`，光标画在窗口图里；`--show-pointer` 是 25.11 之后才有的参数，旧版 niri 会拒绝整个请求，vshot 检测到后自动去掉该参数重试并提示"指针画不进窗口"。输出级路径（`monitor`、`all`、`region`）仍是 screencopy 的 `overlay_cursor`。

`--cursor` 对 `long` 无效（见下一条）；KWin 上是否真的画出光标未验证。

### 已知不稳定点

- **`--cursor` 在 `long` 上无效**——`src/longshot.rs` 的抓帧调用把光标参数写死为 `false`（`longshot::run` 的签名里也没有这个形参），所以 `vshot long --cursor` 会被接受但静默忽略。其余捕获路径（`region`、`monitor`、`all`、`window active`）的 `--cursor` 在本机 Hyprland 上实测有效。KWin 上 `include-cursor` 虽然传了，但**是否真的画出光标未验证**。
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

窗口只是普通窗口，不截图、不需要任何合成器协议，所以在一个 vshot 本来截不了图的合成器上也能用。装包后也可以直接从**应用菜单**里的「VShot Settings」打开（见[「应用菜单入口」](#应用菜单入口)）。`cli` 段的数值留空/留 0 表示「不设，用内置默认」，而不是把 0 存进去；`editor` 段则总是整段写出。保存是**合并写入**：本版不认识的键（新版 vshot 写的、或你自己加的）原样保留，不会因为存一次就被抹掉。

窗口分两页，左边栏切换：**标注编辑器**（工具、颜色、线宽、线型、箭头、文本、马赛克）与**命令行默认值**（压缩、默认输出、pin 密度、滚动截图的各项）。每页是一列卡片，一行一项，标签在左、控件在右；两页在默认窗口尺寸下都**不需要滚动**。下拉框与数字框的箭头是自绘的（原生那套是带斜面的老式三角），所以控件外观与工具栏一致。画这些箭头时注意：**`paintEvent` 里 `QPainter` 的坐标系已经是逻辑像素**，Qt 早把输出缩放折进去了，再除一次 `devicePixelRatioF()` 会让坐标在 2 倍屏上全缩小一半、箭头挤到右上角——这个问题在 1 倍屏上完全看不出来，所以两种缩放都值得看一眼。

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
    "pin": { "density": 2 }
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

`pin.density` 的优先级同样是 `--density` > `VSHOT_PIN_DENSITY` > 配置文件。`cli` 段里不认识的键会被忽略，不会让整个文件失效——一个键写错只损失那一个键，其余照常生效。

`color` 用的是 CSS 那套写法：`#rrggbb`，带透明度时写 `#rrggbbaa`（alpha 在**最后**）。注意这跟 Qt 自己的八位写法 `#aarrggbb` 不同，`vshot settings` 与配置文件都按 CSS 那套来。

## 环境变量

| 变量 | 作用 |
| --- | --- |
| `VSHOT_LANG` | 界面与 `--help` 语言：以 `zh` 开头选中文，其它非空值选英文，缺省跟随系统 |
| `VSHOT_QT_HELPER` | 指定 `vshot-qt-ui` 路径 |
| `VSHOT_PIXEL_DEBUG=1` | 窗口像素识别每一级看到了什么 |
| `VSHOT_SESSION_DEBUG=1` | 本次会话被判成了哪个合成器、依据是什么 |
| `VSHOT_LONG_DEBUG_DIR=<dir>` | 长截图落盘每一帧与每次拼接决定 |
| `VSHOT_PIN_SOCKET` | pin daemon 监听的 socket 路径 |
| `VSHOT_PIN_DENSITY=N` | 每张 pin 图的来源密度，等同 `--density` |
| `VSHOT_PIN_DEBUG=1` | daemon 打印每张 pin 的密度判定 |
| `VSHOT_PIN_FOCUS_DEBUG=1` | daemon 打印 pin 渲染面每一次焦点变化 |
| `VSHOT_PIN_SOURCE_FILE` | 覆盖截图工具记录的路径（默认 `/tmp/screenshot-path`） |

> 只要给 daemon 开了任一 `VSHOT_PIN_*_DEBUG`，它就不再把自己的 stderr 丢给 `/dev/null`，踪迹因此可读。变量必须在 daemon 启动时就位；已经在常驻的那个要先 `vshot pin --quit`。

## 已知限制

- **KWin 的冻结 overlay 仍依赖 `zwlr_layer_shell_v1`**（KWin 提供它）。授权不是对话框，只认 desktop file，所以从 `target/` 直接跑的构建在 Plasma 下会一直拿到 `NoAuthorized`；
- **同一个 `XDG_RUNTIME_DIR` 下可以同时存在多个合成器**，而 `WAYLAND_DISPLAY` 未设置时 libwayland 会用默认的 `wayland-0`（tty 或 ssh 里的 shell 就是这种情况），于是命令可能连到"另一个合成器"上。凡跟合成器有关的失败，vshot 都会额外打印这次连的是哪个 display、本机还有哪些 display；
- **长截图**：选区必须完全落在一块屏幕内；滚动到某处才出现的固定条（浮出的工具条）仍会被当成页面内容，用 `--ignore-top N` 排除；懒加载页面可能重复或缺失少量行；KDE 上帧率明显低于别的合成器（走 D-Bus 抓整屏再裁，且这条路未在现场验证）；抓帧与对齐的耗时随区域面积线性增长，**长截图请用 release 构建**；
- **像素识别**：无缝无边框平铺与完全均匀的桌面没有像素信号，此时如实报错而不是猜；合成器不给焦点窗口描色的边时，`window active --pixel` 答的是"指针下的窗口"；
- **文本**：Qt 文本框接受任意 Unicode（含输入法提交的 CJK）；未携带位图的旧 helper 结果回退到 Rust 内置 5x7 字体，该回退路径仅支持可打印 ASCII；
- **`monitor current`** 依赖 overlay 上收到 pointer enter/motion；通用 Wayland 没有可读取的全局鼠标坐标，因此不会用第一个 output 猜测；
- 原生 screencopy 等待合成器返回帧最多 10 秒，超时返回错误而不是永久阻塞。

## 验证

```sh
cargo fmt --check
cargo test --locked
cargo clippy --locked --all-targets --all-features -- -D warnings
cargo build --release --locked
```

Qt helper 侧没有测试框架，只有**不需要合成器的离屏检查**（默认不构建，加 `-DVSHOT_BUILD_CHECKS=ON`），覆盖配置文件读写与设置窗口、字号换算、剪贴板颜色解析与色卡渲染、pin 的图片自述密度、pin 描边、文字卡片留白、色卡右键菜单：

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
```

其中 `vshot-text-size-check` 盯的是字号那一套换算：面板上的数字就是文字像素高，而写进旧协议时投影回 5x7 回退字体能画的整数刻度（`ui/text_size.hpp`）。两端要对齐（7–448 正好是刻度 1–64），整倍数要能往返，中间值要**向最近刻度取整**而不是截断——截断会让 15px 的字在回退渲染下缩成 14px。

其中 `vshot-config-check` 覆盖的是最容易静默出错的一块：保存时**合并写入**是否真的保住了本版不认识的键、清空一个值是否真的把它删掉、以及 `#rrggbbaa` 是否按 CSS 那套解析（Qt 自己会把它读成 `#aarrggbb`，于是「不透明橙色」变成紫色）。`vshot-settings-check` 则把设置窗口真建出来、逐个驱动它的控件，再回读配置文件——某一个字段接错了线，只有这样才看得出来。改窗口布局时这一项尤其值得跑：它靠控件名找控件，所以重排、换控件类都不会漏掉，只有真的把某个字段接错了才会红。

另有 4 个默认**不执行**（`#[ignore]`）的集成测试，需要真实环境：KWin 的 D-Bus 采集与后端选择（需要跑着的 KWin，起无头 KWin 即可，见 `src/capture/kwin.rs` 的注释，虚拟输出名 `Virtual-0`、1024x768、无 pointer capability，所以只覆盖到 D-Bus 采集这一层）、活跃输出探针（需要任一真实会话）、`/dev/uinput` 滚动注入（需要写权限）。跑法：

```sh
cargo test -- --ignored              # 全部
```

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

MIT（见 `LICENSE`）。
