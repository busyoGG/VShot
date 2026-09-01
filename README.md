# vshot

`vshot` 是面向 wlroots compositor 的 Rust Wayland 截图 CLI。它的交互截图采用严格冻结流程：先用 Rust 原生 `wlr-screencopy-unstable-v1` 捕获所有输出，再用 Rust 原生 `wlr-layer-shell-unstable-v1` + `wl_shm` 将这些静态帧显示为 overlay；region 选择只处理冻结帧。

## 构建

```sh
cargo build --release
```

运行时需要：

- Wayland 会话；
- `wl_compositor`、`wl_shm`（包含 `XRGB8888` 或 `ARGB8888`）、至少一个 `wl_output` 和 `wl_seat`；
- `zxdg_output_manager_v1`，用于 overlay 的输出名称和 logical topology；
- `zwlr_layer_shell_v1`，用于冻结 overlay；
- `zwlr_screencopy_manager_v1`（版本 1 至 3），用于原生帧捕获；
- `wl_output` 的名称事件（版本 4）用于按名称选择捕获输出；
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
```

`region` 的 `--geometry` 与 `--interactive` 互斥；未给出 geometry 时默认进入交互选择。输出 destination 必须且只能是 `--output PATH`、`--output -` 或 `--clipboard` 之一。`--cursor` 会请求 screencopy compositor 将光标合成到每个输出帧。

## active window

通用 Wayland 没有标准的 active-window geometry API。`window active` 在 overlay 显示前依次尝试：

1. Hyprland：`hyprctl activewindow -j` 的 `at`/`size`；
2. Sway：`swaymsg -t get_tree` 中递归查找 focused node 的 `rect`。

解析不到可靠的非空 geometry 时明确失败。目标 geometry 会从已经捕获的冻结场景中裁剪；overlay 显示后不会重新访问 compositor。其他 compositor 的 Portal active-window backend 尚未实现。

## 图像和输出映射

内部帧统一为 RGBA8、top-left origin。PNG 输入由 `image` 解码，最终 sink 前重新编码 PNG。多输出合成支持负 logical origin 和输出间空隙；场景画布使用最高输出 scale，较低 scale 的输出使用 nearest-neighbor 放大。当前仍要求正整数 scale、`transform=normal` 以及可安全证明的 logical/pixel 映射；fractional scale、旋转和无法证明的映射会清晰失败，而不是生成疑似错误的截图。

## 冻结、编辑与资源生命周期

每个输出创建一个全屏、四边 anchored 的 overlay surface。收到 layer-surface configure 后，程序 ack configure，使用 Unix SHM/mmap 创建两个 `wl_buffer`，提交静态帧，并等待所有 output surface 完成初始化后才进入交互等待。所有编辑都作用于首次捕获的静态帧，编辑期间不会再次读取桌面。

`region` 初始拖拽完成后会进入编辑状态。选区四角和边缘可以拖动调整，选区内部可以移动；工具栏直接绘制在冻结 overlay 上，提供矩形、圆形、箭头、自由绘制、文字和马赛克工具。选择文字工具后点击已有文字可以重新编辑，Enter 完成文字输入。双击选区确认并保存，右键或 Escape 取消；取消不会创建文件或写入剪贴板。方向键可以对选区进行 1 logical pixel 微调。overlay、layer surface、buffer 和临时 SHM 映射由 RAII 清理。

## 已知限制

- 当前只实现 `wlr-screencopy-unstable-v1` 的 wl_shm 路径（协议版本 1 至 3）；没有实现 PipeWire、Portal ScreenCast、DMA-BUF 或 ext-image-copy-capture。
- Portal active-window backend 尚未实现；active window 仅支持上述 Hyprland/Sway 命令行接口。
- 编辑结果使用 RGBA8 软件绘制，线宽和坐标按截图 logical scale 转换；文字输入当前使用内置 5x7 ASCII 字体，仅支持可打印 ASCII。
- 编辑器使用 Wayland raw evdev 按键处理基础英文字符，不提供完整 XKB 布局、组合输入、中文输入或键盘重复。
- 混合 integer scale 会统一到最高 scale；fractional scale、rotation 和复杂 viewport 映射会拒绝执行。
- `monitor current` 依赖 overlay 上收到 pointer enter/motion；通用 Wayland 没有可读取的全局鼠标坐标，因此不会用第一个 output 猜测结果。
- 需要 compositor 实际支持 layer-shell、SHM、xdg-output 及相应 seat capability。原生 screencopy 等待 compositor 返回帧最多 10 秒，超时会返回错误而不是永久阻塞。没有 Wayland 环境时，连接阶段会返回 `Wayland connection failed`；动态 overlay 需要在真实 wlroots compositor 中验证。

## 验证

在项目根目录运行：

```sh
cargo fmt --check
cargo test
cargo clippy --all-targets --all-features -- -D warnings
cargo build --release
```
