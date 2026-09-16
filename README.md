# vshot

`vshot` 是 Rust 写的 Wayland 截图 CLI，支持 wlroots 系与 KWin/Plasma 两类合成器。非交互截图采用严格冻结流程：先捕获所有输出——wlroots 系走 Rust 原生 `wlr-screencopy-unstable-v1`，KWin/Plasma 走它私有的 `org.kde.KWin.ScreenShot2` D-Bus 服务——再用 Rust 原生 `wlr-layer-shell-unstable-v1` + `wl_shm` 将这些静态帧显示为全屏父 layer overlay。交互式 `region` 由 Qt helper 负责 overlay 和编辑，始终只处理已经捕获的静态帧。

## 安装（Arch Linux）

仓库根目录的 `PKGBUILD` 把 Rust CLI 和 Qt helper 打进同一个包，一次 `pacman -U` 同时提供 `/usr/bin/vshot`、`/usr/bin/vshot-qt-ui` 与给 KWin 授权用的 `/usr/share/applications/vshot.desktop`（见「KDE 授权」）：

```sh
./scripts/build-arch-package.sh
sudo pacman -U dist/vshot-0.1.0-1-x86_64.pkg.tar.zst
```

脚本把当前工作树（含未提交改动）快照到临时目录后调用 `makepkg`，产物写到 `dist/`；也可以直接 `makepkg -si`。运行时依赖 `glibc`、`wayland`（vshot 通过 dlopen 使用 `libwayland-client`）、`qt6-base`、`layer-shell-qt`；文件输出、`--clipboard` 和 `vshot pin --clipboard` 都需要可选依赖 `wl-clipboard`（写剪贴板用 `wl-copy`，读剪贴板用 `wl-paste`）。

其它发行版请按下面的源码方式自行构建。

## 构建

Rust 后端：

```sh
cargo build --release --locked
```

Qt 交互 helper：

```sh
cmake -S . -B build-qt -DCMAKE_BUILD_TYPE=Release
cmake --build build-qt --parallel
```

运行交互区域截图时，helper 按以下顺序查找：`VSHOT_QT_HELPER` 环境变量、`vshot` 可执行文件同目录、可执行文件相对的 `../build-qt/` 和 `../../build-qt/`（覆盖 `cargo build` + `cmake -B build-qt` 的开发布局）、最后是 `PATH`。也可以显式指定：

```sh
VSHOT_QT_HELPER="$PWD/build-qt/vshot-qt-ui" target/release/vshot region --output shot.png
```

Qt 交互界面支持中/英双语：默认跟随系统语言（`QLocale::system()`，中文系统显示中文，其余显示英文），可用 `VSHOT_LANG` 覆盖（以 `zh` 开头 → 中文，其它非空值 → 英文），例如 `VSHOT_LANG=zh target/release/vshot region ...`。语言在 helper 启动时确定，切换需重新运行。Rust CLI 的 `--help` 与错误信息保持英文。

运行时需要：

- Wayland 会话；
- `wl_compositor`、`wl_shm`（包含 `XRGB8888` 或 `ARGB8888`）、至少一个 `wl_output`；
- `zxdg_output_manager_v1`，用于 overlay 的输出名称和 logical topology；
- `zwlr_layer_shell_v1`，用于 Rust 非交互冻结 overlay；
- `zwlr_screencopy_manager_v1`（版本 1 至 3），用于 wlroots 系的原生帧捕获——**KWin 没有这个协议**，此时改用 KWin 的 D-Bus 截图服务（见下）；
- `wl_output` 的名称事件（版本 4）用于按名称选择捕获输出；
- `vshot-qt-ui` 需要 Qt6 Core/Gui/Widgets/Network 和 LayerShellQt；
- 可选 `wp_cursor_shape_manager_v1`（仅保留的 Rust editor path 使用）；
- 使用交互式 `region` 时需要可执行的 `vshot-qt-ui`，也可通过 `VSHOT_QT_HELPER` 指定；
- 文件输出、`--clipboard` 和 `vshot pin --clipboard` 需要可执行的 `wl-copy`（写剪贴板）与 `wl-paste`（读剪贴板，只有 `vshot pin --clipboard` 用）。

**seat 只在真正读它的时候才要求**。只读像素的操作——`monitor <名字>`、`all`、`window active`、`region --geometry`——不碰 seat，因此一个只有 keyboard 的 seat 也照样能截图：KWin 6.7.5 的嵌套与 `--virtual` 会话对外部客户端就只 advertise keyboard（实测，两处都是），而它的截图路径本来也不需要指针。要读 seat 的是两类操作：`monitor current`（指针在哪块屏）和交互式框选（指针画、键盘收尾），缺哪个能力就以非零状态退出并指名是 `seat pointer` 还是 `seat keyboard`，不会假称截图已经冻结。

## 用法

```sh
# 冻结桌面后在静态帧上拖拽选择
vshot region --output shot.png
vshot region --clipboard

# 在冻结场景上裁剪固定的 global geometry
vshot region --geometry '100,200 800x600' --output shot.png

# 指定输出；所有输出仍会先被捕获并显示冻结 overlay
vshot monitor eDP-1 --output shot.png

# 鼠标所在输出。位置来自冻结 overlay 的 pointer enter/motion，绝不默认第一个输出
vshot monitor current --clipboard

# 合成完整桌面（负坐标和输出之间的空隙保留为透明像素）
vshot all --output desktop.png

# 捕获 active window
vshot window active --output window.png

# 在实时桌面上挑选一个窗口：悬停高亮；点击后重新抓帧并进入编辑，其余与区域截图一致
vshot window pick --output window.png
vshot window pick --clipboard
vshot window pick --pixel        # 跳过 compositor 窗口列表，用像素识别找候选

# stdout 输出纯 PNG bytes；日志只写 stderr
vshot all --output - > desktop.png

# 截图直接 pin 到屏幕（不落盘）
vshot region --pin

# 长截图：框选一块会滚动的内容，vshot 自动滚动并拼成一张长图
vshot long --output long.png
vshot long --geometry '100,200 900x700' --output long.png   # 固定区域，不做交互选择
vshot long --ignore-top 48 --clipboard   # 顶部 48 行是滚动中才出现的固定条时，强制忽略

# pin 管理：添加图片、显隐、清空、退出 daemon
vshot pin shot.png another.png
vshot pin --clipboard        # pin 剪贴板里的图片（或复制的图片文件/路径/文字）
vshot pin --density 2 shot.png   # 手动指定来源屏倍率（见下文，可选兜底）
vshot pin --toggle          # 一键显示/隐藏所有 pin
vshot pin --hide / --show
vshot pin --close-all       # 关闭全部 pin（无 pin 后 daemon 自动退出）
vshot pin --list            # 打印数量与可见状态
vshot pin --quit            # 退出 daemon
```

`region` 的 `--geometry` 与 `--interactive` 互斥；未给出 geometry 时默认进入交互选择。输出 destination 必须且只能是 `--output PATH`、`--output -`、`--clipboard` 或 `--pin` 之一。`--cursor` 会请求 screencopy compositor 将光标合成到每个输出帧。

`--output PATH` 支持 `strftime` 时间格式，前缀和后缀可以任意组合。截图写入后，vshot 会把生成文件的绝对 `file://` URI 复制到剪贴板；这与 `--clipboard` 的 `image/png` 图像数据剪贴板不同。例如：

```sh
vshot region --output "$HOME/Pictures/vshot-%Y-%m-%d_%H-%M-%S.png"
vshot all --output 'shots/capture-%Y%m%d-%H%M%S.final.png'
```

其中 `%Y`、`%m`、`%d`、`%H`、`%M`、`%S` 分别表示年、月、日、时、分、秒，`%%` 表示字面 `%`。文件输出和剪贴板输出都需要 `wl-copy`。

`--png-compression LEVEL` 控制写文件、stdout 和剪贴板的 PNG 压缩等级，取值 `none`、`fastest`、`fast`（默认）、`balanced`、`high`。所有等级都是无损的，区别只在耗时与体积：`fast`/`fastest` 走 fdeflate，4K 帧只要几十毫秒；`balanced`（DEFLATE level 6，多数 PNG 工具的默认档）慢一个数量级，换来约四分之一更小的文件；`none` 完全不压缩，文件最大但几乎不花时间。`--pin` 不落盘，PNG 只经临时文件送到 daemon，因此该参数对 `--pin` 无效。

## 交互式 overlay

`vshot region`（未给出 `--geometry` 时）冻结桌面后交给 `vshot-qt-ui` 全屏 layer overlay，交互流程参考 HyprCapture：

- 冻结画面铺满每个输出；选区之外覆盖半透明暗色遮罩（该 surface 被冻结帧填满、每像素不透明，所以合成器的 layer blur 规则不会透过它把桌面糊掉）；
- 拖拽画出矩形选区；选区四周出现 8 个方向手柄，拖动边缘/角落调整大小，拖动选区内部移动位置，方向键微调（Shift 加速为 10 逻辑像素）；
- 拖拽或调整选区时，光标旁显示放大镜（光标处 8x 像素放大和原生像素坐标），选区左上角显示 `宽 × 高` 尺寸指示；
- Enter、双击选区或工具栏 OK 确认；Esc 或右键取消整次截图；文本框内的 Esc 只关闭文本框；
- 工具栏是一个磨砂超椭圆（squircle，指数 5）浮动面板，背景为冻结画面的实时模糊采样：第一行为工具（Select、Rect、Ellipse、Arrow、Draw、Text、Mosaic）与 Undo、Redo、OK、Cancel；样式子面板按当前工具显隐（7 个颜色色板加自定义颜色选择器——面板内 HSV 渐变与十六进制输入，点外部或 Cancel 关闭、OK 应用、线型 Solid/Dash/Dot、箭头头型 Open V/Filled、粗细有界滑块 1-64、箭头大小有界滑块 1-8、字体大小数字输入 1-64、马赛克形状 Rect/Ellip/Brush、马赛克程度有界滑块 1-3），并始终弹在命令栏背向选区的一侧（面板在选区上方时子面板向上展开，面板靠下时向下展开），展开与收起不会移动命令栏位置。面板默认跟随选区（居中悬浮在选区上方，空间不足时翻到下方，选区跨屏移动或调整时跟随到对应输出），按住面板空白处拖动可固定到任意位置，拖到另一屏幕上释放也会落到该屏；
- Arrow 是直线箭头（按下点到释放点），头部支持 Open V 或 Filled 两种样式，大小由 1-8 有界滑块调整且始终为实线；Draw 是自由绘制；两者都支持虚线/点线线型；
- Mosaic 提供三种形态：矩形、椭圆和自由涂抹（Brush，涂抹半径随粗细和程度变化）；程度 1-3 控制像素块大小（细/标准/粗）与涂抹半径（0.5×/1×/2×），选中已有马赛克后可直接改程度；预览与最终输出一致；画完的马赛克可用 Select 工具选中后拖把手调整大小，大小变化实时反映到像素化区域；
- Select 工具可点选任意标注：单击先选中，拖动移动（文本也一样），形状/线条/马赛克可拖 8 个把手缩放，Delete/Backspace 删除选中的标注；样式区的修改会即时应用到选中的标注；双击文本标注重新编辑内容；
- Text 工具点击放置文本框，点击已有文本可重新编辑；标注颜色、线宽、线型、箭头头型/大小和字号都会传入最终渲染，与预览一致；样式区提供系统字体列表选择（`QFontDatabase` 枚举，每项按自身字形预览），选中的字体即时应用到文本框、overlay 预览和选中的文本标注，并纳入撤销/重做；
- Ctrl+Z / Ctrl+Y（或 Ctrl+Shift+Z）撤销/重做标注，重新编辑文本后撤销会恢复原文；
- 标注以全局逻辑坐标加 `#RRGGBB` 颜色、逻辑粗细、线型（`dash`）、箭头头型（`arrow_style`）和大小（`size`）、马赛克形状（`mask`）和程度（`strength`）传回 Rust，最终 PNG 由内置软件渲染重绘，与 overlay 预览一致。文本标注额外携带字体名（`font`）和 Qt 按场景最高输出 scale 栅格化的 RGBA 标签位图（`bitmap_width`/`bitmap_height`/`bitmap` 指向 session 临时目录中的 raw 文件）；Rust 直接合成该位图，因此最终 PNG 的字形与 overlay 预览完全一致。旧 helper 未携带位图时回退到内置 5x7 ASCII 字体渲染。

## active window

通用 Wayland 没有标准的 active-window geometry API。`window active` 依次尝试：

**只问当前会话自己的合成器**：`XDG_CURRENT_DESKTOP` / `XDG_SESSION_DESKTOP` 里的名字决定本会话是谁，不属于本会话的探针一律不跑（两个变量都没写合成器名时才把下面几路都试一遍）。同时跑着两个合成器时这不是可选项——从 Hyprland 终端里启动的 Plasma 会话会继承 `HYPRLAND_INSTANCE_SIGNATURE`，`hyprctl` 于是照样应答，报的是**没人看的那个 Hyprland 实例**，于是 `window active` 截的是 Hyprland 里的窗口、`window pick` 把那个会话的窗口列表当成 KDE 桌面上的候选。niri 既没有 Hyprland/Sway 那样的矩形探针，也没有输出焦点窗口的几何，它走的同样是第 1 路里"合成器自己画窗口"那条（见下）。

1. **合成器自己画这个窗口**——唯一"直接给出答案"的一路，也是少数不经过任何裁剪的一路：合成器把焦点窗口自己画一遍，按窗口所在屏的 scale 给出原生像素。两条实现：
   - **KWin**（`org.kde.KWin.ScreenShot2` 的 `CaptureActiveWindow`）：带装饰和阴影（阴影那一圈的底色是透明的），回复里的 `scale` 就是密度，`windowId` 是窗口 uuid（见「截图后端」）。它是单次 D-Bus 调用、不依赖任何外部工具，所以排在 KDE 的最前面；代价是 `--cursor` 由 KWin 自己决定画不画；
   - **niri**（`niri msg action screenshot-window --id N --path <绝对路径>`）：niri 自己把该窗口渲染成 PNG 写到我们给的临时路径，再读回来。它**不能改用矩形裁剪**：niri 的 IPC 里平铺窗口没有绝对位置——`niri msg --json windows` 每扇窗只给 `pos_in_scrolling_layout`（1-based 的列/瓦片**索引**）、`tile_size`、`window_size`、`window_offset_in_tile`，而唯一的位置字段 `tile_pos_in_workspace_view` 只对**浮动**窗口有值（平铺路径在 niri 源码里被显式置为 `null`，`Workspace` 里也没有滚动视图偏移），所以"算出矩形再裁冻结场景"在 niri 上只对浮动窗口成立。详见下节；
2. Hyprland：`hyprctl activewindow -j` 的 `at`/`size`；
3. Sway：`swaymsg -t get_tree` 中递归查找 focused node 的 `rect`；
4. KDE Plasma 的兜底（上面那路不可用时才走），两条路，先试能试的那条：`kdotool`（装了就用）——它驱动的是同一套 KWin scripting 接口，但结果通过 `callDBus` 回给自己那个临时总线名，**不经 journal**，所以是本条路上唯一不依赖 KWin 日志的走法；读它 `getwindowgeometry` 的 `Position:`/`Geometry:` 两行——kdotool 0.2.1 只为 `getmouselocation` 提供 `--shell`，对 `getwindowgeometry` 传 `--shell` 会当场报 `invalid option` 并失败（这正是它早先形同不存在的原因）；没装或读不出几何时退回一次性 KWin scripting 探针——通过 `org.kde.kwin.Scripting`（gdbus/dbus-send）加载读取 `workspace.activeWindow`/`activeClient` 的 `frameGeometry`（分别对应 Plasma 6/5），再从用户 journal 轮询标记行取回；探针每次独立加载并在结束后卸载。**这条探针依赖 journald 收到 KWin 的 `console.info`**：KWin 从 tty 起、日志只进那台 tty 时，无论等多久都取不到行——这正是本机 KDE 上 `window active` 曾经落到像素识别的第二个原因；
5. **像素识别兜底**：以上都不可用时，在已捕获的场景帧上自动检测窗口。分析**逐个输出进行，用该输出自己那份原生像素**，不在合成场景上做——场景会把低 scale 的输出放大，从场景边缘出发的泛洪还会跨过显示器接缝，把"整块桌面"当成一个候选。每个输出内部的候选按可信度分四级，高一级有结果就只用这一级：`Ring`（边框带）→ `Segment`（泛洪分割）→ `Outline`（闭合描边轮廓）→ `WholeOutput`（整块输出，只在该输出基本均匀时）。**Ring 优先**，但只认**有颜色**的那条边：合成器给焦点窗口描的边是画面里唯一明确指向"焦点窗口"的信号。它找"薄而恒定的横带"（两侧跳变 ≥ 20、内部变化 < 10、厚度 ≤ 8 设备像素），把这些带拼成长线，要求**上下两条横线各自找到的左右竖线完全一致**、四条边闭合成环，环的高度/宽度还要够窗口尺寸；两条横线各自去找角点是为了排除"两窗共享同一行边框"的假环——那种情况下横线会一直延伸过邻居的边界。侧边还必须是**一条完整的边**（跨度 ≥ 环高的 60%）：实测一个 kitty 窗口的左右边框各占环高的 95%/96%，而它内部的一条滚动条只占 13%，曾经把那条滚动条当成右边框，让裁剪少了 14 个逻辑像素。环上采到的平均饱和度 ≥ 48 才算"焦点描边"，**灰色的环只按 `Segment` 那级参与竞争**（它可能是 `col.inactive_border`，也可能是壁纸里随便一个方框，两者在像素上无法区分），免得闲置屏上的一个壁纸方框压过指针所在屏上的真窗口。**每条路径给出的都是窗口本来的样子，包含合成器画的那条边框**：`Ring` 取环带的外沿，`Segment` 也不再往里缩掉四周的同色边带（那一步曾被用来和 compositor 元数据对齐，现在裁剪以"画面里看到的窗口"为准）。`Segment` 是原路径（从输出边缘泛洪追踪壁纸与阴影，无边框窗口靠 gaps、阴影或壁纸分离）；`Outline` 是"闭合同色矩形轮廓"，且必须覆盖该输出的足够比例才算窗口——网页内容里到处都是同色矩形，不设这道门槛就会截到某人页面中的一块卡片。一个输出最多保留 32 个环、每个环的饱和度只沿四条边各取 64 个采样点：整个帧本身就是一张网格状图片时（屏幕的照片、满屏嵌套面板），边线两两配对能凑出上千个矩形，这两道闸把开销和候选数都压成常数。

`vshot window active --pixel` 跳过上面这些"直接问合成器"的路，直接在捕获的帧上做像素识别：用于测试检测器，也是拿到一个矩形而不是合成器给的像素时唯一的选择。niri 上它同时是退路——`--pixel` 会放弃 niri 自己的窗口截图，改用识别出来的矩形去裁场景（这也意味着它在 niri 上要求输出能被合成成场景）。分析帧按**面积**上限降采样（1080p 逐像素、4K 约 1/2），因为分隔窗口的 gaps 与描边只有几个设备像素宽：按长边压到 1024 会让 4K 场景里的 3px 边框整条消失，实测焦点描边检测在那样的分析帧上一个候选都给不出来。窗口截图**按所在输出原生裁剪**（`SceneSnapshot::crop_output_region`）：混合 DPI 时一块 scale-1 屏上的窗口若从合成场景裁剪，会得到放大一倍且发虚的图，现在直接从那块屏自己的帧裁，PNG 写的密度也是那块屏的；只有跨接缝的窗口才回落到合成场景。像素识别在 release 下两屏共约 0.1 s。**没有焦点描边时按指针判**：合成器若给焦点窗口描一条有颜色的边（Hyprland 的 `col.active_border` 甚至可以是渐变），那条边就是判据；若它描的是纯灰（本机焦点在无边框全屏窗口上时实测四周都是 `#464646` 灰边，平均饱和度 0.8，而有色焦点描边实测 162~175），画面里就没有任何东西能区分"焦点窗口"与"指针下的窗口"，候选按「指针所在输出 → 指针命中 → 面积」排序，给出的是**你指着的那个窗口**，与 `hyprctl activewindow` 可能不一致。`VSHOT_PIXEL_DEBUG=1` 把每个输出的分析尺寸、找到的每个环（含饱和度、是否判为焦点描边）以及每一级的答案打到 stderr，用来查一次错误裁剪到底是哪一级给出来的。**无缝无边框平铺（无 gaps、无阴影）没有任何像素信号**，此时如实报错而不是给出错误裁剪。在保存下来的那张 4K 帧（3830x2156 设备像素，scale 2，右边半屏是焦点 kitty）上实测：边框带把 kitty 精确读成逻辑 (962, 54) 起的 **941x1014** 内容矩形，它的边框是 3 逻辑像素宽，于是裁剪用的矩形是 (959, 51) 起的 947x1020 —— 窗口连同边框；同一帧上泛洪分割给出的却是把并排两窗连成一片的 927..1914 宽一整块，而这条路径早先给出的是 3832x1072——两块屏拼起来的整个桌面。KWin 探针依赖 `journalctl` 与 `gdbus`/`dbus-send` 之一（Plasma 环境均具备），且需要 journald 记录 KWin 的脚本日志；不可用时自动落到像素识别。

第 1 路不经过任何裁剪：像素是合成器直接给的窗口本身，因此跨接缝的窗口也能整块到手（合成场景那条路会把低 scale 的那半放大）。其余各路的目标 geometry 从已经捕获的冻结画面裁剪——窗口路径优先从**所在输出自己那份帧**裁剪（原生分辨率与原生密度，见上），只有跨输出的 geometry 才用合成场景；overlay 显示后不会重新访问 compositor。其他 compositor 的 Portal active-window backend 尚未实现。

### niri：为什么走它自己的截图，以及这条路给出什么

依据是上游源码（`YaLTeR/niri`；本文实测用的会话是 25.11 之后 236 个提交的一个本地构建，仅作旁证）：

- **平铺窗口没有绝对几何**，这不是"没找到"：`niri-ipc` 的 `WindowLayout` 只有 `tile_pos_in_workspace_view: Option<(f64,f64)>` 是"位置"，而它在**浮动**窗口上才被填（`src/layout/floating.rs`），平铺路径显式给 `None`（`src/layout/tile.rs`，`src/layout/scrolling.rs` 只补 `pos_in_scrolling_layout` 那对 1-based 索引）。所以 niri 上的窗口捕获**必须**走 `screenshot-window`，没有第二条路；
- `window active` 先 `niri msg --json focused-window` 拿焦点窗口（layer-shell 表面持有焦点时是 `null`，此时按"没有焦点窗口"处理并落到后面的路），再让 niri 截它；
- `window pick` 用 niri 自己的挑窗：`niri msg --json pick-window`，它只把光标换成十字、点哪个窗口就返回哪个（源码里不绘制悬停高亮），所以在 niri 上**没有我们自绘的压暗 overlay，也没有点击后的标注编辑器**（编辑器需要一个矩形来裁场景，而 niri 给不出平铺窗口的矩形）。要回到 overlay + 像素识别那条路就加 `--pixel`；
- **`--cursor` 是版本相关的**：`Action::ScreenshotWindow` 的 `show_pointer` 字段在上游较新的版本里才有；niri 侧一旦以"未知参数"拒绝，vshot 就退化成不带指针重发一次并在 stderr 说明，而不是把整次截图丢掉；
- **niri 会同时把这张图写进剪贴板**：`save_screenshot` 里设置剪贴板是必走的（与 `--write-to-disk` 无关），关不掉。所以 `--output`/`--clipboard` 下最终剪贴板是 vshot 自己的内容（文件 URI / PNG），而 `--pin` 下不写剪贴板，用户的剪贴板会被 niri 这次截图占据；
- **密度不由这张图声明**：niri 写出的 PNG 只有 `IHDR`/`IDAT`/`IEND`，**没有 `pHYs`**（实测），所以密度由 vshot 自己写。规则是"该窗口所在输出的 scale"，优先取**我们拓扑里那台输出的整数 scale**（同一台屏上 `monitor`/`region`/`window` 写出的密度因此一致），拓扑不可用时退回 niri 的 `logical.scale`（小数四舍五入，与 KWin 那条路同一处理）；两者都拿不到就报错，不假设 1；
- 由于这条路不需要拓扑、也不需要场景，**输出映射的校验被移到了"真正要用拓扑的地方"**：`WaylandSession::connect()` 不再提前校验，`window active`/`window pick` 因此在旋转/翻转输出上也能用（要求场景的 `region`/`monitor`/`all`/`long` 仍然照旧报 `unsupported output mapping`）。

实测（隔离 labwc 里的嵌套 niri，1278x692 输出、scale 1、一个 kitty 平铺窗口）：

| 量 | 值 |
| --- | --- |
| `window_size`（内容） | 615x660（边框 off，lab 现在就是这档）／607x652（把 4px 边框打开时） |
| `tile_size`（含边框） | 615x660（边框 off 时与 `window_size` 相同） |
| niri 写出的 PNG | **639x684** = `window_size` + 每边 12px（开 4px 边框那次是 631x676 = 607+24，加的仍是 `window_size`） |
| 那 12px 是什么 | 窗口的**投影**：纯黑、alpha 沿边由 5 渐变到 86；**边框不在图里**（开了 4px 边框后 PNG 仍是 `window_size`+24，而不是 `tile_size`+24） |
| PNG 里的块 | 只有 `IHDR`/`IDAT`/`IEND`，无 `pHYs` |
| `screenshot-window` 之后的剪贴板 | `wl-paste --list-types` → `image/png` |
| `vshot window active` / `window pick` | 均 exit 0，得到同一张 639x684；`--pick` 用 niri 十字选窗 + 真实点击选中该窗口 |
| 写出的 PNG | 639x684、带 `pHYs` 96 DPI（密度 1，来自 niri 的 `logical.scale`——lab 里拓扑不可用，正好走了回退那条） |

## 选择窗口

`vshot window pick` 分两步：先在**实时桌面**上挑窗口——**移动指针**高亮指针下的窗口，其余部分**压暗**，左上角的提示条给出窗口标题与将要截取的尺寸；**左键点击**结束挑选阶段（点在没有窗口的位置不选中任何东西，Esc 取消）。然后程序**重新捕获一帧**、把点击位置在**当前的窗口列表**上重新解析成窗口矩形，并以那一帧开一个编辑会话（工具栏、标注、Enter 确认、Esc 取消都与区域截图一致）。所以挑选期间切换工作区、移动窗口都不会让结果停在旧的画面上：裁剪用的是点击那一刻的画面，标注也画在同一帧上。（niri 上这段流程不同：见「active window · niri」与下面的说明。）

压暗就是一层半透明黑罩（alpha 80，和编辑阶段选区内外的压暗同一档）。Hyprland 如果给所有 layer namespace 打开了 blur（本机配置就是 `namespace = ".*"` + `blur = true`），这层罩子会让合成器把底下的桌面一起模糊掉——观感上就是"没选中的窗口失焦"（本机实测压暗区的高频能量只剩基线的 0.4%）。overlay 的 layer namespace 是 `vshot-qt-ui`，给它单独关掉 blur 就能得到干净的压暗。

挑窗口阶段本身不画冻结帧（桌面照常更新，压暗靠那层黑罩），而且点击时会把提示描边和罩子先撤下屏幕（合成器是异步销毁这些 surface 的，留着就会被打进紧随其后的那一帧）。点击到编辑会话出现之间有一段重新捕获的等待（200 ms 上限，实测只是一帧量级），这段时间里屏幕回到实时桌面。

挑选期间候选列表**跟着指针刷新**：指针每移动一次（150 ms 内最多一次）helper 就通过 session 的管道向 CLI 要一份新的窗口列表，CLI 现查 compositor 后回答；指针完全不动时也每 300 ms 问一次，免得别处（另一个屏幕切换工作区、窗口被挪走）的变化让高亮停在旧位置上。CLI 没有可复查的来源时（纯像素识别那条路径）会回答「无可奉告」，helper 就继续用它手里那份；回答和手里那份一样时不会重绘。

候选来自 compositor 的窗口列表，依次尝试（**同样只问当前会话自己的合成器**，判定见「active window」）：

**niri 是例外**：它没有窗口列表可用（见「active window · niri」），所以 `--pixel` 之外它在挑选阶段不画压暗 overlay，而是直接调 niri 自己的十字挑窗（`niri msg --json pick-window`）——点到的窗口由 niri 自己截图交回，因此这里没有"重新解析点击位置"这一步，也没有后面的标注编辑器。`--pixel` 时回到下面这套 overlay + 像素识别。

1. Hyprland：把 `hyprctl clients -j` 和 `hyprctl monitors -j` 一起看。Hyprland 会列出**所有**工作区的客户端，而且 `visible` 对隐藏工作区的窗口同样为真、其 `at` 还是上次布局留下的过期值（实测：一个 12 窗口的会话里真正在屏幕上的只有 2 个），所以这里不靠标志位而是按结构筛选——客户端的工作区必须正是它所在显示器当前显示的那个（激活工作区，或已激活的 special workspace；pinned 窗口跨工作区常驻，始终保留），且矩形必须落在该显示器逻辑范围内（过期坐标通常指向另一块屏，正是被这条挡下的）。标题用 `class — title`；候选按叠放序输出（平铺 → 浮动 → pinned 浮动）；
2. Sway：`swaymsg -t get_tree` 的全部叶子节点（隐藏 workspace 下的除外），标题用 `app_id`（X11 用 `window_properties.class`）加 `name`；浮动的容器排在平铺叶子**之后**（sway 同样是先命中浮动容器、并把它画在平铺之上）；
3. KDE Plasma：同一套 KWin scripting 探针的 `list` 模式，遍历 `workspace.stackingOrder`（**自底向顶**，与 KWin 自己的命中测试 `windowAt` 从叠放序末尾往前扫同序；KWin 太老而没有这个属性时退回 `windowList()`/`clientList()`，那是创建顺序，不是叠放序），跳过 `deleted`/`hidden`/`minimized` 与非 `normalWindow` 的项；探针只报几何、不带标题，所以提示条只显示尺寸；

指针命中的候选取**列表里最后一个包含指针的窗口**——候选按**叠放顺序自底向顶**排列，所以那就是指针下最上层的窗口。**这条规则不是"取包含指针的最小矩形"**：压在平铺窗口上的浮动窗口通常更小，但不是一定，按最小取就会选中它下面的那个（本机实测：对一个 941×1014 的平铺窗口 `togglefloating`，体积不变、面积相同，旧规则退化成"谁在 `hyprctl clients` 的数组里靠前"，于是随机取到下面那个窗口）。排列按各自合成器自己的命中顺序来：Hyprland 按它绘制与命中测试都用的三个 pass（平铺 → 浮动 → pinned 浮动，pass 内保持 `hyprctl clients` 的数组顺序，那是它真实的叠放序：窗口向量自底向顶，聚焦时被 `moveToZ` 提到末尾）、Sway 的浮动容器排在平铺树之后、KWin 探针报 `workspace.stackingOrder`（自底向顶，与 KWin 自己的 `windowAt` 从末尾往前扫同序）。点击后的重新解析（`window_at`）用同一条规则，所以高亮与最终截到的窗口不会是不一致的两个。像素识别的候选按面积**从大到小**排，"最后一个命中"于是仍然是最小的那个。窗口列表筛完之后为空（或查询本身失败）时会自动落到像素识别，不会直接报错。`vshot window pick --pixel` 跳过窗口列表，直接在冻结帧上做像素识别（与 `window active --pixel` 同一套逐输出、原生分辨率的分析：边框带、泛洪分割、闭合描边轮廓一起上，把所有候选交给用户），把找到的所有候选交给用户挑；这条路径用于没有窗口列表查询的 compositor，也用于测试检测器。纯像素识别只能看到帧里**可分离**的窗口：无缝无边框平铺（无 gaps、无阴影）以及完全均匀的桌面没有像素信号，此时候选为空并如实报错，不会给出一个猜出来的裁剪。点击后的重新解析同样用窗口列表（`--pixel` 或列表不可用时退回挑选阶段给出的矩形），所以 `--pixel` 这条路径在工作区切换后可能仍停在旧矩形上。

## 长截图（滚动截图）

`vshot long` 把一块**会滚动的内容**拼成一张长图：框选区域后，vshot 自己按节拍发滚轮、连续抓帧、按内容对齐、把新增的部分接到长图底部，直到页面到底、达到上限或用户结束。结束时按既有输出路径写出（`--output` / `--clipboard` / `--pin`）；结果**不经过标注编辑器**，想标注就先 pin 一下再用 pin 的编辑功能。

流程：

1. 抓一帧冻结桌面并框选（与区域截图同一套选区交互，但确定选区后**不会**进入编辑工具栏——那一刻还没有可标注的像素）。`--geometry` 可直接给定区域，跳过选择。
2. 屏幕角落出现一个提示条，报告已拼接的高度与帧数，并接收 **Enter**（保留当前结果）和 **Esc**（丢弃）。
3. 循环：**滚轮按固定节拍发**（默认每 120 ms 一批，`--notches` 决定每批发几档），**抓帧不等画面停下**（按约 50 fps 的上限连拍），每帧与**上一帧**对齐，位移大于 0 就把新增行接到长图底部。滚轮量从第一帧到最后一帧**不变**——一档滚轮在一张页面里走多远是应用自己的事，测量结果只用来决定要不要缩小，不用来改速度。应用本身会给滚轮做动画，所以抓到的多半是**正在移动中的画面**：相邻两帧重叠很大，位移小到几十像素，这正是对齐最可靠的情形。
4. 停止条件：**连续 6 次滚轮都没有产生任何位移**（到底）、`--max-height`、`--max-frames`、`--timeout`，或用户按键。判据是"滚了几次没动"而不是"多久没动"：大区域匹配一帧要花掉大半秒，用时间窗会在页面还没开始动的时候就到期。单帧没动不会收工：懒加载卡顿、应用的动画还没启动、指针被移出选区（滚轮发给了别的窗口）看起来和"到底"一模一样。另一种例外是**一帧的位移超出能测的范围**（对不上），此时把滚轮对半缩小（下限 1 档）；连续多次仍对不上，说明一档都太快，才停止并如实报告。提示条上的原因会停留一下再消失，不会一闪而过。

对齐只取帧里**属于页面**的行：顶部与底部连续不动的行（标题栏、固定工具栏、状态栏）是 chrome，不进探针——它们本来就在原地，拿它们对齐会在偏移 0 处得到完美匹配，把真实滚动读成"没动"。这些行也不会每帧重复：顶部 chrome 只出现在长图最上面，底部 chrome 只出现在最下面，中间的帧只贡献新出现的页面行。一帧要被当成「在滚动」，它**变化的行数得够组成一个探针**，而不是占视口的某个比例：页面滚到最后一行以下时（编辑器的 scrollBeyondLastLine、页脚之后的大片留白），视口里大半是背景，页面自身的空白行也不随滚动变化，按比例衡量就会把"还在滚"读成"没动"，而丢掉的正是该追加的那几行。探针取页内最上面的 `min(96, 视口高/4)` 行，在上一帧里做灰度 SAD 全搜索，逐行早停剪枝。**探针按长度依次询问，第一个能看全这次滚动的说了算**：更短的探针（一半、四分之一，窗口更大）只在它顶到窗口尽头时才接手，此后只能否决——如果它把页面放到了别处，那多半是巧合，宁可这一帧不采纳。反之，最长的、看得全的探针**明确找不到对齐**时，还不能断言页面没动：页面在几百行以下还有一模一样的一段内容时（表格、聊天记录、一串相同的列表项），比那段重复更短的探针会把两处匹配得一样好，看着就是歧义。这时用一次**更长的探针复核**（至多两倍长，并止于帧底之前，好把搜索窗口留给被复核的那个位移），跨过重复之后两处就分开了；复核仍不明确，才断言页面没动——一帧只在原地重绘（光标、动画、视频）时，几十行的短探针很容易在远处凑出一个比真相还低的匹配分，信了它就会往长图里塞进几百行从没出现过的内容。每帧只与**上一帧**比较，不与已拼好的长图比较，所以误差不会累积。

新增行是**从页面底边往上数**的，而这条边**跨帧保留、只降不升**：页面到哪一行为止是窗口给的（底栏多高在一次滚动里不变），而一帧底部有一段内容与上方重复时（大片同色、缓慢渐变的背景、空白的尾部），那段行会被读成"没变化"、把边读高，从那条边往上数出的"新增行"其实是长图里已经有的行——接缝上就出现一段重复。所以只在读数比已知的边更低时才采纳它。

滚动注入有三条路，`--inject auto`（默认）按"打扰最少"的顺序挑：

1. `wlr`：compositor 的 `zwlr_virtual_pointer_manager_v1`，Hyprland / sway / niri 都提供。不需要设备权限，也不调用任何外部程序。
2. `portal`：XDG RemoteDesktop portal（KDE Plasma、GNOME），走 session bus，compositor 弹一次授权；KWin 侧是它自带的 EIS 服务端。**本机是 Hyprland，这条路径没有经过真机验证**（Hyprland 的 portal 不实现 RemoteDesktop），需要在 KDE 上验收。
3. `uinput`：内核虚拟鼠标（`/dev/uinput`）。任何桌面都能用，但需要设备写权限——把用户加进 `input` 组，或给设备一条 `TAG+="uaccess"` 的 udev 规则（发行版的 udev 默认往往不给 `/dev/uinput` 任何权限；本机可用是因为 ydotool 包提供 `GROUP="input", MODE="0660"`、game-devices-udev 提供 uaccess ACL）。运行 vshot 本身仍然不需要 root。

提示条是一个**固定尺寸**的 layer surface，故意不做全屏：桌面在滚动期间是活的，而且它必须待在选区**之外**，否则会被拼进长图。边角都被选区占满时（极端情况）它会缩成看不见的一小块，只保留键盘，Enter/Esc 仍然有效。

已知限制：

- 固定顶栏和底栏靠"这些行在帧里没动"来识别：它们会正确地只出现在长图的头/尾，但一个**滚动到某处才出现**的固定条（浮出的工具条）仍会被当成页面内容。`--ignore-top N` 可以把顶部 N 行强制排除在匹配之外。
- 惯性滚动、动画、视频：画面在动的时候，每一帧的位移就是应用的动画速度，能测到就接上去；对不上的帧不会被采纳，**滚轮也不会退回去**——下一帧仍然和最后一张被采纳的画面比对，所以那一段内容会在下一次成功对齐时一起补进长图；连续多帧都对不上则整个停止并报告原因。滚动因此永远是单向的，不会上下抖。
- 懒加载页面在滚动中新增内容，可能重复或缺失少量行。
- 选区必须完全落在一块屏幕内——滚动内容是单个滚动容器，跨屏没有意义。
- 抓屏花的时间会限制抓帧率：wlroots 只拷贝选区（`capture_output_region`），KDE 上则走 KWin 的 D-Bus 服务抓整屏再裁，且没有走区域抓取（KWin 的 `CaptureArea` 是私有 API，参数顺序未在本机核实），所以 KDE 的帧率明显低于 wlroots。对齐本身也不便宜，而且随区域面积线性增长——一帧的开销里九成是跨位移范围的全搜索。实测（同机、同代码，客户端 CPU，不含合成器往返）：4K（scale 2）上 1800x1400 的区域每帧 **release 约 6.7 ms**（对齐 5.2 + 像素转换 1.5），1080p 上 900x667 约 **2.3 ms**；**debug 构建慢一个数量级**（4K 每帧约 0.7 s）。所以长截图请用 release：`cargo build --release`，然后跑 `target/release/vshot`。没动的那一帧不付对齐的钱（提前返回，release 下 <1 ms）。

## pin 图片浮层

`vshot pin` 把图片作为浮层钉在屏幕上：**拖拽**移动、**滚轮**以图片中心缩放（0.1x–8x，并短暂显示倍率）、**双击**关闭该图、**点击聚焦后按 Space** 进入完整标注编辑器。**点击的那张图会立刻提到最前**，所以重叠时被点到的那张一定压在其余之上；进入编辑也一样，正在标注的图不会被压住。

叠放顺序由 **daemon 自己持有**，不靠合成器：一块输出上的所有 pin 画在**同一个 surface** 里（见下），daemon 的 pin 列表顺序就是绘制顺序，提到最前只是把该 pin 移到列表末尾再重绘一次——不重建 surface、不动输入区域，也不影响键盘焦点。这条是必需的：layer-shell 没有 raise/restack 请求，合成器把同一层的 surface 按 map 顺序排（Hyprland 的实现是每块输出每层一个 surface 向量，新 map 的追加到末尾，绘制按顺序、命中测试反向取），隐藏再显示同一条 surface 不会重排；若每个 pin 各占一个 surface，提升就只剩"重建 surface"这一条路，而那会丢掉刚点出来的键盘焦点（Hyprland 的 xdg-activation 只支持 toplevel，layer surface 的焦点是点击时由合成器给的）并触发一次该层的淡入动画。

新 pin 落在**激活的输出**上：**指针所在的那块屏优先**（`hyprctl cursorpos` 查询并按其逻辑矩形命中显示器），指针读不到时退回**键盘焦点所在**的输出（Hyprland `hyprctl monitors -j`、Sway `swaymsg -t get_outputs`、niri 没有指针位置查询（它的 IPC 里根本没有这样的请求），所以它只能答"焦点窗口所在的输出"：`niri msg --json focused-output` 直接返回那个输出，没有列表可扫（源码里取的是 `layout.active_output()` → `monitor_set.active_monitor_idx`，而这个索引只在**焦点变化**时更新，指针移动不改它——niri 的 focus-follows-mouse 默认也是关的）。**KDE 则问 KWin 自己的 `org.kde.KWin.activeOutputName`**——`gdbus` 与 `dbus-send` 任一能用即可），都没有则回退主输出。这样"在哪块屏幕就在哪块屏幕 pin"才成立；daemon 自己拿不到这个信息（无窗口进程只能看到指针在 (0,0)）。

**只问当前会话自己的合成器**（按 `XDG_CURRENT_DESKTOP`/`XDG_SESSION_DESKTOP` 判定；两者都没写合成器名时才把所有探针都试一遍。`window active` 与 `window pick` 用的是同一条判定）。同时跑着两个合成器时这一步是必须的：从 Hyprland 终端里启动的 Plasma 会话会继承 `HYPRLAND_INSTANCE_SIGNATURE`，`hyprctl` 于是照样应答，报的却是**没人看的那个 Hyprland 实例**的焦点显示器——pin 就固定落在那边，看起来像"永远是主屏"。

CLI 把这个答案的**输出名**发给 daemon（Qt 的屏幕名就是合成器的输出名，按名字匹配一次到位），同时附上**逻辑矩形**作为兜底（老 daemon 或不报名字的合成器按几何匹配）。KDE 上只有名字：KWin 直接回答"当前输出是哪个"，不给几何。Hyprland 上报的 `width`/`height` 是原生分辨率而 `x`/`y` 是逻辑坐标，所以要按 `scale` 换算后才能与 Qt 屏幕几何或指针位置比较。名字的好处是精确：矩形匹配要求 Qt 的逻辑几何与合成器上报值完全相等，而名字没有这个隐患。一个 layer-shell surface 只能属于一块输出，所以 daemon 为**每一块输出各持有一个渲染面**，面上按自己的顺序画**该屏上所有 pin**：pin 的图像、缩放与全局位置由 daemon 统一持有，每个面只画与它重叠的部分，因此拖拽可以**跨越显示器**——手势始终由拖起它的那个面持有，另一块屏上的副本同步跟随。命中测试也在面内做（从栈顶往下找第一个包含指针的矩形），所以"拖谁、缩谁、双击关谁"由面报回的 pin id 决定，daemon 再按 id 取状态。整个面覆盖输出，但输入区域只跟着各张图的矩形走（并集）；某屏上一张图都没有时输入区域被挪到面之外——Wayland 没有"无输入区域"的请求，未设置反而等于整面可点，不挡住那里的点击。显示器热插拔时 daemon 会为新输出补面、为移除的输出收面（并把 pin 收回可视区域）；没有 pin 时 daemon 不持有任何面。

pin 的**尺寸按图片的来源密度来定**，默认不需要任何参数。图片来源密度按以下顺序确定：

1. **手动指定**：`--density N`（1–4），或同级的环境变量 `VSHOT_PIN_DENSITY=N`（便于在快捷键/脚本里设一次）。优先级最高，自动判定不对或信息缺失时用；
2. **vshot 自己的截图**（`region`/窗口/monitor 加 `--pin`）：直接带上来源输出的缩放，逐像素还原，不放大也不糊；
3. **图片自己声明**：PNG 的 `pHYs` 块（例如 192 DPI = 2 倍）。这是通用标准，不依赖任何桌面；96 DPI（Qt 对未声明 PNG 的默认值）和 300 DPI 这类打印分辨率都不算密度声明，不会误用。**vshot 自己写出的 PNG**（`--output` 文件、`-o -` 标准输出、`--clipboard`）都会写上这个块，所以之后再 pin 这些图、或者 pin 回贴到剪贴板里的图，都不需要任何记录；
4. **产出图片的工具留下的记录**：截图工具才是唯一知道图片来自哪块屏的一方（grim、satty、spectacle 都不往图里写密度），所以按约定读取
   - `<图片路径>.scale` 文件里单独一个数字，或
   - `$VSHOT_PIN_SOURCE_FILE`（默认 `/tmp/screenshot-path`）里的一行 `<图片路径> <缩放>`，路径与正在 pin 的图一致才采用，因此不会串用上一张截图的倍率；
5. 都没有时，**按图片尺寸与落点输出推断**（落点输出的倍率由 daemon 自己读 Qt 屏幕的 `devicePixelRatio`，也就是合成器给客户端的 Wayland scale，不经过上面的探针——所以整数 scale 的判定与合成器无关，niri 上也一样）：图片像素数放得进该输出的原生分辨率时按 1 图素 = 1 屏幕素；放不进时说明它不可能来自这块屏，取"能容纳它的最小的那块屏"的缩放。最后再**按输出宽度**收一次上限（不放大、也不因为图比屏幕高而缩小），所以初始 pin 一定是可读的自然尺寸：长截图这类本来就比屏幕高的图保持自然宽度、顶边对齐落到屏幕上，超出屏幕的部分垂在下方，滚轮再从这里缩放。

第 4 条要生效，截图脚本在保存图片后写下记录即可，例如 Hyprland (Lua)：

```lua
-- 截图后：路径 + 当前输出倍率
hl.exec_cmd("echo " .. path .. " " .. hl.get_active_monitor().scale .. " > /tmp/screenshot-path")
```

或给每张图写一个 sidecar（更适合并行/历史图片）：

```sh
echo 2 > "$shot.png.scale"
```

`vshot pin` 与 `--clipboard`（剪贴板里是图片文件路径时）都会去匹配这条记录；剪贴板里是裸图像数据时没有路径可匹配，只能用第 1、3、5 条（vshot 自己放上剪贴板的图带着第 3 条，仍然能被认出来）。

```sh
vshot pin --density 2 shot.png      # 明确来源是 2 倍屏
VSHOT_PIN_DENSITY=2 vshot pin --clipboard
```

最坏情况（图不是 vshot 截的、脚本也没记记录）第 5 条仍能给出合理结果；`--density` 是最后的手动兜底。因此同一张图 pin 到 2 倍缩放的 4K 屏上时，占的逻辑尺寸是 1080p 屏上的一半，**两块屏上的物理大小一致**。跨密度渲染由每个渲染面自己做一次面积滤波下采样并缓存，而不是让 painter 每帧用 2x2 近似重采样（那会让 4K 图 pin 到 1080p 时发糊）。

pin 需要一个**常驻后台进程**（daemon）：layer-shell 浮层 surface 由创建它的进程拥有，`vshot pin x.png` 命令退出后 surface 就会消失；并且"一键显示/隐藏所有 pin""关闭其中一张"都要求一个同时持有全部浮层的进程。因此：

- 复用 Qt 二进制：`vshot-qt-ui --pin-server <socket>` 即 daemon，首次 `vshot pin` 连不上 socket 时自动分离式拉起（不占终端）；
- CLI 是瘦客户端，通过 Unix socket 发送单行 JSON 请求（见下条路径规则）；
- daemon 存续到最后一个 pin 关闭：关闭最后一张 pin（或 `--close-all`）约 0.5s 后 daemon 自动退出（新来的 add 会先被服务并取消退出）；下次 pin 命令自动重新拉起。`vshot pin --quit` 仍可随时手动退出；
- **不要用 `pkill`/`kill -9` 结束 daemon**：它持有 layer-shell surface，被强杀时部分合成器（实测 Hyprland 0.56）会残留该 surface 与其截屏会话，导致**所有输出的 screencopy 永久阻塞**（`vshot`/`grim` 全部超时，且 `hyprctl reload`、DPMS 循环、`force_renderer_reload` 都无法恢复，只能重启会话）。请始终用 `vshot pin --quit`，它会在退出前 unmap 全部浮层；daemon 也已处理 `SIGTERM`/`SIGINT` 走同样的优雅路径；
- socket 路径默认 `$XDG_RUNTIME_DIR/vshot-pin-<uid>.sock`（缺失时回退 `/tmp`），可用 `VSHOT_PIN_SOCKET=<绝对路径>` 覆盖，便于隔离测试多实例；
- pin 的渲染面平时不持有键盘（`KeyboardInteractivity=OnDemand`）：点击后该屏的渲染面获得键盘焦点，**点中的那张图**出现亮色描边（这块屏上其余 pin 不会），点别处自动让出。聚焦时按 **Space** 进入编辑模式，编辑的是该面最后点中的那张。

Wayland 客户端拿不到全局按键，"一键显隐"请自行绑到合成器快捷键，例如 Hyprland：

```
bind = SUPER, P, exec, vshot pin --toggle
```

`vshot region --pin`（以及 `all`/`monitor`/`window` 加 `--pin`）把截图结果直接 pin 上屏：PNG 写入私有临时文件、daemon 读入内存后立即删除，不在用户盘留文件。

`vshot pin --clipboard` 把**剪贴板当前内容**pin 上屏，可与文件参数混用（`vshot pin a.png --clipboard`）。剪贴板由 daemon 用 `wl-paste` 读取——`wl-clipboard` 的读取端，与截图侧写剪贴板用的 `wl-copy` 对称。Qt 自己的 Wayland 剪贴板在这里不能用：Qt 6.11 只实现了 wlroots 的 `zwlr_data_control_manager_v1`（`libQt6WaylandClient` 里只有这套符号），而 KWin/Plasma 提供的是标准化的 `ext_data_control_manager_v1`，两边对不上；实测同一个 KDE 会话里 `wl-paste --list-types` 列出 Firefox 的 `text/html`、`text/plain` 等类型时，Qt 的 `QClipboard` 连 `formats()` 都是空的，pin daemon 因此永远读不到内容。解析顺序：

1. 内嵌图像数据——截图工具或浏览器"复制图像"放入的位图（`image/png`、`image/jpeg` 等，任取剪贴板提供的第一种能解码的）；
2. 复制的文件——文件管理器里复制的图片文件（`text/uri-list`，取第一张能解码的本地文件）；
3. 纯文本——内容为一个存在的本地图片路径；
4. 纯文本——其余文字渲染成一张"文字卡片"图再 pin，按内容自动选择格式：

   - 剪贴板带 `text/html`（IDE/浏览器复制的代码、富文本）→ 按 HTML 渲染，**保留语法高亮配色**。只提供 HTML 而不提供纯文本的剪贴板也走这条（以前 Qt 的 `text()` 会返回空、直接失败）；
   - 看起来是 markdown（代码围栏、标题、列表、表格、加粗、链接等特征）→ 按 GitHub 风格 markdown 渲染；
   - 看起来是代码（分号/花括号/缩进/常见关键字等特征，启发式）→ 等宽字体深色编辑器风格卡片；
   - 其余 → 普通文本卡片（跟随系统亮暗主题，自动换行）。

   文字卡片按所在输出的像素密度渲染，HiDPI 下不模糊；超宽内容自动换行，超高内容截断。

剪贴板为空时提示 `the clipboard is empty`（`wl-paste` 用非零退出码说明"Nothing is copied"），有内容但既无图像也无可用文字时提示内容不可 pin；缺少 `wl-paste` 时直接说明要装 `wl-clipboard`。三种情况都不影响已有的 pin。

### pin 编辑模式（Space）

聚焦某个 pin 后按 **Space**，daemon 会导出该图并拉起与截图相同的完整标注编辑器（工具栏、文字、马赛克、撤销/重做）：

- 编辑器覆盖 pin 所在的整块屏幕，但**不自己绘制图片**：画面上的图就是那个真实的 pin（daemon 的渲染面画出来的），编辑器只在其上叠加标注。开编辑前 daemon 会先把该 pin 提到栈顶，否则一张压在它上面的 pin 会盖掉正在标注的内容。工具栏和弹出面板浮在图片外的空白画布上，与区域截图的工具栏逻辑一致；标注超出图片的部分会被裁掉。
- 选中工具（默认）在图片上拖动时，编辑器通过 daemon socket 的 `move` 命令直接复用 pin 自身的移动逻辑（同样只是改 daemon 持有的位置再重绘），不产生副本；daemon 返回实际落下的矩形（含它自己的防丢失夹取），编辑器据此校正标注位置。方向键可微调，Shift+方向键步长 10px。图片不会被拖到屏幕外（始终至少有 32px 留在它最靠的那块输出上）。
- Esc 取消本次编辑：标注被丢弃、像素保持原样，但**位置不回退**——拖到哪儿就留在哪儿；Enter 或工具栏 OK 确认则连同标注一起写回；
- 确认后由 Rust 渲染管线把标注合成进图像（与截图导出同一条代码路径，保证所见即所得），结果同样通过 socket `move` 命令回写：像素被替换，pin 落到拖动后的位置（图片尺寸不变）；
- 一次只能有一个 pin 处于编辑会话；编辑过程中编辑器持有独占键盘。

## 图像和输出映射

内部帧统一为 RGBA8、top-left origin。PNG 输入由 `image` 解码，最终 sink 前重新编码 PNG。多输出合成支持负 logical origin 和输出间空隙；场景画布使用最高输出 scale，较低 scale 的输出使用 nearest-neighbor 放大。当前仍要求正整数 scale、`transform=normal` 以及可安全证明的 logical/pixel 映射；fractional scale、旋转和无法证明的映射会清晰失败，而不是生成疑似错误的截图。窗口截图（`window active`、`window pick`）与像素识别都尽量绕开这层放大：落在单个输出内的矩形从那个输出自己的帧裁剪、用它的 scale 写密度，像素识别也逐输出分析。

## 截图后端

启动时探测一次，之后与后端无关：

1. 先连 `wlr-screencopy-unstable-v1`。只有它以「缺少 `zwlr_screencopy_manager_v1`」失败时才说明「这不是 wlroots 系合成器」，此时才转后端 2；其它失败（连接不上、缺 `wl_shm`、没有输出等）原样上报，不会被伪装成 KDE 问题。
2. **KWin ScreenShot2**：`org.kde.KWin.ScreenShot2` 是 KWin 的私有会话总线服务（对象路径 `/org/kde/KWin/ScreenShot2`）。vshot 传入一根管道的写端，调用 `CaptureScreen(name, options, pipe)`，KWin 把像素写进管道、在回复的 `a{sv}` 里给出 `width`/`height`/`stride`/`format`/`scale`/`type`，`stride * height` 正是管道上收到的字节数。管道在另一线程里读到底，否则整帧放不进内核管道缓冲区时会与合成器互相等待。

   像素是**预乘 alpha 的 BGRA**（`format = 6`，即 `QImage::Format_ARGB32_Premultiplied`）：vshot 反预乘、丢掉行尾补白、交换到 RGBA。alpha 怎么处理取决于这一帧是什么：**输出截图把 alpha 归一为 255**——截图代表屏幕上的合成结果，物理上不透明，没有被合成内容覆盖的区域（原始 `0,0,0,0`）因此表现为黑；**窗口截图原样保留 alpha**——窗口自己可以盖满画面，也可以像半透明终端那样透出后面的东西，而它四周的阴影本来就只覆盖一部分（见下一条）。`format` 不是 6、或缺 `width`/`height`/`stride` 时明确报错，不会按已知布局硬解读。

   option 里必须带 `native-resolution`：不带时 KWin 按**逻辑尺寸**渲染，一台 4K 屏在 scale 2 下就返回 1920x1080，与拓扑报的 scale 对不上；vshot 会以 `unsupported output mapping` 明确拒绝，而不是把逻辑帧悄悄放大去凑。实测同一个 DP-2（4K、scale 2、KWin 6.7.5）：`native-resolution` 缺省时 `width=1920 height=1080 scale=1.0`（8294400 字节），带上时 `width=3840 height=2160 scale=2.0`（33177600 字节）。

   窗口走 `CaptureActiveWindow(options, pipe)`（接口 Version 5 里还有 `CaptureWindow(handle, options, pipe)`，`handle` 是窗口 uuid）：管道与结果格式一模一样，但像素是**焦点窗口自己**的一帧——`include-decoration: true` 把标题栏和边框带进来；`include-shadow` **不传、保持 KWin 的默认值（画阴影）**，因为阴影是窗口在屏幕上样子的一部分，而这一帧保留 alpha，阴影没盖到的像素就是透明的。实测（私有总线上一个无头 KWin + 一个 kitty 窗口，该 kitty 自己配了 `background_opacity 0.8`）：拿到 770x558，最外圈 8-10 像素 alpha 全 0，再往里是数十像素宽、alpha 由 0 渐升的阴影带，窗口内部 alpha 204（正是 0.8）——把 alpha 归一为 255 的那一版就是把这些半透明像素还原成不透明的深色，也就是窗口四周那圈黑边。实测 KWin 6.7.5：4K 屏（scale 2）上一个 1920x1046 逻辑尺寸的 Firefox 窗口返回 `width=3840 height=2092 scale=2.0 windowId={a9bf0b0c-…}`，把它和同一屏的 `CaptureScreen` 整屏帧对照，它正是该屏设备坐标 (0,68) 起的那一块（逐像素吻合）——也就是窗口逻辑位置 (0,34) 乘 scale 2；那个窗口当时是**最大化**的，阴影被屏幕边缘裁掉，所以尺寸正好等于窗口本身，浮动窗口则会多出阴影那一圈。窗口捕获无论带不带 `native-resolution` 都是原生尺寸（同一窗口两种写法都是 3840x2092），所以 vshot 只从回复里的 `scale` 取密度，读不到就报错而**不假设成 1**：`scale` 是这张图该按多大显示的唯一来源，假设 1 正是"pin 出来的图大一倍"那类错。窗口截图因此不经过场景裁剪，跨接缝的窗口也能整块到手。

   两个后端都不可用时，错误信息同时说明「没有 wlr-screencopy」和「KWin ScreenShot2 也不可用」以及 KWin 那一侧的具体原因，便于区分「合成器不支持」与「会话没配好」。

### KDE 授权

KWin 的 `org.kde.KWin.ScreenShot2` 是**受限 D-Bus 接口，而且它不弹任何对话框**——没有可点的"允许"。`ScreenShotDBusInterface2::checkPermissions`（KWin 6.7.5，`src/plugins/screenshot/screenshotdbusinterface2.cpp`）做三件事：由调用方 pid 取 `/proc/<pid>/exe` 指向的可执行文件，在已安装的 desktop file 里找 `Exec=` 第一个词与该路径 canonical 一致的那一份，然后要求它声明：

```ini
X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2
```

Spectacle 正是这样被授权的（`/usr/share/applications/org.kde.spectacle.desktop` 里就有这行）。不满足时返回 `org.kde.KWin.ScreenShot2.Error.NoAuthorized`，vshot 会把它翻译成"要写哪份 desktop file、`Exec=` 该写哪个路径、要不要跑 `kbuildsycoca6`"，而不是复述 KWin 那句容易被误读成"去点个确认框"的措辞。由此推出：

- **装包安装的 vshot 不需要任何额外操作**：`/usr/share/applications/vshot.desktop` 的 `Exec=/usr/bin/vshot` 且声明了上面那个 key。若装完包 KWin 仍拒绝，跑一次 `kbuildsycoca6 --noincremental`（kservice 靠 ksycoca 数据库查 desktop file）；**刚新建的 desktop file 还有几秒的可见延迟**——运行中的 KWin 是惰性重建 ksycoca 的，它可能在第一次查询时仍拿旧的库，隔几秒重试一次即可（实测：同一份新建的 desktop file，第一次调用被拒，5 秒后同一命令成功）。
- **直接从 `target/release/vshot` 跑的开发构建拿不到授权**——没有任何 desktop file 的 `Exec=` 指向那个路径。加上一份即可，用不着装包（`Exec=` 写当前二进制的绝对路径，vshot 报错时会把这个路径打出来）：

  ```sh
  printf '[Desktop Entry]\nType=Application\nName=VShot\nExec=/path/to/vshot\nNoDisplay=true\nX-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2\n' \
    > ~/.local/share/applications/vshot.desktop
  kbuildsycoca6 --noincremental
  ```
- 不影响授权的因素（均实测）：`NoDisplay=true`、`Exec=` 后面带参数（只有第一个词参与匹配）、desktop file 放在用户数据目录（`~/.local/share/applications`）还是系统数据目录（`/usr/share/applications`——KWin 查的是整条 `XDG_DATA_DIRS`）。能影响的是 `Exec=` 指向的二进制——换了路径就重新授权不了。
- 合成器侧的 `KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1` 跳过整个检查，**仅供开发/测试**。
- 这套授权**只覆盖"允许调用"**：抓像素本身跟其它后端一样，就是向合成器要一帧。

`include-cursor` 选项按 KDE 文档传入；它是否真的把光标画进图像**尚未验证**——无头 `--virtual` 输出上没有指针可画。

## 冻结、编辑与资源生命周期

Rust 非交互模式为每个输出创建一个全屏、四边 anchored 的父 layer surface。收到 layer-surface configure 后，程序 ack configure，使用 Unix SHM/mmap 创建两个有效的父层 `wl_buffer`，将首次捕获的冻结 raw frame 写入两个 slot，并提交 slot 0；该路径只使用父 layer surface 与 `wl_shm`。overlay、layer surface、buffer 和临时 SHM 映射由 RAII 清理，冻结期间不会再次读取桌面。

交互式 `region` 不调用 Rust Wayland editor，而是由 `vshot-qt-ui` Qt helper 负责交互 overlay、选择和编辑。Rust 通过私有 session 将已捕获的静态帧交给 helper；helper 返回选择区域和标注后，Rust 在最终输出阶段应用标注。`region` 一开始就把这帧铺满屏幕（所见即所裁）。

`window pick` 用**两次** helper 会话：第一次是 `window-pick` 模式，session 携带候选窗口列表（`candidates`，含标题）但 helper 不画帧、桌面保持实时；这次会话是双向的——helper 把 `{"request":"candidates"}` 按行写在 stdout 上，CLI 在 stdin 上回一份新的窗口列表（没有可复查来源时回空的 `{}`），所以候选跟着指针刷新。点击即返回（结果里带上点击位置）；Rust 随后重新捕获一帧，把点击位置重新解析成窗口矩形，再以 `region` 模式开第二次会话，session 里带上已确定的 `selection`，helper 收到后直接进入编辑状态（工具栏就位，帧就是刚才捕获的那一帧）。挑窗口期间桌面是实时的，所以这一步的帧与候选列表都可能过期——刷新、重新捕获与重新解析就是为它准备的。

## 已知限制

- 截图后端：wlroots 系走 `wlr-screencopy-unstable-v1` 的 wl_shm 路径（协议版本 1 至 3）；**KWin/Plasma Wayland 改走 `org.kde.KWin.ScreenShot2`**（见「截图后端」），因为 KWin 根本没有 screencopy，也没有 `ext-image-copy-capture`（实测 KWin 6.7.5 的 global 列表与 `libkwin.so.6` 里都找不到这两个接口名）。两者都没有实现 PipeWire、Portal ScreenCast 或 DMA-BUF。**不要以为"grim 能在 KDE 跑所以 vshot 也应该能"**：grim 只带 `zwlr_screencopy_manager_v1` 与 `ext_image_copy_capture_manager_v1`，在 KDE 上两个都不可用（实测报 "compositor doesn't support the screen capture protocol"），KDE 只能走它私有的 D-Bus 服务。GNOME/Mutter 三者都不提供，连 layer-shell 也没有。
- KWin 那条路的**冻结 overlay 仍然依赖 `zwlr_layer_shell_v1`**（KWin 提供它）。授权不是对话框：KWin 只认调用方那个可执行文件对应的 desktop file 里声明的受限接口（见「KDE 授权」），所以从 `target/` 里直接跑的构建会一直拿到 `NoAuthorized`，装上包再用 `/usr/bin/vshot` 才行。**同一个 `XDG_RUNTIME_DIR` 下可以同时存在多个合成器**（例如 tty 里另起的 KDE 占 `wayland-0`、Hyprland 落在 `wayland-1`），而 `WAYLAND_DISPLAY` 未设置时 libwayland 会用默认的 `wayland-0`——tty 或 ssh 里的 shell 就是这种情况，于是命令会连到"另一个合成器"上去。凡是跟合成器有关的失败，vshot 都会额外打印一行说明这次连的是哪个 display、以及本机还有哪些 display。
- Portal active-window backend 尚未实现；active window 先问合成器自己——KWin 用 `CaptureActiveWindow`、niri 用 `screenshot-window`，两者直接给窗口的像素与密度；其他合成器用 Hyprland/Sway/kdotool/KWin scripting 探针给几何——都缺失时回退到像素识别（`--pixel` 可强制），无缝无边框平铺场景除外。像素识别**逐输出**在原生帧上跑，结果只会是某一块屏上的一个窗口，不会横跨接缝；但当合成器不给焦点窗口描有色的边时，它答的是"指针下的窗口"，不是"焦点窗口"（见「截取活动窗口」）。`window pick` 的候选来自 Hyprland/Sway/KWin 的窗口列表；**niri 走它自己的挑窗**（没有 overlay 与标注编辑器，见「选择窗口」）；没有窗口列表查询的合成器要靠 `--pixel`，无缝无边框平铺与均匀桌面下没有任何候选，只能报错。候选列表在挑选期间**跟着指针刷新**（见「选择窗口」），所以切换工作区或移动窗口后，悬停高亮与最终截到的窗口都是实时的；只有 `--pixel` 那条路径没有可复查的窗口列表，会一直用挑选开始时的候选。
- 编辑结果使用 RGBA8 软件绘制，线宽和坐标按截图 logical scale 转换；Qt 文本框接受任意 Unicode 文本（含通过输入法提交的 CJK）。交互式文本由 Qt 按所选系统字体栅格化为 RGBA 位图后由 Rust 合成（见「交互式 overlay」）；未携带位图的旧 helper 结果回退到 Rust 内置 5x7 字体渲染，该回退路径仅支持可打印 ASCII。
- 交互式 `region` 的键盘和鼠标事件由 Qt/LayerShellQt 处理；不依赖 Hyprland 插件或私有输入接口。
- 混合 integer scale 会统一到最高 scale；fractional scale、rotation 和复杂 viewport 映射在**需要把输出合成为场景**的路径上会拒绝执行（`region`/`monitor`/`all`/`long`、像素识别、带标注的编辑）。这个校验在真正要用拓扑时给出，因此合成器自己给窗口像素的两条路（KWin `CaptureActiveWindow`、niri `screenshot-window`）在旋转/翻转输出上仍然可用——它们不碰输出像素，密度取该输出的整数 scale，拓扑不可用时退回合成器自报的 scale。
- `monitor current` 依赖 overlay 上收到 pointer enter/motion；通用 Wayland 没有可读取的全局鼠标坐标，因此不会用第一个 output 猜测结果。
- 需要 compositor 实际支持 layer-shell、SHM、xdg-output 及相应 seat capability。原生 screencopy 等待 compositor 返回帧最多 10 秒，超时会返回错误而不是永久阻塞。没有 Wayland 环境时，连接阶段会返回 `Wayland connection failed`；非交互父层和 Qt helper 需要分别在相应环境中验证。
- 长截图的滚动注入按 compositor 选路（见「长截图」）：Hyprland / sway / niri 用 `zwlr_virtual_pointer_manager_v1`（无需任何权限），KDE / GNOME 用 XDG RemoteDesktop portal（一次授权），都不行时才退到 `/dev/uinput`（需要 `/dev/uinput` 写权限）。无头 compositor 既没有指针也没有可滚动的内容，这条路径只能在真实会话里验证。

## 验证

在项目根目录运行：

```sh
cargo fmt --check
cargo test --locked
cargo clippy --locked --all-targets --all-features -- -D warnings
cargo build --release --locked
```

KWin 的 D-Bus 路径另有两个默认**不执行**的集成测试（需要真在跑 KWin 的会话），它们真的调用一次 `CaptureScreen` 并断言帧尺寸与不透明性，以及一次真实的后端选择。起一个无头 KWin 即可跑：

```sh
mkdir -p /tmp/kwin-e2e/{cfg,data,cache}
KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1 XDG_CONFIG_HOME=/tmp/kwin-e2e/cfg \
  XDG_DATA_HOME=/tmp/kwin-e2e/data XDG_CACHE_HOME=/tmp/kwin-e2e/cache \
  kwin_wayland --virtual --socket wayland-ke2e --no-lockscreen \
  --no-global-shortcuts --no-kactivities &
XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=wayland-ke2e \
  cargo test -- --ignored --nocapture
```

无头 KWin 的虚拟输出名是 `Virtual-0`，尺寸 1024x768；可用 `WAYLAND_DISPLAY=wayland-ke2e wayland-info` 确认。它没有 pointer capability，所以完整流程（需要 `WaylandSession::connect()` 的 seat pointer 检查）在无头环境必然失败——这是预期的，集成测试因此只覆盖到 D-Bus 采集这一层。`VSHOT_KWIN_E2E_OUTPUT` / `_WIDTH` / `_HEIGHT` / `_COLOR=R,G,B` 可覆盖默认的输出名、尺寸与中心像素颜色断言。

长截图需要真实会话：无头 compositor 既没有指针也没有可滚动的内容。排查时可以把每一帧和每一次判定落盘，再和产出的长图逐段比对：

```sh
VSHOT_LONG_DEBUG_DIR=/tmp/long-debug \
  target/debug/vshot long --geometry '2100,200 900x700' --output /tmp/long.png --max-frames 40
```

`/tmp/long-debug` 会留下 `grab-NNNN.png`（每次抓帧）与 `steps.log`（每帧的判定与位移）。不设置这个变量时不会有任何落盘。

## 许可证

MIT（见 `LICENSE`）。
