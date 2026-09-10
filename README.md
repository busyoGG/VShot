# vshot

`vshot` 是面向 wlroots compositor 的 Rust Wayland 截图 CLI。非交互截图采用严格冻结流程：先用 Rust 原生 `wlr-screencopy-unstable-v1` 捕获所有输出，再用 Rust 原生 `wlr-layer-shell-unstable-v1` + `wl_shm` 将这些静态帧显示为全屏父 layer overlay。交互式 `region` 由 Qt helper 负责 overlay 和编辑，始终只处理已经捕获的静态帧。

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
- `wl_compositor`、`wl_shm`（包含 `XRGB8888` 或 `ARGB8888`）、至少一个 `wl_output` 和 `wl_seat`；
- `zxdg_output_manager_v1`，用于 overlay 的输出名称和 logical topology；
- `zwlr_layer_shell_v1`，用于 Rust 非交互冻结 overlay；
- `zwlr_screencopy_manager_v1`（版本 1 至 3），用于原生帧捕获；
- `wl_output` 的名称事件（版本 4）用于按名称选择捕获输出；
- `vshot-qt-ui` 需要 Qt6 Core/Gui/Widgets/Network 和 LayerShellQt；
- 可选 `wp_cursor_shape_manager_v1`（仅保留的 Rust editor path 使用）；
- 使用交互式 `region` 时需要可执行的 `vshot-qt-ui`，也可通过 `VSHOT_QT_HELPER` 指定；
- 仅在使用 `--clipboard` 时需要 `wl-copy`。

当前实现严格要求 seat 同时具有 pointer 和 keyboard capability。缺少上述任一能力时会以非零状态和明确错误退出，不会假称截图已经冻结。

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

# stdout 输出纯 PNG bytes；日志只写 stderr
vshot all --output - > desktop.png

# 截图直接 pin 到屏幕（不落盘）
vshot region --pin

# pin 管理：添加图片、显隐、清空、退出 daemon
vshot pin shot.png another.png
vshot pin --clipboard        # pin 剪贴板里的图片（或复制的图片文件/路径/文字）
vshot pin --toggle          # 一键显示/隐藏所有 pin
vshot pin --hide / --show
vshot pin --close-all       # 关闭全部 pin（无 pin 后 daemon 自动退出）
vshot pin --list            # 打印数量与可见状态
vshot pin --quit            # 退出 daemon
```

`region` 的 `--geometry` 与 `--interactive` 互斥；未给出 geometry 时默认进入交互选择。输出 destination 必须且只能是 `--output PATH`、`--output -`、`--clipboard` 或 `--pin` 之一。`--cursor` 会请求 screencopy compositor 将光标合成到每个输出帧。

## 交互式 overlay

`vshot region`（未给出 `--geometry` 时）冻结桌面后交给 `vshot-qt-ui` 全屏 layer overlay，交互流程参考 HyprCapture：

- 冻结画面铺满每个输出；选区之外覆盖半透明暗色遮罩；
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

1. Hyprland：`hyprctl activewindow -j` 的 `at`/`size`；
2. Sway：`swaymsg -t get_tree` 中递归查找 focused node 的 `rect`；
3. KDE Plasma：优先 `kdotool`（若安装），否则一次性 KWin scripting 探针——通过 `org.kde.kwin.Scripting`（gdbus/dbus-send）加载读取 `workspace.activeWindow`/`activeClient` 的 `frameGeometry`（分别对应 Plasma 6/5），从用户 journal 轮询标记行取回；探针每次独立加载并在结束后卸载；
4. **像素识别兜底**：以上都不可用时，在已捕获的场景帧上自动检测焦点窗口——先拟合"焦点描边"（平铺合成器给焦点窗口画的高亮边框，闭合同色矩形轮廓），失败再做背景泛洪分割（从帧边缘追踪壁纸/阴影，无边框窗口靠 gaps、阴影或壁纸分离，已知光标位置时用于消歧）。

`vshot window active --pixel` 跳过 compositor 元数据，直接走像素识别——用于测试检测器，也可用于完全没有元数据接口的合成器。像素识别在降采样的分析帧上运行，4K 场景开销可忽略；**无缝无边框平铺（无 gaps、无阴影）没有任何像素信号**，此时如实报错而不是给出错误裁剪。KWin 探针依赖 `journalctl` 与 `gdbus`/`dbus-send` 之一（Plasma 环境均具备），且需要 journald 记录 KWin 的脚本日志；不可用时自动落到像素识别。

目标 geometry 会从已经捕获的冻结场景中裁剪；overlay 显示后不会重新访问 compositor。其他 compositor 的 Portal active-window backend 尚未实现。

## pin 图片浮层

`vshot pin` 把图片作为浮层钉在屏幕上：**拖拽**移动、**滚轮**缩放（0.1x–8x，光标为锚点并短暂显示倍率）、**双击**关闭该图、**点击聚焦后按 Space** 进入完整标注编辑器。

pin 需要一个**常驻后台进程**（daemon）：layer-shell 浮层 surface 由创建它的进程拥有，`vshot pin x.png` 命令退出后 surface 就会消失；并且"一键显示/隐藏所有 pin""关闭其中一张"都要求一个同时持有全部浮层的进程。因此：

- 复用 Qt 二进制：`vshot-qt-ui --pin-server <socket>` 即 daemon，首次 `vshot pin` 连不上 socket 时自动分离式拉起（不占终端）；
- CLI 是瘦客户端，通过 Unix socket 发送单行 JSON 请求（见下条路径规则）；
- daemon 存续到最后一个 pin 关闭：关闭最后一张 pin（或 `--close-all`）约 0.5s 后 daemon 自动退出（新来的 add 会先被服务并取消退出）；下次 pin 命令自动重新拉起。`vshot pin --quit` 仍可随时手动退出；
- **不要用 `pkill`/`kill -9` 结束 daemon**：它持有 layer-shell surface，被强杀时部分合成器（实测 Hyprland 0.56）会残留该 surface 与其截屏会话，导致**所有输出的 screencopy 永久阻塞**（`vshot`/`grim` 全部超时，且 `hyprctl reload`、DPMS 循环、`force_renderer_reload` 都无法恢复，只能重启会话）。请始终用 `vshot pin --quit`，它会在退出前 unmap 全部浮层；daemon 也已处理 `SIGTERM`/`SIGINT` 走同样的优雅路径；
- socket 路径默认 `$XDG_RUNTIME_DIR/vshot-pin-<uid>.sock`（缺失时回退 `/tmp`），可用 `VSHOT_PIN_SOCKET=<绝对路径>` 覆盖，便于隔离测试多实例；
- pin 浮层平时不持有键盘（`KeyboardInteractivity=OnDemand`）：点击后该 pin 获得键盘焦点（出现亮色描边），点别处自动让出。聚焦时按 **Space** 进入编辑模式。

Wayland 客户端拿不到全局按键，"一键显隐"请自行绑到合成器快捷键，例如 Hyprland：

```
bind = SUPER, P, exec, vshot pin --toggle
```

`vshot region --pin`（以及 `all`/`monitor`/`window` 加 `--pin`）把截图结果直接 pin 上屏：PNG 写入私有临时文件、daemon 读入内存后立即删除，不在用户盘留文件。

`vshot pin --clipboard` 把**剪贴板当前内容**pin 上屏，可与文件参数混用（`vshot pin a.png --clipboard`）。由 daemon 进程直接读取 Wayland 剪贴板，解析顺序：

1. 内嵌图像数据——截图工具或浏览器"复制图像"放入的位图；
2. 复制的文件——文件管理器里复制的图片文件（URI 列表，取第一张能解码的本地文件）；
3. 纯文本——内容为一个存在的本地图片路径；
4. 纯文本——其余文字渲染成一张"文字卡片"图再 pin，按内容自动选择格式：

   - 剪贴板带 `text/html`（IDE/浏览器复制的代码、富文本）→ 按 HTML 渲染，**保留语法高亮配色**；
   - 看起来是 markdown（代码围栏、标题、列表、表格、加粗、链接等特征）→ 按 GitHub 风格 markdown 渲染；
   - 看起来是代码（分号/花括号/缩进/常见关键字等特征，启发式）→ 等宽字体深色编辑器风格卡片；
   - 其余 → 普通文本卡片（跟随系统亮暗主题，自动换行）。

   文字卡片按所在输出的像素密度渲染，HiDPI 下不模糊；超宽内容自动换行，超高内容截断。

剪贴板既无图像也无可用文字时命令失败并提示，不影响已有的 pin。

### pin 编辑模式（Space）

聚焦某个 pin 后按 **Space**，daemon 会导出该图并拉起一个与截图相同的完整标注编辑器（工具栏、文字、马赛克、撤销/重做），编辑器窗口精确覆盖在这个 pin 上方：

- 画布固定为整张图片，无需先拖选区；Esc 取消本次编辑（pin 保持原样），Enter 或工具栏 OK 确认；
- 确认后由 Rust 渲染管线把标注合成进图像（与截图导出同一条代码路径，保证所见即所得），结果通过 socket `replace` 命令回写，pin 的屏幕位置与显示大小保持不变；
- 一次只能有一个 pin 处于编辑会话；编辑过程中编辑器持有独占键盘。

## 图像和输出映射

内部帧统一为 RGBA8、top-left origin。PNG 输入由 `image` 解码，最终 sink 前重新编码 PNG。多输出合成支持负 logical origin 和输出间空隙；场景画布使用最高输出 scale，较低 scale 的输出使用 nearest-neighbor 放大。当前仍要求正整数 scale、`transform=normal` 以及可安全证明的 logical/pixel 映射；fractional scale、旋转和无法证明的映射会清晰失败，而不是生成疑似错误的截图。

## 冻结、编辑与资源生命周期

Rust 非交互模式为每个输出创建一个全屏、四边 anchored 的父 layer surface。收到 layer-surface configure 后，程序 ack configure，使用 Unix SHM/mmap 创建两个有效的父层 `wl_buffer`，将首次捕获的冻结 raw frame 写入两个 slot，并提交 slot 0；该路径只使用父 layer surface 与 `wl_shm`。overlay、layer surface、buffer 和临时 SHM 映射由 RAII 清理，冻结期间不会再次读取桌面。

交互式 `region` 不调用 Rust Wayland editor，而是由 `vshot-qt-ui` Qt helper 负责交互 overlay、选择和编辑。Rust 通过私有 session 将已捕获的静态帧交给 helper；helper 返回选择区域和标注后，Rust 在最终输出阶段应用标注。

## 已知限制

- 当前只实现 `wlr-screencopy-unstable-v1` 的 wl_shm 路径（协议版本 1 至 3）；没有实现 PipeWire、Portal ScreenCast、DMA-BUF 或 ext-image-copy-capture。
- Portal active-window backend 尚未实现；active window 优先使用 Hyprland/Sway 命令行接口，缺失时回退到像素识别（`--pixel` 可强制），无缝无边框平铺场景除外。
- 编辑结果使用 RGBA8 软件绘制，线宽和坐标按截图 logical scale 转换；Qt 文本框接受任意 Unicode 文本（含通过输入法提交的 CJK）。交互式文本由 Qt 按所选系统字体栅格化为 RGBA 位图后由 Rust 合成（见「交互式 overlay」）；未携带位图的旧 helper 结果回退到 Rust 内置 5x7 字体渲染，该回退路径仅支持可打印 ASCII。
- 交互式 `region` 的键盘和鼠标事件由 Qt/LayerShellQt 处理；不依赖 Hyprland 插件或私有输入接口。
- 混合 integer scale 会统一到最高 scale；fractional scale、rotation 和复杂 viewport 映射会拒绝执行。
- `monitor current` 依赖 overlay 上收到 pointer enter/motion；通用 Wayland 没有可读取的全局鼠标坐标，因此不会用第一个 output 猜测结果。
- 需要 compositor 实际支持 layer-shell、SHM、xdg-output 及相应 seat capability。原生 screencopy 等待 compositor 返回帧最多 10 秒，超时会返回错误而不是永久阻塞。没有 Wayland 环境时，连接阶段会返回 `Wayland connection failed`；非交互父层和 Qt helper 需要分别在相应环境中验证。

## 验证

在项目根目录运行：

```sh
cargo fmt --check
cargo test --locked
cargo clippy --locked --all-targets --all-features -- -D warnings
cargo build --release --locked
```
