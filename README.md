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
- **OCR 取字**——框选一块区域把文字读出来（中英日），走 `vshot ocr`，编辑器工具栏里也有「取字」按钮——它把识别到的文字就地选出来，而不是整段复制走；识别完弹一条桌面通知
- **翻译**——把识别到的文字翻译成别的语种，走 `vshot translate`：交互时译文直接画在原文上，也可输出「译文信封」给程序按原位置摆放；九个 provider，其中五个（Google / Microsoft / 火山 / 腾讯 Transmart / 彩云 Lingocloud——最后一个借用彩云的 token）不用你自己的密钥，其余是 Bing / 百度 / OpenAI 兼容接口 / 自己的程序
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

vshot annotate toggle                       # 屏幕标注：显隐浮层（顺手拉起 daemon）
vshot annotate clear                        # 清空全部标注
vshot annotate quit                         # 退出 daemon（连标注一起）

vshot settings                              # 设置：开窗口改编辑器样式与命令行默认值（写进 config.json）

vshot formats                               # 格式：本构建编译进来的文件格式，各自能调什么
vshot formats --json                        # 同上，输出成 JSON（设置窗口读的就是它）

vshot ocr                                   # OCR：框选，文字到 stdout
vshot ocr --json                            # OCR：框选，文字与每个字的位置输出成 JSON
vshot ocr --clipboard                       # 同上，进剪贴板
vshot ocr --input shot.png                  # 读一个已有的图片文件

vshot translate                             # 翻译：框完松手就翻，译文画在原文上；Enter 确认后译文到 stdout
vshot translate --output out.png            # 把合成好的译文 PNG 写到文件
vshot translate --clipboard                 # 把合成好的译文 PNG 复制进剪贴板
vshot translate --input shot.png            # 读一个图片文件，翻译它的文字
vshot translate --to en --from ja           # 指定目标 / 源语言
vshot translate --provider bing             # 换一个翻译服务
vshot translate --provider auto             # 按顺序挑可用的翻译服务
vshot ocr --json | vshot translate --stdin-ocr --json   # 翻译一个 OCR 信封

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
vshot replay status                             # 现在握着多少历史、在录还是待机
vshot replay stop                               # 结束回录会话
```

全局参数对所有捕获生效：

| 参数 | 说明 |
| --- | --- |
| `-o, --output PATH` | 写 PNG 到 PATH，展开 `%Y%m%d` 这类 strftime 格式；写完后把文件的 `file://` URI 复制进剪贴板。PATH 里的目录不存在时会建出来——`%Y%m` 这种"每个月一个目录"的写法因此可以直接用。`-` 表示写 stdout 且不复制 |
| `--clipboard` | 把 PNG 数据复制进剪贴板 |
| `--pin` | 把图像 pin 到屏幕，不落盘（daemon 读入内存后立即删除临时文件） |
| `-c, --cursor` | 请求合成器把光标画进每个输出帧。**`long` 不支持**（见[「已知不稳定点」](#已知不稳定点)）；截图里没有光标最常见的原因见[「光标（`--cursor`）」](#光标--cursor) |
| `--sdr-format FORMAT` | SDR 那一份写成什么格式，默认 `png`（本构建唯一的格式）。可用的格式与各自的参数见 `vshot formats` |
| `--hdr-format FORMAT` | 截图带 HDR 内容时 SDR 那份旁第二份的格式：`avif`（默认）或 `hdr`。见 [HDR](#hdr) |
| `--format-param FORMAT.NAME=VALUE` | 某个格式的某个编码参数，可重复，例如 `--format-param png.compression=high --format-param avif.quality=40`。名字与取值范围由格式自己声明，`vshot formats` 列出；超出范围的值会被**夹到边界**，不认识的名字用该格式的默认值 |
| `--tone-map MODE` | SDR 那一份怎么从 HDR 内容映射下来：`auto`（默认）/ `fixed` / `normalize`。见 [HDR](#hdr) |
| `--tone-map-white LEVEL` | SDR 白落在输出范围的哪个位置，0.5–0.95，默认 0.8。`fixed` 与 `auto` 使用 |
| `--hdr-area-test BOOL` | 是否按**面积**判定 HDR 内容（默认 `true`）。关掉退回「有一个像素超过就算」 |
| `--hdr-area-ratio SHARE` | 超过 SDR 白的像素要占画面的多少才算 HDR 内容，0–1，默认 0.0005。`0` 表示总是 HDR。只在开关打开时读取 |
| `--hdr-reference-white NITS` | HDR 文件里 `1.0` 代表多少 cd/m²，默认 203（BT.2408）。**只对文件自己没说的情况生效**：别人写的 Radiance 文件，或没有 VShot 私有 box 的 AVIF。自己截的图总是读那块输出的参考白 |

每次捕获必须且只能给一个输出目标。`region --geometry` 与 `--interactive` 互斥，不给 geometry 时默认交互选择（`--interactive` 用于显式声明这一意图）。路径格式示例：

```sh
vshot region --output "$HOME/Pictures/vshot-%Y-%m-%d_%H-%M-%S.png"
vshot all --output 'shots/capture-%Y%m%d-%H%M%S.final.png'
```

`%Y` `%m` `%d` `%H` `%M` `%S` 分别为年、月、日、时、分、秒，`%%` 是字面 `%`。前缀后缀可任意组合。文件输出与剪贴板输出都需要 `wl-copy`。

## 区域截图与标注编辑
`vshot region` 不给 `--geometry` 时，冻结帧铺满每块输出，选区之外盖半透明暗色遮罩：

**手势一览**（键盘动作里可改绑定的那些见下面「`shortcuts`——键盘绑定」一节；这张表是鼠标与「按住什么」的部分）：

| 手势 | 作用 |
| --- | --- |
| 左键拖 | 画当前工具的图形；**没握任何工具时**拖动 = 重新框一个选区（点一下不算，要真的拖起来） |
| 左键点标注 | 选中它（手上是什么工具都一样）；拖它的边缘 = 移动，形状/线条/马赛克可拖 8 个手柄缩放 |
| 左键双击选区内 | **确认并保存**（等同 Enter / 工具栏 OK） |
| 中键拖 | 移动整块选区（pin 编辑器里是移动底图）；落点在某张标注的**框**里 = 挪那张标注 |
| 右键**点一下** | **取消整次截图** |
| 右键**按住** | 取色器：放大镜跟着光标走，`C` 复制色值、`V` 设为当前颜色；松手什么都不发生 |
| 按住 `Ctrl` 拖 | **拿起**指针下的标注或整块选区：移动它，而不是用当前工具落墨 |
| 按住 `Shift` 拖 | 保持比例：矩形画成正方形、椭圆画成正圆，缩放选区或标注时保持长宽比 |
| 方向键 | 走光标（屏幕上真正的指针跟着走），一起按 `Ctrl` 是一步 10 逻辑像素；有选中的标注时移动它/它们 |
| `Ctrl+A` / `Ctrl+D` | 全选 / 取消全选（全选之后方向键挪的就是整套） |
| `Delete` / `Backspace` | 删除选中的标注 |
| `Ctrl+Z` / `Ctrl+Y` | 撤销 / 重做 |
| `Esc` | 先退出取字、翻译这类状态，再按一次才取消整次截图 |

- 拖拽画矩形，四周 8 个手柄调整大小，**按住中键**拖选区内部移动位置，方向键微调（Ctrl 加速为 10 逻辑像素）；拖拽或调整时，光标旁显示 8x 放大镜与原生像素坐标，选区外侧显示 `宽 × 高`（优先挂在选区左边、与上边缘齐平；左边没位置时改挂上方或下方），不压住要截的画面。没有选中任何工具时，在选区里拖拽就是**重新框一个**，而不是移动它；**Select 工具**在手时同一拖拽是移动整块选区（按住 `Ctrl` 或中键则任何工具下都能拖走它：落点在某张标注的**框**里就是挪那张标注，落在别处——包括选区的身上——就是挪整块选区，所以「手边有个选中的标注」不会让选区变得够不着）；**没有工具时「按下」还不算重新框**：要真的拖动起来才开始画新框，点一下（双击的第一下也在内）选区原样不动——「选区内双击确认截图」靠的就是这个，否则第一下就把选区缩成一个像素，第二下就没东西可确认了
- **Enter**、选区内双击或工具栏 OK 确认；**Esc** 或**右键点一下**取消整次截图（右键**按住**是取色器，见下；文本框内的 Esc 只关闭文本框）
- 工具栏是两行按钮加右侧固定的一角：按钮按顺序排下来——**Select** 与绘图工具 Rect、Ellipse、Arrow、Line、Wave、Bezier、Draw、Text、Number、Mosaic、Pick，再接上作用在截图上的动作：图片、取字、翻译、长截图、Pin——**上面一行填满面板的实际宽度，剩下的才折到第二行**；面板宽度取命令条与样式子面板里更宽的那个，所以换个工具把样式子面板撑宽时，多出来的宽度用来把更多按钮收进第一行，而不是留成第二行末尾的一块空卡片；换行时按钮是**滑过去**的（140ms），不是直接跳过去；右下角是两行两列的 Undo/Redo 与 OK/Cancel（上排撤销/重做，下排确定/取消），贴着面板右边而不是跟在最长那行的末尾（面板宽度取命令条与样式子面板里更宽的那个）；样式子面板按当前工具显隐，跟随选区移动。「长截图」把这块选区交给滚动长截图（`vshot long`）而不是保留它；选区横跨两块屏幕时该按钮置灰。**「取字」与「翻译」是开关**：再点一次就退出它开的那件事——取字模式关掉（不再重新识别一遍），译文从画面上撤下来——而**翻译之后按 Esc 也是先撤译文**，再按一次才退出截图（和取字模式一样，先退状态、后退会话）
- **Pin** 和 OK 一样结束编辑，但成品直接钉在屏幕上（等同 `--pin`）而不落盘；Pin 编辑器里不再提供它
- 样式项：颜色色板（含自定义取色器：HSV 渐变 + 十六进制输入）、线型 Solid/Dash/Dot、箭头头型 Open V/Filled、粗细 1-64、箭头大小 1-8、字号 7-448（直接就是像素高）、马赛克形状 Rect/Ellip/Brush、马赛克程度 1-3、系统字体列表（每项按自身字形预览）。Arrow 是按下点到释放点的直线箭头；Draw 是自由绘制；Mosaic 的马赛克程度控制像素块大小与涂抹半径
- 工具栏有个 **Select 工具**，但它不是选中的前提：**单击任意标注即选中它**（手上是什么工具都一样），拖动它的边缘即移动（文本同样），形状/线条/马赛克可拖把手缩放，Delete/Backspace 删除；样式修改即时应用到选中标注；**Ctrl+A 全选**之后，**按住中键**或在 **Select 工具**下（或按住 `Ctrl`）从任意一个标注上拖，就是**整套一起移动**（方向键同样挪整套，撞到画面边缘时整体停下，相对位置不变）；手上还握着绘图工具时，按在标注上照旧是那个工具落墨——全选是「选中了一整套」，不是切进了一个模式；**Ctrl+Z / Ctrl+Y**（或 Ctrl+Shift+Z）撤销/重做。标注以全局逻辑坐标传回 Rust，最终 PNG 由内置软件渲染重绘，与预览一致
- **Pick** 是取色器：指针停在画面上时旁边跟着放大镜，下方显示光标下那个像素的色块和十六进制值；点一下把颜色交给进 Pick 之前用的那个工具（**没有选中任何工具**时进就交给 Draw），并自动回到那个工具。取色只取 RGB，不透明度保持该工具原来的值；Pick 自己没有样式项，样式行显示的是那个工具的值
- **贴图 / 取字**：工具栏的「图片」按钮从磁盘挑一张，或 **Ctrl+V** 直接把剪贴板里的图贴进来——原尺寸落在选区正中，比选区大时等比缩小塞进去，贴完自动选中它；「取字」按钮把选区里的文字识别成一层可就地选取的文字层，而不是整段复制走（见[「OCR 取字」](#ocr-取字)，`cli.ocr.notify` 可关）

界面语言默认跟随系统（`QLocale::system()`），可用 `VSHOT_LANG` 覆盖：以 `zh` 开头选中文，其它非空值选英文。语言在 helper 启动时确定，切换需重新运行。Rust CLI 的 `--help` 走同一套判定，`VSHOT_LANG=zh vshot --help` 即中文。

## OCR 取字
`vshot ocr` 把屏幕上某块区域的文字读出来。不给参数就是框选：

```sh
vshot ocr                    # 框一块区域，文字到 stdout
vshot ocr --json             # 同上，但把每行文字和每个字的位置输出成 JSON
vshot ocr --clipboard        # 同上，进剪贴板
vshot ocr --geometry '0,0 800x200'
vshot ocr --input shot.png   # 读一个已有的图片文件
```

识别完默认会弹一条**桌面通知**：成功给出识别到的文字（长了截断到 160 字），失败给出原因——`vshot ocr` 被快捷键拉起时，没有别的东西告诉你它跑完了。通知交给会话总线上 `org.freedesktop.Notifications` 的服务去画，**没有通知服务也照样能用**，只是少这条提示；开关在设置窗口的「文本识别」页，也可以手改 `cli.ocr.notify`。

剪贴板里放的是识别出的文字本身，**不会多出一个结尾换行**；只有输出到 stdout 时才补一个，免得终端的提示符挤在最后一行文字上。

**`--json`** 不打印纯文本，而是把识别到的每一行连同每个字的位置写成 JSON 输出到 stdout，让程序自己摆放这些文字而不是读它们。它与 `--clipboard` 互斥，并且**不弹桌面通知**——要 JSON 的调用方是程序，不是人。

编辑器里「取字」也识别选区里的文字，但不把整段丢进剪贴板：识别到的文字会变成**一层可就地选取的文字层**，画在字符原本所在的位置，**一进来就是全选**——最常见的情况（整段都要）按一下 **Enter** 就拿走。在文字上拖拽可以缩小范围，双击取指针下的词，三击扩到整行，**Ctrl+A** 全取，**Enter** 或 **Ctrl+C** 只把选中的那部分复制走并退出该模式，**Esc** 退出该模式回到普通编辑（它**不取消**这次截图，再按一次 **Esc** 才是取消）。按钮自己会说明进展：识别期间显示「识别中」，随后是「已复制」或「失败」。该模式期间不提供工具栏，因为这一层描述的就是这次截图自己的选区，换工具会让它失效。

**外接引擎**（GPU 那条路）只报文字、不报每个字的位置，没有东西可选：它的整段文字照旧复制走，并往 stderr 写一句说明。

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

## 翻译
`vshot translate` 把屏幕上某块区域的文字翻译成另一个语种。不给参数时走 Qt overlay 的翻译模式：框一块区域，松手就识别、翻译，译文画在原文上；Enter 确认（Esc 撤下译文后可再 Enter 重翻这块框）：

译文是**就地盖在原文上**的：每行的底色取该行上下各 2 像素里最常见的颜色，所有底色先铺完再写文字——后一行的底色不会盖掉上一行的字；字用**桌面自己的字体**（Qt 应用字体，也就是系统字体），只有当它画不出译文里的字时才回退到内置的 CJK 字体表。译文放不下时先缩小字号（下限为行高的一半），再放不下就把底色块横向加宽，而不是溢出画面。

```sh
vshot translate                    # 框选，译文上屏；Enter 后译文到 stdout
vshot translate --output out.png   # 把合成好的译文 PNG 写到文件
vshot translate --clipboard        # 把合成好的译文 PNG 复制进剪贴板
vshot translate --input shot.png   # 读一个图片文件，翻译它识别出的文字
vshot translate --geometry '0,0 800x200'
vshot translate --to en --from ja  # 指定目标 / 源语言
vshot translate --provider bing    # 换一个翻译服务
vshot translate --provider auto    # 按顺序挑可用的翻译服务
```

`--stdin-ocr` 是编辑器用的原语：它从 stdin 读 `vshot ocr --json` 打印的 **OCR JSON 信封**，只翻译其中的 `lines[].text`，再把一个**翻译后的信封**写到 stdout——不截图、不跑 OCR、不碰合成器，也**不弹通知**。编辑器就是这么调它的：

```sh
vshot ocr --json | vshot translate --stdin-ocr --json
```

`--json` 把信封原样写回：`lines[].text` 换成译文，原文留在同一条的 `source` 里；`geometry` 与每行的 `rect` 都保留，`chars` 丢掉（逐字框只用来**选取原文**，翻译浮层不选原文，所以不需要）——`geometry` 仍为 `true`，因为每行的 `rect` 是真的、也是 Qt 文字层摆放译文要用的。这正好是编辑器文字层已经在解析的形状，译文因此落在原文的位置上。某一行翻译失败时，这一行仍在：`text` 保持原文，另加一个 `error` 字段——**一行坏掉不会丢掉整批**。`--stdin-ocr` 这一路**总是**输出信封，`--json` 加不加都一样——它本来就隐含 `--json`。

不带 `--json` 时打印译文，一行一行，和 `vshot ocr` 打印识别结果的方式一样。`cli.translate.notify`（默认 `false`）控制翻译结束时那条桌面通知；`--stdin-ocr` 这一路永远不弹。

### provider
九个服务由 `--provider` 或配置里的 `cli.translate.provider` 选，其中**五个不用你自己的密钥**：

| provider | 需要的凭据 | 说明 |
| --- | --- | --- |
| `google` | 无 | 官方 Android 应用用的免密钥接口；每行一次请求（4 路并发）再按序拼回 |
| `microsoft` | 无 | 微软 edge 的免密钥接口；一次请求可带多行（JSON 数组），`from` 留空即自动识别 |
| `volcengine` | 无 | 火山翻译；发出伪装成厂商自家 Chrome 扩展的请求，一次只翻一行 |
| `transmart` | 无 | 腾讯 Transmart；一次请求可带多行（`text_list`） |
| `lingocloud` | 无（借用彩云的 token，可用 `token` 覆盖） | 彩云小译；一次请求可带多行，`trans_type` 是 `<from>2<to>`；简体中文要写 `zh`，繁体 `zh-Hant` 照收 |
| `bing` | `api-key`（可选 `region`） | Azure Translator，一次请求可带多行 |
| `baidu` | `app-id` + `secret-key` | 百度翻译，多行用 `\n` 拼成一个 `q` 一次发，`sign` 是 `md5(appid + q + salt + secret_key)` |
| `ai` | `endpoint` + `api-key` + `model` | 任意 OpenAI 兼容的 chat 接口；`prompt` 可覆盖内置的系统提示词 |
| `external` | 无（用你自己的程序） | 从 stdin 读原文、按行把译文写到 stdout |

`google`、`microsoft`、`volcengine`、`transmart` 这四个都免密钥：不要账号、不要密钥，配置里什么都不用填。`lingocloud` 也不用你配任何东西，但它跑在**从彩云 web 应用借来的 token**（`9sdftiq37bnv410eon2l`）上——这个常量出现在几十个第三方客户端 fork 里，彩云随时可能吊销它。要停止借用，就在配置里写 `cli.translate.lingocloud.token` 放一个你自己的彩云 token；真被吊销时改配置即可，不用改代码。

`bing` 的 `endpoint` 默认 `https://api.cognitive.microsofttranslator.com`；`ai` 的 `endpoint` 以 `/chat/completions` 结尾时直接用，否则拼上 `/chat/completions`。密钥配错了会**明确报错**并指名缺哪一项，而不是悄悄换一个 provider。

`--provider auto`（或配置里的 `"provider": "auto"`）不指名任何服务：它按内置顺序 **google、microsoft、volcengine、transmart、lingocloud、bing、baidu、ai、external** 依次试，先试不用凭据的五个——`lingocloud` 排在这五个的最后，因为它借的是别人的凭据；跳过没有东西可跑的——`bing`、`baidu`、`ai` 要各自的密钥，`external` 要命令，而那五个永远能跑。谁先给出结果就用谁，信封里的 `provider` 报的是真正跑的那个。

`cli.translate.fallback` 是一个可选的数组，列出主 provider 之后要接着试的 provider（主 provider 是 `auto` 时，就排在整条 auto 顺序之后）。它**默认是空的**，不会跑任何你没写的东西。一个 provider 只要**至少翻出一行**就算给出了结果；每一行都失败时——就是被限流的 Google 那种情况，每行各自带着 `error` 而命令仍然退出 0——就换链上的下一个；一旦有一行成功，整份结果就原样采用，包括其中失败的行。两个及以上 provider 的链整条都失败时，报错会指名每一个试过的 provider 及它最后给的原因。和链上已有的重名会被跳过，不认识的写法会报错。

**没有 `fallback`** 时链上只有一个 provider，它就不算一条链：结果照旧返回，包括逐行的 `error` 字段，所以单跑 `--provider google` 的行为一点没变。链和它那条「全都失败」的报错，只有在你写了第二个 provider 之后才存在。

`external` 是唯一的万能逃生口——只要一个 shell 脚本就能用：

```json
{"cli": {"translate": {"provider": "external",
                       "external": {"command": ["/usr/local/bin/my-translate"], "timeout": 30}}}}
```

它把原文按行写到子进程的 stdin，从 stdout 读回译文；**行数必须和输入一致**，多一行少一行都会报错并说明期望与实际的行数，同时带上程序写到 stderr 的内容。超时默认 30 秒，到点杀掉子进程。

### 语种代码
语种用 vshot 自己的 BCP-47 风格写法：`zh-Hans`、`zh-Hant`、`en`、`ja`、`ko`……`auto` 表示自动识别。每个 provider 有自己的代码，vshot 在发请求前换算：

- **Google** 用 `zh-CN`/`zh-TW`，识别写作 `auto`；
- **Microsoft** 用 vshot 原样的标签（`zh-Hans`/`zh-Hant` 都照收），识别是 `from` 留空；
- **火山（volcengine）** 多数标签原样透传，但**简体中文要写 `zh`**（实测 `zh-Hans`/`zh-CN`/`zh-TW` 会让它回英文），繁体 `zh-Hant` 照收；请求根本不带源语言字段，识别始终是隐式的；
- **腾讯 Transmart** 把 `zh-Hans`/`zh-Hant` 都并成 `zh`；源语言按调用方给的原样下发，**`auto` 也一样**（实测该接口接受 `auto` 并自动识别；反倒是按某些客户端那样发 `en`，遇到非英文原文会原样返回、不翻译）；
- **彩云（lingocloud）** 把两个标签拼成 `<from>2<to>`：**只有 `zh-Hans` 会被改写成 `zh`**（该接口把 `zh-Hans` 当作源和目的都拒收），`zh-Hant` 原样下发且**确实回繁体**（实测 `ja2zh-Hant` 给的是「今天天氣真好啊。」，不是简体；`zh-Hant2ja` 则被服务内部归一成 `zh2ja`）；其余标签——包括旧客户端只认的六种之外的 `de`、`ko`——全部原样传下去，由服务自己的 `rc=-1` 报错；
- **Bing** 用 vshot 原样的标签，识别就是**不带** `from` 参数；
- **百度** 用 `zh`/`cht`/`jp`/`kor`/`fra`，识别写作 `auto`；
- **`ai` 与 `external`** 原样透传。

**表里没有的标签原样传下去**，绝不悄悄丢掉——不认识的语种宁可让服务自己报错，也好过用错误的语言翻译。

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
vshot record monitor --fps 120                  # 瞄准 120fps（至少 1，没有上限，默认 60）；`--duration 60` 则 60 秒后自动停
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
- **录制怎么停 / 输出到哪 / 帧率**：`vshot record stop`（不需要显示器，可直接绑快捷键）或启动它的终端里按 Ctrl+C，两种方式都会先把文件正常收尾再退出，所以文件总是可寻址的 MP4；`--duration` 让它自己到点停。全局 `-o/--output` 展开 strftime，不给时写入视频目录下的 `vshot-%Y%m%d-%H%M%S.mp4`——视频目录先取 `$XDG_VIDEOS_DIR`，再取 `~/.config/user-dirs.dirs` 里 xdg-user-dirs 记的那个（中文桌面通常是 `~/视频`），最后才是 `~/Videos`；目录不存在时会建出来，你自己用 `-o` 指的那一层也一样建。名字没有 `.mp4` 后缀会自动补上，`-`（stdout）不接受。`--fps` 是循环"瞄准"的速率，每帧带着它在屏上的真实时长写入 MP4（可变帧率），所以编码跟不上时回放是"少几帧"而不是"变慢"；单块 4K 实测能稳定跑满 120fps。
- **编码器（`--encoder`）**：h264（默认）、hevc、av1 三选一，都跑 GPU 的媒体引擎。编码与封装走 ffmpeg 的库（`libavcodec` + `libavformat`，与 wf-recorder 同一条路线），运行时用 `dlopen` 加载，所以没有 ffmpeg 库的机器上截图照常工作，只是 `record` 会说明缺什么；需要 `ffmpeg` 与 `libva`（AMD/Intel 的 VAAPI）。码流每帧都是 IDR（全帧内），任意播放器可读，任何位置都能跳。
- **编码后端（`--encoder-backend`）**：`auto`（默认）、`vaapi`、`vulkan`、`nvenc` 四选一。`auto` 按**零拷贝优先**试：VAAPI（AMD/Intel）→ Vulkan → NVENC；写死某一个时只用那个，打不开就明确报错并说明是哪一层失败（例如 `no CUDA device for NVENC`）。**每条路都是硬件编码**（`h264/hevc/av1_vaapi`、`_vulkan` 或 `_nvenc`），vshot 里没有任何 x264/x265 之类的 CPU 编码路径；差别只在**帧怎么送到编码器**：VAAPI 与 Vulkan 都能导入合成器的 dma-buf，全程零拷贝，而 **NVENC 没有 dma-buf 导入**，它的帧要经 CPU 搬一趟（所以 CPU 占用略高，编码本身一样在 GPU 上）。**NVIDIA 上要零拷贝就得选 `vulkan`**：那里的 `*_vulkan` 底层就是同一颗 NVENC 硬件单元，而 `*_nvenc` 那条路永远拿不到 dma-buf。多显卡机器上 NVENC 默认用第一个 CUDA 设备，`VSHOT_NVENC_DEVICE=<索引>` 换一个；Vulkan 默认用第一个物理设备，`VSHOT_VULKAN_DEVICE=<索引>` 换一个。NVIDIA 机器还需要 `ffmpeg` 构建时带 `nvenc`（多数发行版默认带）。
- **麦克风（`--mic`）**：把麦克风录进同一个 MP4，编码是 AAC（ffmpeg 自己的编码器）。不带名字用会话的默认输入设备，给名字或节点序号录别的输入（`wpctl status` 列得出来，序号比如 `--mic 55`）。麦克风在视频编码器之前打开（采样率与声道数要写进 MP4 文件头），样本在每个视频帧后抽干一次，两条轨共用一个时钟。`--no-mic` 在配置文件记着 `cli.record.mic` 时也强制不录；两个都不给就是"按配置"（默认不录）。麦克风走 PipeWire，所以和 `--portal` 一样需要 libpipewire；没有默认输入设备的会话会明确报错并提示看 `wpctl status`。
- **逐应用音频（`--app-audio`）**：额外录一段所录那扇窗**自己在播的声音**——别的应用在响什么都不会进去。只在 `vshot record window` / `vshot replay start window` 上有意义（其它目标会被拒绝）。它可以**独立使用**，也可以**和 `--mic` 同时给**：麦克风是房间、应用音频是窗口，两者在 Rust 侧**逐样本相加**成 MP4 的那**一条**音轨（不是两条轨道）。窗口的 pid 由合成器报告（Hyprland、niri 会给，KWin 由 scripting 探针报），vshot 拿这个 pid 去 PipeWire 的客户端表里找到该进程的播放节点，只连那一个；Sway 与 labwc 不报窗口 pid，会明确说明做不到，而不是悄悄退回麦克风。窗口没在放声音时保留原有音源（首次录制则只录视频），而不是报错。`--portal` 与 `--app-audio` 互斥；音轨同样是 AAC。
- **跟随焦点（`--follow`）**：给若干窗口（`--follow NAME`，可重复），录制就跟着焦点在这些窗口之间移动——焦点落在其中哪扇就录哪扇，落在别处时**保持录上一扇**（不中断、不留空档，也不会把那扇窗录进来）。**回录读的是同一份名单、做法略有不同**：焦点不在名单里时它**继续录当前这一扇**（和录像的单向跟随一样），名单里的窗口**全都关掉**才是待机（没有可录的窗口就不录）；焦点换到名单里的另一扇时它**按那扇自己的尺寸重开一段**，而不是把画面缩进旧画布——见[「窗口回录：待机与按窗口重开」](#窗口回录待机与按窗口重开)。`--follow` 与窗口 `NAME`、`--pick` 互斥（它自己就是选窗方式），只对 `record window` 与 `replay start window` 有效。**不带 `--follow` 的 `record window` 会跟随配置里 `cli.record.follow` 记着的窗口**（回录对应 `cli.replay.follow`），`--no-follow` 则对那一次显式关掉。换源走的是**和窗口缩放同一条路**：新窗口的画面等比缩放进开录时的画布，所以**一条 MP4 只有一个帧尺寸**、时间线连续。`--app-audio` 时音轨也跟着换到新窗口的声音，而麦克风那一路**不动**；新窗口的应用若没在放声音，保留上一扇的，不退回麦克风。焦点查询每 250ms 一次，只在给了 `--follow` 时才问合成器；合成器报不出焦点时会**明确报错**而不是永不切换。注意跟随的是**焦点**，不是"正在玩的游戏"：切到不在白名单里的窗口时录制停在上一扇，但那一瞬间的操作不会被收录。
- **宽度上限 4096**：这是硬件 H.264 编码器的限制（本机 7900 XT 的 VCN 实测如此），所以两台 4K 屏拼合出的 `record all`（5760 宽）会被拒绝并说明原因——录单块屏即可，或改用 `--encoder hevc`（本机可编 7680 宽的全桌面）。单块 4K（3840）没问题。
- **记住的默认值**：`--encoder`、`--encoder-backend`、`--fps`、`--portal`、`--mic`、`--follow` 不写时，去配置文件的 `cli.record` 段取值（`replay` 对应 `cli.replay` 段，见 [`cli`——命令行默认值](#cli命令行默认值)）；`--no-portal`、`--no-mic` 与 `--no-follow` 是对**那一次**录制把记着的值关掉。`cli.record.follow` 只在**不带窗口名、也不给 `--pick`** 的 `record window` 上生效，别的目标会忽略它（而不是报错）。`vshot record mics` 列出这次会话里能录的音频输入，每行是 `节点序号	节点名	说明`（节点名就是 `--mic` 收的值），设置窗口的麦克风下拉读的正是它。`--app-audio` 只认命令行，不进配置文件。
- **`--portal`：走桌面 portal 录制**（`org.freedesktop.portal.ScreenCast`）。合成器自己的捕获协议各自只在部分桌面存在，而 portal 是每个桌面都有的那一套——代价是**由合成器决定录什么**：它会弹出自己的选择器，在那里选中的屏幕/窗口就是录下来的内容。`record monitor --portal` 让它列屏幕，`record window --portal` 让它列窗口，但名字与 `--pick` 决定不了具体是哪一个；每开一个会话都会弹一次选择器。一次只录一路流，所以 `record all --portal` 会被拒绝。屏幕投射是**按需出帧**的（画面变了才出一帧，所以静止画面在文件里是一帧长帧），`--fps` 是向合成器要的采样上限而不是文件帧率。帧以 dma-buf 送来时零拷贝进编码器，否则在 CPU 上转成 RGBA；`VSHOT_PORTAL_SHM=1` 强制走内存拷贝那条。需要 `xdg-desktop-portal` 与 `libpipewire`（编译要有头文件，库运行时 `dlopen`），缺了只是没有 `--portal`，其他功能照常。

## 回录
`vshot replay` 是「一直在录，但不落盘」的录屏：屏幕持续编码，编码后的包进一个**内存环**，`vshot replay save` 把环里现有的内容拷成一个 MP4 —— **流拷贝，不重新编码** —— 所以触发几乎不花时间，触发之前不写任何东西到磁盘。适合打游戏时挂着，出精彩操作按一下就把刚才那几十秒留下。

```sh
vshot replay start monitor --background     # 后台挂起，默认留最近 30 秒
vshot replay save                           # 落盘到视频目录，带时间戳
vshot replay save /tmp/clip.mp4 --seconds 10
vshot replay status                         # 环现在覆盖多少秒、已服务多少次保存、在录还是待机
vshot replay stop
```

`start` 的目标和 `record` 完全一样：`monitor [NAME]`（不写即你当前所在的那块）、`all`、`region`（`--geometry` 或冻结桌面上拖出）、`window`（焦点窗口、按名字、或 `--pick`）。帧走相同的捕获后端、相同的 libavcodec GPU 编码器，`record` 用零拷贝 dma-buf 的地方回录也用。

回录用 `--gop` 秒的关键帧间隔（默认 1）而不是录制那样的全帧内编码，环因此只是全帧内码流的一小部分；保存从「不晚于 `now - seconds` 的最后一个关键帧」起头，所以文件至少有你要求的秒数、并且从第一个字节就能解码（要的比环里有的还多就给全部）。环因此要多留一个 GOP，否则会正好把要用的关键帧丢掉。

### 窗口回录：待机与按窗口重开

窗口回录（`replay start window`）和屏幕回录不一样：**一个环只有一个帧尺寸**，所以它不会把另一扇窗的画面缩进已有的画布里，而是**换一扇就换一段**。

- **切窗口 = 重开**。焦点移到 `--follow` 名单里的另一扇窗时，当前的采集会话、音频与环一起丢掉，按新窗口**自己的尺寸**重开一段——所以 4K 的那扇不会被压进小窗口的画布。代价是**旧窗口环里的内容随之丢弃**：切换后立刻 `save` 只能拿到新窗口的这一小段。
- **焦点跑掉不算待机**。焦点落到名单外的窗口（或桌面）上时，回录**继续录当前这一扇**——和录像一样是单向跟随：你切出去查攻略的那一下仍在窗口自己的像素里。名单里只要有**任意**一扇窗开着就录（优先录焦点那扇），所以 `replay start window --follow A --follow B` 可以先跑起来再去开游戏：A 或 B 一出现就开始录，窗口全程没出现过也一样，它等，不会报错退出。
- **待机 = 名单里的窗口全关了**。所有 `--follow` 窗口都被关掉（或合成器结束那次采集）时回录才进入待机：不建采集会话、不开编码器、不占显存，只留控制 socket 和最近一扇窗的环（`save`/`status`/`stop` 照常能用），名单里任何一扇回来就自动接上，适合游戏崩溃/重启、显示器关掉又开。不带 `--follow` 的窗口回录同样如此：它等的是**命令里点的那扇窗**回来（按名字/标题找，游戏重启换了 toplevel 也能接上）。只有**第一次**开不起来才算命令失败（名字打错、编码器不支持那一档），之后的问题都只让它回到待机。
- **待机时的 `save`/`status`**：**待机不丢历史**——最近一扇窗留下的那几秒还在环里，`save` 照样把它写成文件（只有还没录过任何窗口的会话才会明确回答「没有可存的内容」，而不是写一个空文件）；`status` 打印 `秒数	保存次数	live|idle`，`idle` 表示此刻没在录，前面的秒数是上一扇窗留下的。
- 回录一次只跑一个会话，所以「两个游戏各挂一个环」做不到；要同时留两扇窗，用 `--follow` 让它在两者之间切换（切过去就重新开始攒）。

### 参数

| 参数 | 说明 |
| --- | --- |
| `--window N` | 内存里保留多少秒（1–3600，默认 30）。不写时跟随配置里的 `cli.replay.window`。窗口回录里这段历史属于**当前那一扇窗**：切窗口后重新从零攒，待机则保留它留下的那一段（`save` 仍写得出来） |
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
- 右键点击**任意** pin 弹出菜单：色卡在前几行列出它的各种格式，点哪一项就把那个值抄回剪贴板（↑/↓ 选行、回车抄走、Esc 关闭；复制成功后右下角闪一个徽标）；后面六行对所有 pin 都在：`复制图像`把 pin 自己的像素（含标注）作为 PNG 放进剪贴板；`另存为…` 弹出保存对话框把这张图写成 PNG，存完在右下角报结果；`编辑`打开标注编辑器，接着在它原有的标注上画；`重置缩放`把被滚轮改过大小的 pin 恢复到它刚钉上来时的尺寸；`取字…` 打开 pin 编辑器并直接进入取字模式（见[「OCR 取字」](#ocr-取字)），钉住的图也能这样读字；`关闭`关掉这个 pin（双击它也是同一个效果）

新 pin 落在**激活的输出**上：指针所在的那块屏优先，指针读不到时退回键盘焦点所在的输出，都没有则回退主输出。**区域与窗口的截图 pin 回它被拍下来的位置**：截图中带上了那块矩形在桌面上的全局坐标，pin 就严丝合缝地落在原地（既不居中也不级联），于是把一扇窗 pin 在自己头上不需要再拖。它的外观与其余 pin 一样，描边与阴影都按配置画（把设置里的「边框宽度」调成 0 就是不画边框）——那道边框正是告诉用户桌面上那东西是 pin 而不是原窗的依据。文件、剪贴板、色卡与整屏截图没有“原位”（整屏那一份落在它的输出上本就和居中重合），才落在激活输出的中间。pin 的对象：

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

- 编辑器覆盖 pin 所在的整块屏幕，并且**自己把 pin 那张图画进它自己的表面**（和区域编辑一样）：以前它不画，靠下面那个真实 pin 的窗口透出来显示，但**透明的表面并不拥有透出来的像素**——那是合成器放上去的，合成器有权放别的东西：只要某条 layer rule 对 alpha 超过阈值的 layer surface 开模糊，取字高亮那条半透明条下面的字就会被糊成一片颜色（高亮之外的地方因为表面全透明反而不受影响）。表面自己带像素就改不了，这正是区域编辑一直没这个毛病的原因；画上去的那份是不透明的、只盖住 pin 的图像矩形，所以屏幕上依旧只有一张图，pin 的边框和阴影在矩形之外、还是 pin 自己的。超出图片的标注会被裁掉；选中工具在图片上拖动时通过 daemon socket 复用 pin 自身的移动逻辑，**不产生副本**，方向键可微调（Ctrl 加速为 10px），图片不会被拖到屏幕外。用方向键走光标时，屏幕上那个指针同样会**真的跟着走**：pin 编辑器的会话只描述 pin 那一块屏，所以 daemon 另外把整个桌面布局的包围盒写进会话，CLI 才按正确的坐标系换算——否则指针会落到实际位置的若干分之一处；
- 编辑器给图片描的那圈边框**只在这张 pin 是活动的那张时画**。它和选区框是同一套 Qt chrome，而其他 pin 都是低一层的 Wayland surface 画的，同一层的 surface 按映射顺序堆叠、协议没有 restack——所以当用户点到别的 pin 上、这张不再是活动的那张时，这圈边框就会压在其他所有贴图之上，这不是顺序能解决的问题。daemon 知道哪张是活动的（该 surface 持有键盘且指针在它上面），于是通过 pin socket 把答案发给编辑器（`watch-active` 订阅），编辑器据此收起边框；**编辑本身不受影响**——标注、输入区域、拖动照旧，用户点回这张 pin 时边框回来；
- **点回这张 pin 会把 pin 提回顶层**。平时的「点一下就把 pin 提到最前」是 pin 自己的 surface 在做的，而编辑期间编辑器的 layer surface 覆盖整块屏并持有键盘，点击根本到不了下面那张 surface——所以编辑器在按下时替用户向 daemon 发一条 `raise`（在图片上和边框上按都算）。没有这一步，用户点回正在标注的图时边框是回来了，图却还压在别的 pin 底下；
- 编辑期间的手势和区域编辑是同一套：**中键**拖图片把底图挪到标注下面（按住 `Ctrl` 拖也一样），按住 `Ctrl` 拿起指针下的标注，按住 `Shift` 保持比例，方向键微调（`Ctrl` 一步 10 逻辑像素），**右键点一下**就是取消这次编辑（等同 Esc）；**双击在这里不确认**——文字标签上的双击是就地改字，确认始终是 Enter 或 OK。手势全表见[上面那张](#区域截图与标注编辑)
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

## 屏幕标注
`vshot annotate` 把桌面变成一块可以随手画的画布：一个**常驻 daemon** 在每块输出上铺一张 layer-shell 浮层（Overlay 层，与 pin 浮层同一套），鼠标直接画在**实时桌面**上。每块输出有自己的画布、自己的工具栏，画在哪块就留在哪块。首次 `show` / `toggle` 连不上 socket 时，daemon 由 `vshot-qt-ui --annotate-server <绝对 socket 路径>` 分离式拉起，与 pin daemon 一样；`show` 与 `toggle` 会顺手拉起 daemon，`hide`、`clear`、`quit`、`status` 都不会。

```sh
vshot annotate toggle     # 显隐浮层（没有 daemon 时顺手拉起）
vshot annotate show       # 显示（同样会拉起 daemon）
vshot annotate hide       # 隐藏，画的东西留着
vshot annotate clear      # 清空每块输出上的全部标注
vshot annotate quit       # 退出 daemon，连标注一起
vshot annotate status     # daemon 在不在、握着几条笔画
```

工具栏是沿该输出**顶边居中**排的一小条横向调色板，左边一个把手可以把它拖到任意位置，内容依次是 Draw（自由画笔）、Erase、Rect、Arrow、Text、六个颜色、三档线宽、Undo、Redo、Clear，以及最右边那个退出 daemon 的 ✕。**Erase 拖过哪条笔画就删掉哪条**（可撤销），不是擦像素；**Text** 在你点击的地方开一个内联文本框，Enter 落笔、Esc 取消，输入法打出的中文照样收下。撤销/重做也管擦除与清空，所以误按一次 Clear 只需一次撤销。

浮层显示期间它会**吃掉那块输出上的每一次点击**——「随便画」就是这个意思——所以要拿回指针，得在合成器里另绑一个跑 `toggle` 或 `hide` 的快捷键（Wayland 客户端收不到全局按键；KWin/Plasma 建一条自定义快捷键，niri 写一条 `binds` 条目）。浮层**刻意不抓键盘**：只有内联文本框会临时升起键盘交互性，关掉就还回去，所以在别的窗口里打字照常。

```
bind = SUPER, A, exec, vshot annotate toggle
bind = SUPER SHIFT, A, exec, vshot annotate quit
```

标注是**要入镜**的：截图或录屏会自己把**工具栏**藏起来（画的笔画不藏），所以冻结下来的画面上有笔画、没有工具栏；就算录制进程被杀掉，daemon 盯着请求方的 pid，也会把工具栏还回来。**回录（`vshot replay start`）不在这个范围内**——它一开着就持续抓帧，整个会话都藏工具栏会让浮层没法用，所以标注和回录本就不打算一起跑。这套功能只在 **Hyprland** 上实测过，且和 pin 浮层一样需要 `wlr-layer-shell`，没有 layer shell 的合成器根本画不出来。最后是和 pin daemon 一样的警告、一样的道理：**不要用 `pkill` / `kill -9` 结束标注 daemon**——它持有 layer-shell surface，被强杀时部分合成器会残留已映射的 surface 卡住 screencopy（pin daemon 上在 Hyprland 0.56 实测过）。请用 `vshot annotate quit`，它退出前会 unmap 全部；daemon 也已处理 `SIGTERM` / `SIGINT` 走同一条路。

## 图像与输出映射
**落在单个输出内的矩形一律从那块输出自己那份原生帧裁剪，并按那块屏的 scale 写密度**：`region` 的 `--geometry` 与交互选择、`window active`、`window pick` 以及像素识别给出的矩形都走这条路，只有**跨接缝**的矩形才回落到合成场景；`all` 是整块桌面，只能由场景给出。内部帧统一为 RGBA8、top-left origin，多输出合成支持负 logical origin 和输出间空隙（场景画布用最高输出 scale，较低 scale 的输出用 nearest-neighbor 放大）。当前要求正整数 scale、`transform=normal` 以及可安全证明的 logical/pixel 映射；fractional scale、旋转和无法证明的映射会清晰失败，而不是生成疑似错误的截图。这个校验只在**需要把输出合成为场景**的路径上生效，所以 KWin 与 niri 直接给窗口像素的两条路在旋转/翻转输出上仍然可用。

### HDR
**这一节没有经过真机验证。** 写它的机器上既没有 HDR 显示器，也没有能给出 HDR 输出的合成器，所以下面描述的路径**一次都没有在真实的 HDR 内容上跑过**：协议交互与像素换算只有离屏检查（`vshot-pin-hdr-check`、`vshot-backdrop-check`）覆盖，HDR 捕获、色调映射、`.hdr` 的写出和 HDR pin 都是照着协议写出来的，没有实测。SDR 输出不受影响——合成器不描述 HDR 时不写 `.hdr`，也不铺 backdrop surface。

输出自己被合成器描述为 HDR（PQ 或 HLG）时，一次截图产出**两份**：`<name>.png` 是同一份内容的 SDR 色调映射，第二份是 HDR 内容本身，与 PNG 同名、只有后缀不同（默认 `<name>.avif`；`--hdr-format hdr` 改成 Radiance RGBE 的 `<name>.hdr`）。有标注时标注在**线性光**里合成到 HDR 那一份上，SDR 那一份再由它映射而来，两份因此描述同一束光、同一批标记。SDR 那一份**不用合成器给普通客户端准备的那张画面**：Hyprland 把它按 `DEFAULT_SRGB_IMAGE_DESCRIPTION`（峰值 80 cd/m²）写出，而一张 SDR 图里「白」的含义是这块输出自己的参考白（这里是 203），于是普通 SDR 内容会被压到白以下而不是落在白上——实测取到的是 sRGB 220 而非 255（145 cd/m²），整幅画面一起变暗，而不只是高光。VShot 因此自己映射。**画面里没有超过 SDR 白的东西时，它就是一整幅 SDR 图，按原样显示**：白落在 255，白以下的每个码值都保持它自己的光。只有画面里确实有超过白的光时，白点才下移——移到 `SDR_WHITE_LEVEL`（0.8），把白以上的码值腾出来装高光——这时 SDR 白以内的光按这个系数等比落码，比例原样保留、色相不变；超过 SDR 白的光再按 Reinhard 滚降到 `ROLL_OFF_PEAK`（10 倍 SDR 白）处的满量程，单调且分离，一块更亮的高光不会被抹成纯白（8 倍 SDR 白落在 252，与白相差 21 个码值）——这正是过去按峰值归一那套做不到的：它把超过白的一切压成同一个码，testufo 的 HDR 测试里 `HDR`/`WCG` 字样于是与周围的 SDR 再也分不出来。代价是白点成了画面的属性而非固定值：同一束光会因为画面里有没有高光而落在不同的码上。这是与另一种取舍相反的取舍——固定白点会把 HDR 输出上**每一次** SDR 截图都压暗到 sRGB 231，也就是一张 SDR 窗口的截图出来是错的。**色域按输出自己报的色度坐标换算**：Hyprland 对一块 P3 面板报出的坐标既不是 BT.709 也不是 BT.2020，按「更接近哪个」去猜会把整幅画面的颜色算错；落在 BT.709 之外的颜色按**朝白点去饱和**映射进去，而不是把负分量截成 0——截断会挪动色相，去饱和只丢装不下的彩度。内容本身没有超过 SDR 白时不会写第二份——这一判定见下一段。

**「画面里有超过 SDR 白的东西」怎么判定**：判定发生在程序内部，看的是 daemon 手里那份帧本身，而不是任何一个外部文件，所以标签、第二份文件、pin 的走法三者永远同进同退。外层的闸门是**输出的声明**：只有合成器把这块输出描述成 PQ 或 HLG 才会拿到 10-bit 缓冲区，SDR 输出上根本不存在第二份可写。声明之后要答的是另一个问题——**这个矩形里有没有超过白的光**——而声明答不了它：同一块 HDR 输出上的一个 SDR 窗口和一段 HDR 视频，声明完全一样。于是按**面积**判定：超过 SDR 白 `HDR_WHITE_EPSILON` 的像素占画面的比例达到 `--hdr-area-ratio`（默认 0.0005，万分之五）才算 HDR 内容。为什么不看最亮的那一个像素：10-bit PQ 会把**普通的 SDR 白**落在略高于 1.0 的码上（203 cd/m² 参考白下，码 594/595/596/597 解出 0.99958/1.00897/1.01845/1.02801），所以一张再普通不过的 SDR 桌面也散着几千个「超过白」的像素——实测一幅 3.7 MP 的桌面有 17 489 个像素超过 1.02；老判据是「峰值超过 1.02 就算 HDR」，只要有**一个**这样的像素，整幅画面就被判成 HDR，白点随之下移，**整幅截图暗约 18 %**（实测均值 49573 对 60606）。把阈值抬到 0.05 也救不了：同一幅画面仍有约 37 个像素在它之上（实测占比 0.00001），照样超过任何「有就行」的判据。真正的 HDR 是一块**成片**的高光，量化噪声是**散落**的，所以判据是占比而不是峰值。两个开关可以把它调回原样：`--hdr-area-test false` 退回「有一个像素超过就算」（老行为）；`--hdr-area-test true --hdr-area-ratio 0` 则是「只要是 HDR 输出就一律按 HDR 处理」，不做任何区域判定。阈值本身（`HDR_WHITE_EPSILON`，0.05）不是配置项：它要跨过 10-bit PQ 的量化台阶，而 1.5 倍白的高光解出来就是 1.5，离它很远。

第二份的格式由 `--hdr-format`（或配置文件里的 `cli.hdr-format`）决定，默认 `avif`：10-bit、BT.2020 + PQ，AV1 序列头与容器的 `colr` box 声明同一个 CICP 三元组，所以任何懂 AVIF 的读取器都能正确显示，代价是**有损**。`hdr` 则是 Radiance RGBE，**原样保留截取时的色域**（不做任何转换，广色域留给它），色域写在 `PRIMARIES=` 头里——RGBE 本身没有色度字段，而 ffmpeg 与 ImageMagick 都会忽略这一行、按 Rec.709 解读，所以用这类工具看广色域内容会偏艳；为它们转换过的是旁边的 SDR 那一份。要无损归档就选它。AVIF 走 `rav1e` + `avif-serialize`：`image` 自带的 AVIF 编码器写不了 HDR（它固定 8-bit，且色彩描述固定为 sRGB / BT.709），详见 `src/model/codec/avif.rs`。

**读回来和写出去走同一层**：每种格式是 `src/model/codec/` 下的一个模块，实现同一个 `HdrCodec`（`encode`/`decode` 一对，路径形式有默认实现），都读写同一个中间值 `HdrImage`（线性光帧 + 它的参考白）。所以「编码再解码」是一次往返而不是两套换算，`pin` 读回磁盘上的第二份文件时用的就是写它的那个 codec。每种格式是一个 **cargo feature**（`avif`、`radiance`，默认全开），关掉哪个就不注册哪个——`avif` 是唯一有构建代价的（`rav1e` + `avif-serialize` 写、`dav1d` + `mp4parse` 读，`dav1d` 链接系统的 `libdav1d`）；`avif-asm` 再打开 rav1e 的手写 SIMD（需要 `nasm`）。至少留一个格式，一个都不留会在编译期报错而不是留到第一次截图才 panic。颜色换算（传递函数、YCbCr 矩阵、full/limited range、位深）在 `src/model/color.rs`，codec 只声明自己的字节是哪条曲线、哪个矩阵。

**参考白**：一帧的 `1.0` 是「截它时那块输出的 SDR 白」，所以一份 HDR 文件必须能说出它的白是多少 cd/m²。AVIF 没有这个字段，本程序写一个 `vshot.refwhite01` 私有 `uuid` box 进 `meta`（不认识的读取器跳过它，按 BT.2408 的 203 显示）；Radiance 写在非标准的 `REFERENCE_NITS=` 头里。读一个**没有**这个答案的文件（别人写的 Radiance、别的程序产的 AVIF）时用 `--hdr-reference-white`（或 `cli.hdr-reference-white`），默认 203。自己截的图永远读那块输出自己报的参考白，不读这个设置——它只回答「文件没说」的情况。

颜色一律问显示器，不猜像素：10-bit 缓冲区在 HDR 输出上就是那块输出自己的像素，按它宣告的传递函数与参考白解码（`wp_color_manager_v1` 的输出描述，参考白即该输出的 SDR 白）。Hyprland 上这是 `misc:screencopy_hdr` 打开时**才**成立的约定——关掉时合成器只交 8-bit sRGB，此时不会写第二份。

上面那套映射的行为由 `--tone-map`（或配置文件里的 `cli.tone-map`）选择，三种：

- **`auto`（默认）**——白点由画面决定，也就是上一段描述的：没有超过 SDR 白的东西就按原样显示，有高光才把白下移。「有没有高光」问的就是上面那套面积判定，与决定要不要写第二份的是同一个答案，所以一张 SDR 窗口的截图是**精确**的。
- **`fixed`**——白点永远是 `--tone-map-white`（默认 0.8），不看画面。代价是 HDR 输出上**每一次** SDR 截图都会略暗（白落在 sRGB 231），换来的是**同一个像素的码值不随画面里还有什么而变**——pin 出来的一份因此和它截自的内容一致。
- **`normalize`**——按画面自己的峰值归一，即 SDR 白落在 `1/峰值`，最亮的那一点正好落在白上。高光之间**仍有先后，但没有间隔**：白以上的一切都被压进这个倒数腾出来的空间里，一条亮渐变会摊平。只在画面峰值确实是一个值得归一的高光时才合适。

`--tone-map-white` 是 SDR 白在输出范围里落点，0.5 到 0.95，超出这个范围会被**夹到边界**而不是报错（它是用户写下的数字，映射对它有一个明确的答案）。它被 `fixed`（永远用）和 `auto`（只在画面确实有高光时用）读取，`normalize` 自己算，忽略它。设置窗口里这一项是一个百分数输入框（默认 80 %），选中 `normalize` 时置灰。`--hdr-reference-white` 同理被夹到 1–1000 cd/m²。

冻结帧在交互界面上也按原样显示：VShot 在 overlay 下面另起一层 surface，挂的是**那块输出自己的 image description**（不是照着它造一个像的），所以合成器既不转换也不做色调映射，选中的区域就是屏幕上原本的光；overlay 自己只画遮罩（选区挖空）、标注与工具条。别的路线是造一份“像”的描述，那不够：compositor 会把它当成另一个空间，往面板自己的范围里做一次色调映射，整幅画面会一起变暗。

**pin 到屏幕上的 HDR 图也是 HDR 的**，走的是同一条道理：pin daemon 随自己启动一个小进程（`vshot --pin-hdr-server`），它在每块输出上铺一张 overlay 层的 surface，挂上和冻结帧一样的那块输出自己的 image description，把标注后重新按 PQ 编码的十位像素写进去，于是合成器不转换、不色调映射，贴上去的就是原来那束光。Qt 的 pin 浮层做不到这一点——它是 Qt 窗口，描述由 `QColorSpace` 造出，没有亮度信息，合成器会当成另一个空间压暗。所以**每一张贴图都由这个 helper 画**，不只是 HDR 的那些——原因不是 HDR 本身，而是堆叠顺序：同一层的 surface 按**映射顺序**堆叠，协议没有 restack 请求，而 helper 的那张必须在 daemon 的第一张 surface 之前映射（它先报过到），于是它永远在所有 Qt surface 之下，Qt 浮层上画的任何东西都会盖到 helper 画的每一张贴图上，包括本该盖住它的、排在它前面的贴图——一条边框也会压过挡在它前面的那张图。所以整摞图（图像、阴影、边框）都是 helper 的，一张 surface、一次 commit：HDR 贴图按自己的 PQ 直通，SDR 贴图由 daemon 写成 PNG 交给它、由它按该输出自己的参考白编码成 PQ（见下），Qt 浮层只留**角标、右键菜单、`HDR` 标签和输入遮罩**，并把图像那块挖空留给它。顺带修掉的是 SDR 贴图在 HDR 输出上被合成器色调映射压暗的问题：交给 helper 的是一束光，而不是一张等着被转换的 sRGB 图。helper 必须在 daemon 的第一张 surface 之前映射，所以它在 daemon 启动时就被拉起。截下来的内容没有超过 SDR 白（按上面的面积判定，不写第二份）时没有 HDR 那一份，pin 就是普通 SDR pin，走的就是上面那条 PNG 的路；合成器不提供 `wp_color_manager_v1` 时 helper 干脆不铺 surface，整摞退回 Qt 浮层自己画（图像、阴影、边框都在它这边）。每个图片文件只读一次（按路径缓存），拖动时 daemon 每批只发最后一条位置，所以快速拖动不会每个鼠标事件都重画一遍。

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
| **屏幕标注**（`annotate`） | ✅ wlr-layer-shell（实测） | ⚠️ 未测试 | ⚠️ 未测试 | ⚠️ 未测试 | ⚠️ 未测试 |

没有适配的地方会**自动降级**而不是报错：没有窗口列表就落到像素识别，问不到指针在哪块屏就落到 Qt 报的主屏。

### 验证到哪一步了
- **Hyprland**——本机会话就是 Hyprland，也是主要开发与验证环境：截图、选区标注、窗口、长截图、pin 与滚动注入都在这里跑过。**录屏编码后端**（本机 7900 XT）的 VAAPI 与 Vulkan 两条路都实测过（零拷贝 dma-buf、h264/hevc/av1、4K 跑满 60fps、窗口中途缩放、portal、回录）；NVENC 的**选路与失败路径**已验证（无 NVIDIA 的机器上干净失败并点名原因），但**真实的 NVENC 编码从未在 NVIDIA 硬件上跑过**。
- **逐应用音频 / 麦克风**——在 Hyprland 上实测过隔离（两个 mpv 分别播 880 Hz 与 220 Hz，`--app-audio` 只录到所录窗口的那一路）与混音（`--mic --app-audio` 出**单条** AAC 音轨，两路同时检出）；niri 与 KWin 的 pid 路径只有单元测试与无头实测，未在真实音频会话上验证隔离；Sway 与 labwc 没有窗口 pid 来源，会明确拒绝。**跟随焦点（`--follow`）** 在 Hyprland 上实测过（两个不同尺寸/音调的 mpv，切焦点得到一个连续文件）；**报不出焦点的合成器未实测**。
- **niri**——平铺与浮窗两条**截图**路径已实机验证（`--no-blend` 可绕开浮窗定位）；**窗口录制/回录走合成器自己的 ScreenCast 服务**（`org.gnome.Mutter.ScreenCast`，不需要 portal、没有 picker、没有授权弹窗），4K 窗口录制、中途改尺寸、窗口关闭收尾、回录的 `status`/`save`/`stop` 都实测过；`--follow` 在这条路上不可用（服务 cast 的是启动时那一扇窗）。**注意一个 niri 侧的限流**：它的 cast 是在**输出渲染循环**里画的，所以会话不活动时渲染基本停摆（实测掉到约 1.3fps）；要让 niri 以正常帧率录制，它的 VT 必须是当前活动 VT。**KWin/Plasma** 的 D-Bus 采集、窗口列表、长截图、窗口录制与回录都已在**无头 `--virtual` KWin** 上实测（含授权被 ksycoca 重写临时拒绝时的重试规则）；**仍未验证**：`--cursor`、窗口点选时的 `--pick` 整段交互、以及滚动注入（需要 KDE 上验收）。
- **Sway / labwc / GNOME**——Sway 只有探针实现与单元测试，**没有现场验证**；**labwc 等其它提供 wlr-screencopy 的合成器**理论上基础截屏可用，但没有窗口列表与 pin 落点探针，且**没有现场验证**；**GNOME** 不支持，没有实现计划（Mutter 不提供 wlr-screencopy、KWin 那套 D-Bus 服务，也没有 layer-shell）。
- **屏幕标注（`annotate`）**——只在 **Hyprland** 上实测（画笔、擦除、文字、撤销/重做、每块输出各自的画布与工具栏、截图/录屏时自动藏工具栏）；niri、KWin/Plasma、Sway、labwc 均**未经测试**。

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

窗口只是普通窗口，不截图、不需要任何合成器协议，所以在一个 vshot 本来截不了图的合成器上也能用；装包后也可以直接从**应用菜单**里的「VShot Settings」打开（见[「应用菜单入口」](#应用菜单入口)）。`cli` 段的数值留空/留 0 表示「不设，用内置默认」，而不是把 0 存进去，**设置窗口里这些框直接显示内置默认值**（默认值留在原处不动、保存时照样不写进文件，所以哪天内置默认改了，没动过它的人会自动跟上），`editor` 段则总是整段写出；保存是**合并写入**，本版不认识的键原样保留，不会因为存一次就被抹掉。窗口分十页，左边栏切换，**一页一个功能**：**标注编辑器**、**输出**、**格式设置**、**HDR**、**滚动截图**、**文本识别**、**录制**（`record` 与 `replay` 的全部默认值）、**文件对话框**、**Pin 浮层**与 **键盘**；除**录制**页外，各页在默认窗口尺寸下都**不需要滚动**。一页里的设置按**主题**再分小组，卡片内用一条细分隔线加一行小标题隔开（比如「HDR」页分成「第二份文件」、「SDR 那一份」与「什么算 HDR」），顺序按**用户碰到它们的频率**排：常改的在前，设一次就不动的在后。**「输出」页放两个格式选择器**（SDR 格式与 HDR 格式），**「格式设置」页的卡片是按编译进来的格式生成的**：本构建有几种格式就有几张卡，每张卡里的行来自该格式自己声明的参数——在 Rust 侧给一个编码器加一个参数，这一页自动多一行，设置窗口不用改；反过来，`--no-default-features` 去掉某个格式，这一页也就不会出现它。它和麦克风那一行一样**不阻塞窗口打开**：格式清单是起一个 `vshot formats --json` 子进程问出来的，窗口先开出来，答案到了再填。HDR 那五项原来挤在「输出」页的一张卡里，单独成页是因为它们本来就是同一个主题，而不截 HDR 的人不该为了改输出设置滚过它们。麦克风那一行**不阻塞窗口打开**——它要起一个 `vshot record mics` 子进程去问 PipeWire（约一秒），所以窗口先带着「不录音 / 会话的默认输入设备」两个答案开出来，设备列表拿到了再填进去；「检测」按钮重新问一次。保存之后窗口不关，左下角显示「已保存。」；关窗口按「取消」——那时它的意思就只剩「关闭」。

```json
{
  "editor": {
    "tool": "arrow", "color": "#ff8800ff", "width": 4, "textPixels": 28,
    "dash": "dotted", "arrowSize": 3, "arrowStyle": "filled",
    "mosaicShape": "brush", "mosaicStrength": 3, "font": "Noto Sans",
    "selectMode": "loose"
  },
  "cli": {
    "sdr-format": "png",
    "hdr-format": "hdr",
    "format": {
      "png": { "compression": "high" },
      "avif": { "quality": 40, "speed": 4 }
    },
    "tone-map": "fixed",
    "tone-map-white": 0.75,
    "hdr-area-test": false,
    "hdr-reference-white": 250,
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
| `selectMode` | `precise` / `loose` | `precise` |
| `color` | `#rrggbb` 或 `#rrggbbaa` | `#ff4040ff` |
| `width` | 1–64 | `2` |
| `textPixels` | 7–448（像素高） | `14` |
| `dash` | `solid` / `dashed` / `dotted` | `solid` |
| `arrowSize` | 1–8 | `1` |
| `arrowStyle` | `open` / `filled` | `open` |
| `mosaicShape` | `rect` / `ellipse` / `brush` | `rect` |
| `mosaicStrength` | 1–3 | `2` |
| `font` | 字体族名；空串用系统默认 | `""` |

`tool` 是**编辑状态**的起始工具，和其他样式一样**只在配置里**。区域截图**不**采用它——它的第一步是拖出选区，进去就带着工具（比如 Text）会让第一次点击变成放置文本；框好之后也仍然不采用，所以刚框完想改一下选区，直接拖就是重新框，不用先去工具栏放下工具。真正采用它的，是**打开时就已有选区**的会话：`window pick` 的后续编辑和贴图重进编辑（整个画面预先框好），它们没有框可拖，就用配置里记着的工具开局。滚动截图同理永远不带工具，它没有标注这一步。取值超出范围的整数会被夹到范围内，不认识的名字按默认值处理，手改文件写错了不会报错，只是那一项不生效。**`textPixels` 的单位是像素**：你填 14，字就是 14 像素高，跟编辑器里那个数字框完全一致；旧协议里那个「字符格整数倍」的刻度（1–64，一格 7 像素）只在把结果写出去时换算一次。早期版本这里叫 `textSize`，存的是那个刻度，**旧文件会被自动迁移**（`2` → 14 px、`3` → 21 px），下次保存时写成 `textPixels`、旧键删掉；两个键同时存在时以 `textPixels` 为准。

`selectMode` 决定**已经选中一个标注之后，再按下去会拿起什么**。`precise`（默认）要求指针落在标注身上；`loose` 是「先选中、再随便拖」——标注一旦选中，屏幕上**任何位置**按下拖动都在移动它，一像素宽的笔迹也不用再瞄准。代价是这期间**选区让位给标注**（选区手柄仍然优先，它是个明确的小目标）：想重新拖选区，先在空处**单击一下**把标注放开——单击只取消选中，不动选区，之后选区照旧可用。

### `cli`——命令行默认值
给**没有在命令行上给出**的参数提供默认值。优先级是：

```
命令行参数 > 环境变量 > 配置文件 > 内置默认
```

| 键 | 对应参数 | 内置默认 |
| --- | --- | --- |
| `sdr-format` | `--sdr-format` | `png` |
| `hdr-format` | `--hdr-format` | `avif` |
| `format.<格式>.<参数>` | `--format-param <格式>.<参数>=<值>` | 各格式自己声明，见 `vshot formats` |
| `tone-map` | `--tone-map` | `auto` |
| `tone-map-white` | `--tone-map-white` | `0.8` |
| `hdr-area-test` | `--hdr-area-test` | `true` |
| `hdr-area-ratio` | `--hdr-area-ratio` | `0.0005` |
| `hdr-reference-white` | `--hdr-reference-white` | `203` |
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
| `translate.provider` | `translate --provider` | `google` |
| `translate.from` | `translate --from` | `auto` |
| `translate.to` | `translate --to` | `zh-Hans` |
| `translate.timeout` | 翻译 HTTP 请求的超时（秒） | `20` |
| `translate.notify` | 翻译结束时弹桌面通知 | `false` |
| `translate.fallback` | 主 provider 没给出结果后按顺序接着试的 provider（数组） | 空 |
| `translate.bing.endpoint` / `api-key` / `region` | Bing（Azure Translator）的地址与凭据 | 全球端点 / 无 / 无 |
| `translate.baidu.app-id` / `secret-key` | 百度翻译的凭据 | 无 |
| `translate.ai.endpoint` / `api-key` / `model` / `prompt` | OpenAI 兼容接口的地址、凭据、模型与系统提示词（空则用内置） | 无 |
| `translate.external.command` / `timeout` | 外部翻译程序（数组）与超时（秒） | 无 / `30` |
| `translate.lingocloud.token` | 彩云（lingocloud）的 token；不填则用内置借来的那个 | 内置借用值 |

`format` 那一段是**每种格式自己的编码参数**，一层格式名、一层参数名：`format.png.compression`、`format.avif.quality`、`format.avif.speed`。哪些格式、每个格式有哪些参数，都由**编解码层自己声明**——加一个参数不用改设置窗口，加一种格式只要在 `Cargo.toml` 里打开对应的 feature（`avif` / `radiance` / `png`，默认三个都开），`vshot formats` 与设置窗口的「格式设置」页就跟着变。**`png-compression` 这个旧键已废弃**，不再被读取，改用 `format.png.compression`（取值不变）。`sdr-format` 与 `hdr-format` 仍然留在 `cli` 顶层：它们选的是**哪个格式**，不是格式的参数。

`google`、`microsoft`、`volcengine`、`transmart` 四个免密钥 provider 没有任何配置键，把名字写进 `translate.provider` 即可使用；`lingocloud` 同样免配，唯一可选的键是 `translate.lingocloud.token`（不填就用内置借来的 token）。

`pin.density` 的优先级同样是 `--density` > `VSHOT_PIN_DENSITY` > 配置文件；`cli` 段里不认识的键会被忽略，不会让整个文件失效。`ocr.engine` 只认 `builtin` 与 `external` 两个值，写了别的名字会**报错**而不是当默认值处理，因为把 `external` 拼错会让人以为自己配的 GPU 引擎生效了（`engine: "external"` 而没有 `command`、或命令跑不起来，同样明确报错，详见[「用 GPU：外接引擎」](#用-gpu外接引擎)）。设置窗口覆盖 `editor`、常用的 `cli` 项（含 `format` 段）、`ocr.notify` 这个开关，以及 `record` 与 `replay` 两段；`ocr.engine`、`ocr.external` 与整个 `cli.translate` 段要手改文件（`translate.provider` 认那九个名字再加 `auto`，写错会报错）。**两个 `notify` 开关只写「关」**，因为键不存在就是「开」；`translate.notify` 相反——它默认就是「关」，所以手改文件时才需要写出来。麦克风那两项是**从当前会话检测出来的**；`follow` 那两项在窗口里是**一行逗号分隔的窗口名**，在文件里是一个数组——手改时写成 `["game", "chat"]`。

`record.follow` / `replay.follow` 只在**不带窗口名、也不给 `--pick`** 的 `record window` / `replay start window` 上生效；其它目标（`monitor`、`all`、`region`，或命令行上点了名的窗口）会**忽略**记着的跟随列表，而不是因为它在而报错。`--no-follow` 是对那一次录制/回录把记着的列表关掉，正如 `--no-mic` 对记着的麦克风那样。`color` 用的是 CSS 那套写法：`#rrggbb`，带透明度时写 `#rrggbbaa`（alpha 在**最后**）——注意这跟 Qt 自己的八位写法 `#aarrggbb` 不同，`vshot settings` 与配置文件都按 CSS 那套来。

### `dialog`——文件对话框外观
保存与打开窗口（pin 的「另存为…」、编辑器里贴本地图片）是 **layer surface**，合成器不给它们画任何装饰，所以外框和阴影是它们与背后内容之间唯一的东西。

| 键 | 取值 | 默认 |
| --- | --- | --- |
| `radius` | 0–1000000，逻辑像素 | `12` |
| `borderWidth` | 0–1000000，逻辑像素；0 完全不画 | `1` |
| `borderColor` | `#rrggbb`；**不写**表示按配色方案推导 | 无 |
| `shadow` | `true` / `false` | `true` |
| `shadowSize` | 0–512，逻辑像素 | `14` |
| `shadowOffset` | -512–512，逻辑像素 | `3` |
| `shadowOpacity` | 0–255 | `120` |

`borderColor` 不写（或删掉）时会按对话框自己的配色推导出一道比底色略深的线，所以在亮/暗主题下都读得出是条边，写了就用你写的。阴影的四个键与 `pin` 段**是同一套**（含义、范围和默认值都相同，见下一节）。对话框是 layer surface，只能有它自己申请的那个大小，所以阴影画在窗口内部留出的一圈里，`shadowSize` 因此也决定了对话框本体比窗口小多少——它是「一圈」的宽度，不是叠加在外面的。

### `pin`——pin 浮层外观
pin 同样是 layer surface，里面只有图片，所以圆角、身下的阴影、外面那道线都得 vshot 自己画。这一段和 `cli.pin` 是**两回事**：那个是 pin 的**尺寸**（来源密度，属于命令行默认值），这个是 pin 的**样子**。

| 键 | 取值 | 默认 |
| --- | --- | --- |
| `radius` | 0–1000000，逻辑像素；0 是直角 | `0` |
| `shadow` | `true` / `false` | `true` |
| `shadowSize` | 0–512，逻辑像素；0 等于不模糊 | `14` |
| `shadowOffset` | -512–512，逻辑像素；负值把阴影抬到上方 | `3` |
| `shadowOpacity` | 0–255 | `120` |
| `borderWidth` | 0–1000000，逻辑像素；0 完全不画 | `2` |
| `borderColor` | 未激活时的边框色；不写用内置浅灰 `#c0c0c0` | 无 |
| `activeBorderColor` | 激活时（鼠标停在那张 pin 上、且该屏持有键盘）的边框色；不写用内置黑色 | 无 |

默认**不圆角但有阴影**：截图是一张窗口的像，圆角会切掉它自己的内容；而一张截图钉在一模一样颜色的窗口上时，没有阴影就完全看不出边界在哪。阴影的三个数值键：`shadowSize` 是模糊向外扩多远（也就是阴影视觉上的「软硬」），`shadowOffset` 是把整个阴影往下压多少——光从上面来，所以默认 3，填负数它就跑到 pin 上方去；`shadowOpacity` 是阴影的浓度，模糊只是把它摊开、不会加深；`shadow` 是总开关，**关掉不会丢失数值**，所以可以临时关一下再打开，尺寸还在。`radius` 是**上限而不是承诺**：真正画的时候会夹到图片短边的一半——再大就不是圆角而是胶囊了，而 pin 的尺寸随滚轮每滚一格都在变，所以这件事只能画的时候算。边框**居中在图像边缘**上，一半在外一半在内，宽度改变时圆角 pin 和直角 pin 的外沿位置一致。圆角和边框宽度**没有实际上限**（表里的一百万逻辑像素就是 `int` 能给的最大值再收一点，一千块屏，谁也用不到）：它们不影响任何分配，圆角画的时候会被夹到短边的一半。**只有阴影的两个数是真有上限的**（512），因为它们是这里唯一「越大越贵」的东西：模糊要按形状向外扩出的范围做一张图，文件对话框的窗口本体也要按这个范围撑大，再大就是花掉几百 MB 换一圈看不到的雾。阴影是一次性算好缓存的，拖动时不会重算，缩放或改配置会；**改完配置要生效，下次 `vshot pin` 会重新读**——daemon 在还有 pin 的时候不会退出，但每加一张 pin 都会重读一次文件，所以不用手动重启 daemon。

### `shortcuts`——键盘绑定

截图在屏幕上时，哪个键做什么。**每个动作在工具栏上也有按钮**，所以一个碍事的键可以直接清掉（写成空串），而不是只能挪到别处。改它推荐用 **`vshot settings` 的「键盘」页**：点一下那一行就弹出这个动作的按键编辑器——最上面是录制按钮，点一下再按你要的键；下面每个已设的键各占一行，右边有移除按钮。手写字符串得猜 `QKeySequence` 的拼法，猜错了会被当成「这个动作没键」，而不是报错。

录到一个**别的动作已经占着**的键时会先问一句要不要把它挪过来（默认「否」，所以顺手一个回车不会把别人的键拿走）；答「是」就把那个键从原动作上摘掉再挂到当前动作。录到一个**本动作已经有**的键则直接忽略——那是同一件事说了两遍，不是冲突，多出一行只会多出一个移除按钮。编辑器的「取消」把整份绑定退回打开时的样子。

**取色相关的两个键只在「取色器」可见时生效**，也就是**右键按住**带出来的那个放大镜。`copy-color`（`C`）与 `adopt-color`（`V`）是「对着某个像素」的动作，而拖拽时的放大镜只是个坐标读数——它存在的意义是告诉你光标在哪，不是那个像素是什么颜色。这不只是语义问题：取色器可见时右键正按着，指针仍是当下正在移动的东西，光标键恰恰是这时最需要的——而光标行走也占用的字母（`A` 就是 `cursor-left`）会在取色器亮着的那段时间被它吃掉，偏偏那正是最不能吃掉的时刻。`V` 不是光标键，两边都按得出来。放大镜本身照旧**任何拖拽都会出现**（画线那种落墨的除外），只是不带取色器。

**右键按住时的取色器跟着光标走**：它读的是光标*当下*压着的像素，不是按下去那一刻的，所以按住右键划过去，取色器的读数一路跟着变。**右键是两种手势，靠按住的时间分开**：点一下（不到 300 ms）就是取消整次截图——这个键本来就是会话自己的键；按住不放才是取色器，松手时什么都不发生，画笔和标注都停在原处。放大镜在按下的一瞬间就亮，不等判决：取色器要等按住一会儿才出现的话，等它出来用户已经瞄准完了。

**取色器的读数分两行**：上面一行是颜色代码，底色**就是那个颜色本身**，字色按 Rec. 601 亮度在纯黑和纯白里挑一个，保证任何底色上都读得清（固定白色在白像素上就没了，而白像素恰恰是取色器最常被指到的东西）；下面一行是 `C`/`V` 两个键的提示，用固定的深色底——它是说明文字，跟着取到的颜色变底色只会每次移动都闪一下。

| 键（动作 id） | 默认 | 说明 |
| --- | --- | --- |
| `confirm` | `Return, Enter` | 接受这次截图并写出来 |
| `cancel` | `Esc` | 丢掉这次截图 |
| `undo` | `Ctrl+Z` | 撤销 |
| `redo` | `Ctrl+Y, Ctrl+Shift+Z` | 重做 |
| `copy` | `Ctrl+S` | 把截图和标注合成后放进剪贴板 |
| `copy-text` | `Ctrl+C` | 复制取字模式或翻译选中的文字 |
| `paste` | `Ctrl+V` | 把剪贴板里的图片贴进选区 |
| `select-all` | `Ctrl+A` | 选中所有标注 |
| `select-none` | `Ctrl+D` | 放开所有标注 |
| `next-mark` | `Tab, Ctrl+Tab` | 焦点移到下一个标注 |
| `previous-mark` | `Shift+Backtab, Ctrl+Shift+Backtab` | 焦点移到上一个标注 |
| `delete` | `Del, Backspace` | 删掉焦点所在的标注 |
| `copy-color` | `C` | 放大镜显示时，复制光标下那个像素的颜色代码 |
| `adopt-color` | `V` | 放大镜显示时，把那个颜色设为当前工具的 |
| `magnifier` | `M` | 不拖拽也显示两秒放大镜 |
| `cursor-left` / `cursor-right` / `cursor-up` / `cursor-down` | `Left, A` / `Right, D` / `Up, W` / `Down, S` | 光标走一个像素 |

三个**只读**的绑定，它们是在别的操作已经进行时读键盘状态，而不是自己收一次按键——`QKeySequence` 没有「单独按 Shift」这种写法，能写出来的「Shift」也会在 Shift+F4 上一起触发，所以只列出来给用户看，不给改：

| 键 | 默认 | 说明 |
| --- | --- | --- |
| `preserve-aspect` | `Shift` | 缩放或画矩形/椭圆时按住：缩放保持比例，矩形画成正方形、椭圆画成正圆 |
| `coarse-step` | `Ctrl` | 移动光标时按住，一次走十像素 |
| `select-mark` | `Ctrl` | 按住以「拿起」光标下的标注或整块选区：移动它，而不是用当前工具画 |

表里有两个动作共用 `Ctrl`：`coarse-step` 读的是**方向键**按住时键盘的状态，`select-mark` 读的是**鼠标按下**那一刻的状态，没有哪个手势同时是这两件事，所以两个读者不会撞上。真要说交集也是顺的——按住 `Ctrl` 拖一张标注时，方向键走的仍然是十像素：`Ctrl` 在这两处的意思都是「调得粗一点」。（`Ctrl+A`、`Ctrl+Z` 那些是**组合键**，问的是「按了哪个键」，和这两个问「按住什么」的不是一个问题。）

**移动选区或标注窗口**用的是**鼠标中键**，不是键盘上的修饰键：拖动本身就是一个「按住不放、一直移动」的手势，用修饰键意味着整个移动过程都得按住一个键，而真正干活的却是左键。中键按住时拖动选区主体即移动选区，在贴图编辑里则拖动画面把底图挪到标注下面；松手即结束。它不在上面的表里，因为鼠标按键根本读不了键盘状态。

「选择」工具删掉之后，**拿起一个已有标注**这件事由按住 `Ctrl` 来做：按住时按在标注上拖就是移动它，**不会切换当前标注工具**——松手就回到原样，所以画到一半顺手挪一下刚才那笔不需要重新选工具。按住 `Ctrl` 时指针下的标注会**套一个虚线框**，和选中标注那个框同一种画法：光标形状只说「这里能拖」，说不出**哪一个**标注会被拖走，而标注是一块面积不是一个点，所以还得有个框把它指出来。不按住时按在标注*身上*仍然是当前工具落墨（画笔得能在已有标注上起笔，否则画面越满笔越难用），但**按在标注边框上永远可以拖**，**拉伸则始终走边框上那 8 个操作柄**：它们是又小又明确的靶子，只能有一个意思，所以不挑按住什么、也不挑当前有没有工具。`Ctrl` 同样管**选区**：按住它按在选区身上拖，整块选区就跟着走——这是绘图工具没法承担、平时只有中键能干的事。这样「临时接管」的拖动进行期间，工具栏上的 **Select 按钮会亮起来**（那才是编辑器此刻真正的状态，手上的绘图工具不是），松手就回到原来那个工具，不需要再选一次。

**举起手来是按「框」判定的，不是按墨迹**：`Ctrl` 和中键问的是「您指的是哪个标注」，答案是标注的**外框**（就是选中框和 8 个操作柄围出来的那个盒子）——斜着的一笔是一条线，正圆只有一圈边，谁也没法把指针稳在一条两像素宽的线上，而用户瞄准一个标注时看的就是那个框。所以按住 `Ctrl` 或按中键，在标注框里任何位置按下都能把它拿走；**不按住时**则是画笔说了算：按在已有标注的框里但没压到墨上，照旧落墨（画面越满，画笔越得能在标注上起笔）。马赛克、贴图、文字这些「本来就是一块」的标注不受影响，它们的墨迹判定本来就是整个框。

`Shift` 还有两个「优先级」上的例外，都是用户报过的问题：

1. **画正方形/正圆优先于拿起标注**。手上是矩形或椭圆工具、又按住 `Shift` 时，这一下就是**在画一个正方形/正圆**，压在下面的任何标注都不参与（`Shift` 和 `Ctrl` 同时按着也一样：方就是方）——一张图上有几个标注之后，几乎每个点都落在某个标注框里，让标注先应答等于「只要这儿画过东西，正方形就画不出来」。**选区自己的手柄不受影响**：那是会话自己的边框，不是躺在路上的标注，`Shift` 在这些手柄上仍然是「保持比例缩放」。
2. **选中标注不该挡住选区**。上面这条只约束标注，选区永远排在前头：中键、或 `Ctrl`+按，只要落点在选区身上（而不是在那张已经被拿起、正拿在手里的标注框里），走的就是**整块选区**。之前中键会被「松散模式」的标注拖拽抢走，结果是「选过一次标注就再也够不着选区了」——那是 bug，不是设计。

**Ctrl+A 拿起来的是整套标注**：一次全选之后，移动整套要一个**明确的「调整」手势**——**中键**、**Select 工具**在手、或按住 `Ctrl`——从**任何一个**标注上按下拖动，移动的就是**全部**标注，位移完全相同（所以它们之间的相对位置不变）；方向键同样一次挪动整套（全选之后按方向键就是挪整套）。整套一起撞到画面边缘时会**一起停下**——位移按「每个标注各自还能走多远」的交集夹取，否则最前面那个顶住边缘、后面的还在滑，这一组就散开了。**全选不是一个模式**：手上还握着画笔/矩形时，按在标注上照旧落墨，按在空处照旧重新框选区，点一下空处或选中别的就把这套放下；而**换工具（换成 Select 以外的）也会把整套放下**，和放下单个标注一样。

用方向键走光标时，**屏幕上那个指针会真的跟着走**。光标是编辑器自己走的一格，放大镜和下一次落笔读的都是它，但合成器画出来的那个箭头是另一回事——只有 CLI 能动它（注入后端在 CLI 手里：合成器的虚拟指针协议、portal、`/dev/uinput`），所以编辑器把「走到哪了」写成一行请求，从会话进来时那根管道发回去，由 CLI 转成一次指针移动。请求**节流但会合并**：按住方向键的重复速率远高于一次跨进程加合成器的往返，所以间隔内的步子会攒下来、到期只发最新的一次（位置是绝对的，旧的没有意义），而不是丢掉——丢掉的话快速点五下右键光标只走一格。CLI 这边**只在合成器提供虚拟指针协议时**才动指针，不回落 portal 和 `/dev/uinput`：portal 会弹授权对话框，而敲一下方向键不该弹出任何东西；这种情况光标照走、放大镜照指，只是屏幕上的箭头不动。区域编辑器和 **pin 编辑器**都走这条路，位置一律是**全局逻辑像素**；pin 编辑器的会话本身只描述 pin 那一块屏，所以 daemon 额外写了整个桌面布局的包围盒（`desktop`），CLI 按它换算——没有这个字段就干脆不动指针，因为按 pin 的矩形换算出来的位置是错的。

**正在画的那一笔也吃这套键。** 那些「按下定起点、松开定终点」的工具——矩形、椭圆、区域马赛克、箭头、直线、波浪、自由画笔和笔刷——整个拖拽过程左键都按着，鼠标恰恰是最没法把终点放准的东西。这时按键动的是**进行中的手势**而不是已提交的标注：按下时定的锚点原地不动，远端跟着光标走，一格一格（按住步长修饰键则十格十格）。钢笔路径是同一件事的另一层意思——它最后一个锚点已经落下了，所以按键拉的是那个锚点伸出去的控制柄。

**自己走出来的回声不会把放大镜关掉。** 上面的指针回写会让合成器报一次指针移动，而这次上报是在请求发出**之后**才到的；把它当成「用户动了鼠标」，就会掐掉这一步刚刚亮起来的那个两秒放大镜——于是每一步都闪一下，最后一下是留下还是消失全看竞态。回声是按落点认的：回写把指针恰好放到光标走到的那个像素上，所以报回来的移动就落在那一个像素上，而手真的动鼠标至少要走一格。请求发出后的一小段窗口内，落点正好等于回写目标的移动就按回声处理——不把光标交还给鼠标，也不结束放大镜。其余任何移动照旧两件事都做，所以放大镜不会跟着指针一路亮着不走。

一个键可以写**多个拼法**，用逗号隔开（`"Del, Backspace"`），任何一个都触发。**清空**写成 `""`（或者干脆不写这个键，那就是用内置默认）——两者不一样：`""` 是「这个动作不要键」，不写是「跟随内置默认」，所以哪天内置默认改了，没动过它的人会自动跟上。**认不出来的拼法回落到内置默认**，不会让动作变得按不出来。存的时候只有**与内置默认不同**的才写进文件，其余省掉。

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
| `VSHOT_HDR_HELPER` | 指定画 pin 图像栈的 `vshot --pin-hdr-server` helper 进程的 `vshot` 路径（默认按 `vshot-qt-ui` 所在位置推断） |
| `VSHOT_PIN_DENSITY=N` | 每张 pin 图的来源密度，等同 `--density` |
| `VSHOT_PIN_DEBUG=1` | daemon 打印每张 pin 的密度判定 |
| `VSHOT_PIN_FOCUS_DEBUG=1` | daemon 打印 pin 渲染面每一次焦点变化 |
| `VSHOT_PIN_SOURCE_FILE` | 覆盖截图工具记录的路径（默认 `/tmp/screenshot-path`） |
| `VSHOT_ANNOTATE_SOCKET` | 标注 daemon 监听的 socket（默认 `$XDG_RUNTIME_DIR/vshot-annotate-<uid>.sock`） |
| `VSHOT_ANNOTATE_DEBUG=1` | 让标注 daemon 把自己的诊断留在 stderr |
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
- **屏幕标注**：`vshot replay start` 期间**不会**自动隐藏工具栏（回录一开就持续抓帧，整段会话都藏会让浮层没法用），所以标注与回录不打算一起跑；和 pin 浮层一样需要 `wlr-layer-shell`，没有 layer shell 的合成器画不出来；只在 **Hyprland** 上实测过，其它桌面未经测试。
- 原生 screencopy 等待合成器返回帧最多 10 秒，超时返回错误而不是永久阻塞。

## 验证
```sh
cargo fmt --check
cargo test --locked
cargo clippy --locked --all-targets --all-features -- -D warnings
cargo build --release --locked
```

Qt helper 侧没有测试框架，只有**不需要合成器的离屏检查**（默认不构建，加 `-DVSHOT_BUILD_CHECKS=ON`），覆盖配置读写与设置窗口、字号换算、剪贴板颜色解析与色卡渲染、文件对话框的样式表与缩略图、pin 的图片自述密度、描边与 HDR 标记、文字卡片留白、色卡右键菜单、贴图的导出格式、标注浮层的五个工具与撤销/清除/工具栏位置、overlay 是否把冻结帧留给 backdrop、工具栏的落位、标注渲染缓存的命中、高 DPI 屏上的缓存分辨率，以及文字层——它解析 `vshot ocr --json` 打印的 JSON（这套线格式一头在 `src/ocr.rs`、一头在 Qt 侧），并且不建控件，因此不需要 `QT_QPA_PLATFORM`：

```sh
cmake -S . -B build-qt -DVSHOT_BUILD_CHECKS=ON && cmake --build build-qt
build-qt/vshot-config-check
QT_QPA_PLATFORM=offscreen build-qt/vshot-settings-check
build-qt/vshot-text-size-check
QT_QPA_PLATFORM=offscreen build-qt/vshot-color-check
build-qt/vshot-pin-density-check
QT_QPA_PLATFORM=offscreen build-qt/vshot-pin-outline-check
QT_QPA_PLATFORM=offscreen build-qt/vshot-pin-hdr-check
QT_QPA_PLATFORM=offscreen build-qt/vshot-text-card-check
QT_QPA_PLATFORM=offscreen build-qt/vshot-pin-menu-check
QT_QPA_PLATFORM=offscreen build-qt/vshot-paste-check
QT_QPA_PLATFORM=offscreen build-qt/vshot-file-dialog-check
QT_QPA_PLATFORM=offscreen build-qt/vshot-annotate-check
QT_QPA_PLATFORM=offscreen build-qt/vshot-backdrop-check
QT_QPA_PLATFORM=offscreen build-qt/vshot-toolbar-check
QT_QPA_PLATFORM=offscreen build-qt/vshot-annotation-check
QT_QPA_PLATFORM=offscreen build-qt/vshot-dpr-check
build-qt/vshot-text-layer-check
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
