# vshot

[中文](README.md) | **English**

> **This project is pure vibe coding**: requirements come from a human, code and docs are written by AI.

A Wayland screenshot tool written in Rust, with a Qt interactive UI and a resident pin overlay. Captures are **strictly frozen**: the desktop is photographed into a still frame first, and every selection and annotation happens on that frame, so nothing on screen moves while you are choosing.

Works with Hyprland, niri, KWin/Plasma, Sway, and basic capture on any compositor providing `wlr-screencopy` (such as labwc).

> **Verification varies**: every Hyprland feature was tested live; niri's tiled and floating window capture paths were both verified on a real session; KWin/Plasma's D-Bus capture, window list, and long screenshots were tested, but `--cursor` and scroll injection were not; Sway and labwc are **untested on a live session**. See ["Compositor support and verification status"](#compositor-support-and-verification-status).

## Features

- **Region capture** — drag on the frozen desktop, or give a fixed geometry; eight resize handles, with a magnifier and size readout following the pointer
- **Annotation editing** — rectangles, ellipses, arrows, freehand, text, mosaic; undo/redo, move and resize a selection, colors and line styles, arrow head styles, font sizes, system font picker
- **monitor / all** — capture one output by name or by pointer position, or compose the whole desktop by logical position
- **window active / pick** — the focused window, or click one on the live desktop; KWin and niri hand over the window's own pixels
- **Long screenshot** — frame a scrolling region, and vshot sends the wheel, grabs frames, aligns them by content, and stitches one long image
- **Pin overlay** — pin images or clipboard content to the screen: drag, wheel to zoom, double-click to close, one-key show/hide, Space to annotate
- **Clipboard pinning** — colors, images, copied image files, plain text (rendered as a card as HTML / markdown / code / plain text)
- **OCR** — frame a region and get its text back (Chinese, English and Japanese), through `vshot ocr` or the editor toolbar's *Text+* button, with a desktop notification when it finishes
- **Output targets** — file (with strftime paths), stdout, clipboard, or an on-screen pin; exactly one
- **Bilingual UI** — the interface and `--help` follow the system language

## Install (Arch Linux)

`PKGBUILD` builds the Rust CLI and the Qt helper into one package, so a single install provides `/usr/bin/vshot`, `/usr/bin/vshot-qt-ui`, the `/usr/share/applications/vshot.desktop` that authorizes KWin, and an icon-bearing launcher entry `/usr/share/applications/vshot-settings.desktop` (see ["Application menu entry"](#application-menu-entry)):

```sh
./scripts/build-arch-package.sh
sudo pacman -U dist/vshot-0.1.2-1-x86_64.pkg.tar.zst
```

The script snapshots the current working tree (uncommitted changes included) into a temporary directory and runs `makepkg`, writing the result to `dist/`; `makepkg -si` works directly too. Runtime dependencies are `glibc`, `wayland` (uses `libwayland-client` through dlopen), `qt6-base`, `layer-shell-qt`, and **`onnxruntime`** (the OCR inference engine); file output, `--clipboard`, and `vshot pin --clipboard` need the optional `wl-clipboard` (writes via `wl-copy`, reads via `wl-paste`). The package also installs about 30 MB of OCR models under `/usr/share/vshot/models/`, which `makepkg` fetches and SHA-256-verifies. For other distributions, build from source as below.

Those 30 MB are downloaded on the **first** build only. The models are cached in `$XDG_CACHE_HOME/vshot/makepkg-sources` (usually `~/.cache/vshot/makepkg-sources`) and every later build takes them from there, with or without a network. A fixed cache directory is needed because `makepkg`'s own source cache lives inside the directory it builds in, and this script builds in a fresh `mktemp -d` each time -- so the default cache disappears along with the previous run's temporary tree and the download happens again. Set `SRCDEST` and that is used instead. To force a re-download, delete the directory; the next build fetches the files again. If `models/` already holds the three files, copying them in skips even the first download, since the checksums match:

```sh
mkdir -p ~/.cache/vshot/makepkg-sources && cp models/* ~/.cache/vshot/makepkg-sources/
```

`onnxruntime` is a **virtual package** on Arch: all six variants (`onnxruntime-cpu`, `onnxruntime-cuda`, `onnxruntime-rocm`, …) declare `Provides: onnxruntime` and conflict with each other, so a system has exactly one. Depending on the virtual name rather than on `onnxruntime-cpu` means **someone who already has a GPU variant installed does not have to tear it out** for vshot — removing it would take `rccl`, `migraphx` and `rocm-hip-sdk` with it. vshot uses only the shared library and the `.pc` file, which every variant ships, and nothing here selects a GPU provider, so a GPU variant runs the OCR on the CPU exactly like the CPU one. A fresh install lets pacman pick; `onnxruntime-cpu` is the recommended choice (about 46 MB with cpuinfo and protobuf, against well over a gigabyte for the GPU builds).

## Application menu entry

The package installs a `vshot-settings.desktop` that shows up in the application menu as **VShot Settings** and opens the graphical settings window, `vshot settings`. Its icon is `icons/vshot.svg`, installed under `hicolor/scalable/apps/` — one scalable SVG rather than a size ladder, because the shells that read it (KDE, GNOME, wlroots launchers) render SVG themselves and ask for the size they need.

Wayland has **no per-window icon**: a compositor takes the application id the client declares, finds `<id>.desktop`, and draws whatever `Icon=` names. So the name declared in `ui/main.cpp` has to match the installed desktop file's name; change one and you must change the other, or the window silently falls back to a generic placeholder.

That id is not only an icon lookup, though: **Qt also registers it with the host portal**, which resolves it against the same directories and prints `Failed to register with host portal ... App info not found for 'vshot-settings'` on stderr when no such file is installed. A copy run straight from the source tree or `build-qt/` has no desktop file installed, so it prints that on every start — which is exactly what it should say about a copy with no desktop entry. The id is therefore **declared only when the file is actually installed** (`QStandardPaths::locate` on `ApplicationsLocation`); without it the window falls back to the compositor's placeholder and the portal stays quiet.

`vshot-settings-check` has a section that watches this chain: whether the app id matches the file name, whether the file `Icon=` points at is in the source tree, whether the PKGBUILD installs all of it, and **whether the declaration still has its "only if installed" guard** — the last one was added after the guard itself, because the original check only looked at the literal and stayed green with the guard removed.

The `vshot.desktop` that authorizes KWin is a **separate file** and cannot be merged into this one: its `Exec=` has to name `/usr/bin/vshot` exactly (KWin resolves the caller's pid to `/proc/<pid>/exe` and compares it against `Exec=`'s first word), and it is `NoDisplay=true`, because running `vshot` with no subcommand only reports that no output destination was given.

## Build

```sh
cargo build --release --locked                                # Rust CLI
cmake -S . -B build-qt -DCMAKE_BUILD_TYPE=Release             # Qt helper
cmake --build build-qt --parallel
```

OCR needs **ONNX Runtime's development files**: `ort-sys` finds `libonnxruntime.pc` through `pkg-config` (on Arch every one of the six variants ships it, together with the library and the headers), and links against the system copy rather than downloading another at build time. Without it the build fails and says why. The models live in the source tree's `models/` (`det.onnx` / `rec.onnx` / `dict.txt`, see [OCR](#ocr)); the build works without them, but `vshot ocr` then reports at runtime that it cannot find them.

Interactive features look for the helper in this order: the `VSHOT_QT_HELPER` environment variable, the directory holding the `vshot` executable, its relative `../build-qt/` and `../../build-qt/`, then `PATH`. You can also point at it explicitly:

```sh
VSHOT_QT_HELPER="$PWD/build-qt/vshot-qt-ui" target/release/vshot region --output shot.png
```

At runtime you need a Wayland session, `wl_compositor`, `wl_shm`, at least one `wl_output`, `zxdg_output_manager_v1`, and `zwlr_layer_shell_v1`. **The seat is required only where it is actually read**: `monitor <name>`, `all`, `window active`, and `region --geometry` never touch it; `monitor current` needs a `seat pointer`, and interactive selection needs both pointer and keyboard. Whichever is missing exits non-zero and names it.

## Usage

```sh
# Region capture
vshot region --output shot.png              # freeze, then drag
vshot region --clipboard
vshot region --geometry '100,200 800x600' --output shot.png

# Outputs
vshot monitor eDP-1 --output shot.png       # by name
vshot monitor current --clipboard           # the one the pointer is on
vshot all --output desktop.png              # whole desktop (gaps between outputs stay transparent)

# Windows
vshot window active --output window.png     # focused window
vshot window active --pixel                 # skip compositor metadata, use pixel detection
vshot window pick --output window.png       # click one on the live desktop
vshot window pick --pixel

# Long screenshot
vshot long --output long.png
vshot long --geometry '100,200 900x700' --output long.png
vshot long --ignore-top 48 --clipboard

# To stdout (logs go to stderr only)
vshot all --output - > desktop.png

# Pin straight to the screen, nothing written to disk
vshot region --pin

# Pin management
vshot pin shot.png another.png
vshot pin --clipboard                       # pin the clipboard contents
vshot pin --density 2 shot.png              # force the source density
vshot pin --toggle                          # show/hide all
vshot pin --show
vshot pin --hide
vshot pin --close-all
vshot pin --list
vshot pin --quit

# Settings: a window for the editor style and the command-line defaults
vshot settings

# OCR: read the text out of a region of the screen
vshot ocr                                   # frame a region, text to stdout
vshot ocr --clipboard                       # the same, onto the clipboard
vshot ocr --input shot.png                  # read an existing image file

# Recording: the screen to an MP4 (H.264, encoded on the GPU)
vshot record monitor eDP-1 --output clip.mp4    # one output
vshot record monitor --fps 30                   # the output you are on (NAME defaults to current)
vshot record all                                # every output, default video dir
vshot record window                             # one window's own pixels, occlusion and all
vshot record stop                               # stop the recording that runs
vshot record monitor current --duration 30      # stop itself after 30 seconds
```

The global options apply to every capture:

| Option | Meaning |
| --- | --- |
| `-o, --output PATH` | Write the PNG to PATH, expanding strftime formats like `%Y%m%d`; afterwards the file's `file://` URI is copied to the clipboard. `-` writes to stdout without copying. `record` reads the same flag as the video path (strftime expanded, `.mp4` added when missing, `-` refused, no URI copied) |
| `--clipboard` | Copy the PNG data to the clipboard |
| `--pin` | Pin the image to the screen instead of writing it (the daemon deletes the temporary file once it is in memory) |
| `-c, --cursor` | Ask the compositor to draw the cursor into every output frame. **Not supported by `long`** (see ["Known rough edges"](#known-rough-edges)); for the most common reason a capture has no cursor, see ["The cursor (`--cursor`)"](#the-cursor---cursor) |
| `--png-compression LEVEL` | `none` / `fastest` / `fast` (default) / `balanced` / `high`, all lossless, differing only in time and size |

Every capture must name exactly one output target. `region --geometry` and `--interactive` are mutually exclusive; with no geometry the default is interactive selection (`--interactive` states that intent explicitly).

Path format examples:

```sh
vshot region --output "$HOME/Pictures/vshot-%Y-%m-%d_%H-%M-%S.png"
vshot all --output 'shots/capture-%Y%m%d-%H%M%S.final.png'
```

`%Y` `%m` `%d` `%H` `%M` `%S` are year, month, day, hour, minute, and second; `%%` is a literal `%`. Prefixes and suffixes combine freely. Both file and clipboard output need `wl-copy`.

## Region capture and annotation editing

When `vshot region` gets no `--geometry`, the frozen frame fills each output and everything outside the selection is dimmed by a translucent mask:

- Drag to draw the rectangle, eight handles around it resize, dragging inside moves it, arrow keys nudge (Shift accelerates to 10 logical pixels)
- While dragging or resizing, an 8x magnifier and native pixel coordinates appear next to the cursor, and the selection's `width × height` sits at its top-left corner
- **Enter**, a double-click inside the selection, or the toolbar's OK confirms; **Esc** or right-click cancels the whole capture (Esc inside a text box only closes that box)
- The toolbar is a frosted floating panel (its background is a live blur sample of the frozen frame). Its first row holds the tools Select, Rect, Ellipse, Arrow, Draw, Text, Mosaic plus Undo, Redo, OK, Cancel; style sub-panels appear according to the current tool, always pop out on the side of the command bar facing away from the selection, can be dragged to a fixed spot, and follow the selection across screens
- Style entries: a color palette (with a custom picker: HSV gradient plus hex input), line style Solid/Dash/Dot, arrow head Open V/Filled, thickness 1-64, arrow size 1-8, font size 7-448 (the number *is* the pixel height, exactly as typed; the legacy integer the protocol carries is derived once, on the way out), mosaic shape Rect/Ellip/Brush, mosaic strength 1-3, and a system font list (each entry previewed in its own glyphs)
- Arrow draws a straight arrow from press to release; Draw is freehand; the mosaic strength controls both the pixel block size and the brush radius, and selecting an existing mosaic lets you change the strength directly
- The **Select** tool picks any annotation: click to select, drag to move (text too), shapes/lines/mosaics resize by their handles, Delete/Backspace removes it; style changes apply to the selected annotation immediately, and double-clicking text reopens it for editing
- **Ctrl+Z / Ctrl+Y** (or Ctrl+Shift+Z) undo/redo
- **Pasting an image**: the toolbar's *Image* button picks one from disk, or **Ctrl+V** pastes whatever image the clipboard holds — it lands centred at its own size, shrunk to fit when it is larger than the selection, and comes up selected so it can be dragged and resized by its handles; Ctrl+Z undoes it like any other mark
- **Reading text**: the toolbar's *Text+* button recognizes the text in the selection and puts it on the clipboard, the button itself flashing *Copied* or *Failed*, and a desktop notification follows (see [OCR](#ocr); `cli.ocr.notify` turns it off); the recognition runs in a `vshot ocr --input` child process
- Annotations come back to Rust in global logical coordinates and the final PNG is redrawn by the built-in software renderer, matching the preview; text is rasterized by Qt in the chosen font and composited as a bitmap, so the glyphs are identical

The UI language follows the system by default (`QLocale::system()`) and can be overridden with `VSHOT_LANG`: a value starting with `zh` selects Chinese, any other non-empty value selects English. The language is fixed when the helper starts, so switching needs a rerun. The Rust CLI's `--help` uses the same rule, so `VSHOT_LANG=zh vshot --help` is Chinese.

## OCR

`vshot ocr` reads the text out of a region of the screen. With no arguments it asks you to frame it:

```sh
vshot ocr                    # frame a region, text to stdout
vshot ocr --clipboard        # the same, onto the clipboard
vshot ocr --geometry '0,0 800x200'
vshot ocr --input shot.png   # read an existing image file
```

A finished recognition raises a **desktop notification** by default: the text it read (cut at 160 characters), or why it failed — when `vshot ocr` is started from a keybinding, nothing else says it is done. The notification is handed to whatever owns `org.freedesktop.Notifications` on the session bus, and **a session with no notification daemon still works**: it just misses the note (a line on stderr says so, and the text arrives all the same). The switch is on the settings window's *Text recognition* card, or in the file:

```json
{"cli": {"ocr": {"notify": false}}}
```

What lands on the clipboard is the recognized text itself, **with no trailing newline added**; one is added only for stdout, so the shell prompt does not end up on the last line of the output.

Recognition uses **PaddleOCR's PP-OCR models** (the official models converted to ONNX) on ONNX Runtime, **on the CPU**, in this process. Measured end to end on a 720p screenshot of code: about **240 ms**, and 13 px text still reads correctly.

The models are the `PP-OCRv6_small` tier, about 30 MB:

| File | Size | Role |
|---|---|---|
| `det.onnx` | 9.4 MB | text detection (language-independent) |
| `rec.onnx` | 20.3 MB | text recognition |
| `dict.txt` | 73 KB | the 18708-character alphabet |

The package installs them under `/usr/share/vshot/models/`; a source checkout keeps them in `models/` (`vshot ocr` walks up from the executable, so `target/release/vshot` and `target/release/deps/vshot-*` both find it). **They are not committed** — the PKGBUILD fetches and SHA-256-verifies them through `source=()`.

### Why the default is the CPU

Because it is fast enough, and the GPU costs are out of proportion:

- Measured on the recognition model: **4.0 ms on the CPU against 2.2 ms on the GPU** — invisible inside a 240 ms run
- On Arch, `onnxruntime-opt-rocm` depends on `rocm-hip-sdk` plus `rccl` (446 MB) and `migraphx` (787 MB) — **1.2 GB for those two alone**, before rocm-hip-sdk's twenty-odd packages. Dragging a screenshot tool's dependency list to 2 GB to save 1.8 ms is not a trade worth making.

So `depends` names the virtual package `onnxruntime` rather than pinning `onnxruntime-cpu` (about 46 MB with cpuinfo and protobuf) — see [Install](#install-arch-linux): pinning it would force anyone with a GPU variant to remove it, taking a whole chain of ROCm packages with it, when all vshot uses from that variant is the shared library and the `.pc` file.

### Using a GPU: the external engine

To reach a GPU, point vshot at **a program of your own**. It is handed a PNG, it writes the text on stdout, and vshot only starts it and reads that stdout — **vshot itself never links a GPU runtime**:

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

- `command`: the program and its arguments, as an **array** (no shell, so arguments with spaces need no escaping)
- `stdin`: `true` sends the PNG on stdin; absent or `false` appends the temporary PNG's **path** as the last argument instead
- `timeout`: seconds, 30 by default; on expiry the child is killed rather than left to hang the capture

What that program is does not matter — a Python `rapidocr` on the ROCm wheels, a `curl` to a service on another machine, another ONNX Runtime build with CUDA. Here is one with Python rapidocr:

```sh
#!/bin/sh
# /usr/local/bin/my-ocr — reads a PNG on stdin, writes text on stdout
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

**A misconfiguration is an error, never a silent fall back to the CPU**: `engine: "external"` with no `command`, a command that will not start, or a program exiting non-zero each produce a specific message (including whatever the program wrote to stderr). Someone who configured a GPU engine wants to hear that it did not run, not to be handed CPU output they did not ask for.

## Capturing windows

### window active

The focused window is obtained in this order:

1. **The compositor draws the window itself** (the only route that answers directly instead of cropping):
   - **KWin**: `CaptureActiveWindow`, with decorations and shadow (the shadow's outer ring is transparent), and the `scale` in the reply is the density;
   - **niri**: `niri msg action screenshot-window`, where niri renders the window to a PNG at a temporary path which vshot reads back. niri's IPC reports no absolute position for a tiled window, so on niri this is the only workable route. The render excludes the border (niri draws it on the tile), and vshot adds it back by converting through `tile_size` / `window_size` / `window_offset_in_tile`. For a translucent window it captures that output once more, locates the render on the frame, and crops the frame, so the translucent parts show the real background; when locating fails it first checks stability (content unchanged → fall back to niri's own translucent render; content moving → retry with a fresh render, at most 3 times). `--no-blend` skips the whole locating step and hands over niri's render as it is: never misaligned, but the translucent parts are empty and the border is not included;
2. Hyprland: `hyprctl activewindow -j`;
3. Sway: the focused node's `rect` from `swaymsg -t get_tree`;
4. KDE Plasma fallback: `kdotool` (used when installed) → a one-shot KWin scripting probe (reads `workspace.activeWindow.frameGeometry`, retrieved through the journal);
5. **Pixel detection**: when none of the above is available, detect the window on the captured frame.

`--pixel` skips 1-4 and does pixel detection on the frame directly (for testing the detector, and the only choice when all you have is a rectangle). Detection runs **per output** on that output's own native pixels, with candidates ranked by confidence (border band → flood segmentation → closed outline contour → the whole output), and it **includes the border the compositor drew**. When the compositor draws no colored border around the focused window, nothing in the picture distinguishes "the focused window" from "the window under the pointer", so candidates are ordered by "pointer's output → hit by the pointer → area", which answers with the window you are pointing at. A seamless borderless tiling layout (no gaps, no shadows) has no pixel signal at all, and vshot reports that honestly instead of guessing. `VSHOT_PIXEL_DEBUG=1` prints each level's verdict.

Only **the current session's own compositor** is asked (decided by `XDG_CURRENT_DESKTOP` / `XDG_SESSION_DESKTOP`; when neither names a compositor, every probe is tried). niri ignores both variables (they are unreliable for it and empty when a session is started manually from a TTY) and is recognized instead by the `NIRI_SOCKET` filename `niri.$WAYLAND_DISPLAY.$PID.sock`. With two compositors running at once this decision is essential, otherwise you would capture a window from the other session. `VSHOT_SESSION_DEBUG=1` prints the verdict and its basis.

### window pick

First you pick a window on the **live desktop**: moving the pointer highlights the window under it and dims the rest, with a hint bar at the top-left giving the window title and size; a left click finishes the pick, and Esc or right-click cancels. Then vshot **captures a fresh frame**, re-resolves the click position into a window rectangle against the current window list, and opens the same editing session `region` uses on that frame. Switching workspaces or moving windows during the pick therefore never leaves the result on a stale picture.

Candidates come from the compositor's window list (Hyprland `hyprctl clients`, Sway `swaymsg -t get_tree`, the KDE KWin scripting probe), ordered bottom-to-top in the compositor's own stacking order, and a pointer hit takes **the last window in the list containing the pointer** (that is, the topmost one). Candidates refresh with the pointer, and every 300 ms even when it does not move. An empty list or a failed query falls back to pixel detection.

`--pixel` skips the window list, does pixel detection on the frozen frame, and lets the user pick from all candidates.

**niri is the exception**: it has no usable window list, so vshot uses niri's own crosshair picker and niri renders the pixels itself — hence no dimming overlay and no annotation editor. Only `--pixel` returns to the overlay plus pixel detection.

## Long screenshots

`vshot long` stitches a scrolling region into one long image. Without `--geometry` it first asks for a selection (the same selection interaction as region capture, but confirming does **not** open the annotation editor). Then:

1. A hint bar appears in a screen corner reporting the stitched height and frame count, accepting **Enter / Space / left click** (keep the result) and **Esc / right click** (discard);
2. vshot sends the wheel at a fixed cadence (one batch every 120 ms by default, with `--notches` deciding how many notches per batch) while grabbing frames continuously at up to about 50 fps, aligning each frame with the previous one and appending newly revealed rows to the bottom of the long image whenever the shift is greater than zero;
3. At the end it writes through the usual output routes (`--output` / `--clipboard` / `--pin`). The result does not pass through the annotation editor; to annotate it, pin it first and use the pin's editing feature.

**Stopping conditions**: six consecutive wheel batches with no shift (the page is at its end), `--max-height`, `--max-frames`, `--timeout`, or a key press. When a frame's shift is beyond the measurable range the wheel is halved (down to a floor of one notch); only when several frames in a row still fail to line up does it stop and report why.

Alignment uses only the rows that **belong to the page**: runs of rows at the top and bottom that stay put (title bars, fixed toolbars, status bars) are chrome, never probed and never repeated — top chrome appears only at the very top of the long image and bottom chrome only at the very bottom. New rows are counted up from the page's bottom edge, and that edge is carried across frames and only ever moves up. Each frame is compared only with the previous one, so error does not accumulate.

| Option | Default | Meaning |
| --- | --- | --- |
| `--geometry` | interactive | A fixed global rectangle, formatted `x,y WxH` |
| `--notches N` | 1 | Wheel notches sent per batch |
| `--max-height N` | 30000 | Height cap of the stitched result, in pixels |
| `--max-frames N` | 6000 | Frame cap for one capture |
| `--timeout N` | 120 | Time limit for one capture, in seconds |
| `--ignore-top N` | 0 | Top N rows of each frame are excluded from matching, for sticky headers that move |
| `--inject M` | `auto` | Wheel backend: `wlr` (compositor virtual pointer, Hyprland/sway/niri), `portal` (XDG RemoteDesktop, one authorization prompt on KDE/GNOME), `uinput` (`/dev/uinput`, needs write access) |

`auto` picks in order of "least intrusive". `VSHOT_LONG_DEBUG_DIR=<dir>` saves every grabbed frame as `grab-NNNN.png` and appends every stitch decision to `steps.log`.

## Screen recording

`vshot record` writes the screen to an MP4: frames come from the same capture
backends the screenshots use (`zwlr-screencopy` on wlroots sessions, KWin's
ScreenShot2 on Plasma) and encoding happens on the GPU's media engine.

```sh
vshot record monitor eDP-1 --output clip.mp4    # one output; NAME as in `vshot monitor`
vshot record monitor                            # NAME defaults to `current`: where you are
vshot record all                                # every output, composed at its position
vshot record window                             # one window's own pixels (not the area it covers)
vshot record window firefox                     # by app id or title
vshot record window --pick                      # click the window to record
vshot record monitor --encoder hevc             # codec: h264 (default) / hevc / av1
vshot record monitor --fps 120                  # aim for 120 fps (1-240, default 60)
vshot record monitor --duration 60              # stop by itself after 60 seconds
vshot record stop                               # stop the running recording
```

- **Which output `current` is.** A recording has nothing of its own on screen,
  and a Wayland client sees no pointer at all until it owns a surface under it,
  so `current` cannot ask the seat the way a screenshot does. It asks the
  compositor instead — `hyprctl`, `swaymsg`, `niri msg` or KWin's D-Bus, the
  same query a pin uses to land on the right monitor: the output under the
  pointer where the compositor reports that (Hyprland), the focused output
  otherwise. When nothing answers, vshot says so and points at `monitor NAME`
  and `all`.

- **A window recording is not a recording of the area a window covers.**
  `record window` records the window's *own pixels*, which the compositor
  copies out itself (`ext_image_copy_capture_v1`, with the window's
  `ext_foreign_toplevel_handle_v1` as the source). So a window that is covered
  by another one records whole, one dragged half off the screen records whole,
  nothing behind it ever appears, and a window on a workspace that is not even
  visible records all the same — measured: a Discord window on a hidden
  workspace OCR'd back as Discord, while a screen recording of the same area
  showed the window that was actually there. Name the window by app id or
  title (whole name first, then a case-insensitive substring), `--pick` it with
  a click, or leave it out for the focused one. A compositor without these
  protocols is told to record a screen instead.
- **Two ways a window recording ends by itself.** If the window is *resized*
  or *closed* while recording, the recording ends there: the file is finished
  properly (trailer written) and stderr says why. One MP4 holds one frame size,
  and the encoder would refuse the frames a resize brings, so stopping there
  beats writing a broken file. If the window's output is off, disabled or
  disconnected, no frame ever arrives; that is reported after a few seconds
  rather than waited on forever.

- **Stopping.** `vshot record stop` (no display needed, so it binds to a
  compositor keybinding) or Ctrl+C in the terminal that started it. Both
  finish the file properly first — libavformat writes the trailer, the sample
  table and the index — so the result is always a seekable MP4. `--duration`
  ends a recording on its own.
- **Where it goes.** The global `-o/--output`, with strftime expanded; with
  no path the file lands in the videos directory as
  `vshot-%Y%m%d-%H%M%S.mp4` — `$XDG_VIDEOS_DIR`, else the one
  `~/.config/user-dirs.dirs` names (xdg-user-dirs keeps the localised name
  there, `~/视频` on a Chinese desktop), else `~/Videos` — and that directory
  is created when missing, because it is vshot's own choice of location. A
  path you name with `-o` is not created; a missing directory there is
  reported. A missing `.mp4` suffix is added; `-` (stdout) is refused,
  because a video is not something a terminal carries.
- **Frame rate.** `--fps` is what the loop *aims* for; each frame carries the
  wall time it was on screen into the MP4 (variable frame rate), so a
  recording that drops frames plays back short rather than slow. A single 4K
  output measures at a steady 120 fps here (the ceiling on this 7900 XT is
  around 149 fps), well past 60.
- **The codec.** `--encoder` picks h264 (the default), hevc or av1, all on the
  GPU's media engine. Encoding *and* muxing run on ffmpeg's libraries —
  libavcodec for the bitstream, libavformat for the MP4 boxes — the same
  route wf-recorder takes, loaded at run time with `dlopen`. A machine without
  the ffmpeg libraries still takes screenshots; only `record` reports what is
  missing. Needs `ffmpeg` and `libva` (for AMD/Intel VAAPI). Every frame is an
  IDR (all-intra), so any player reads it and any position is seekable.
- **Width limit 4096.** That is the hardware H.264 encoder's limit (measured
  on this machine's 7900 XT VCN), so `record all` across two 4K screens
  (5760 wide) is refused with that explanation — record one output instead, or
  switch to `--encoder hevc`, which encodes the 7680-wide desktop here. A
  single 4K screen (3840) is fine.

> Why not libva directly? We tried; the first implementation was exactly
> that. On this machine (mesa-git 26.3.0-devel, radeonsi) the encoder
> dereferences a null pointer inside `vaEndPicture` (`mov 0xb0(%rdi),%rax`
> with rdi = NULL) in about half of all runs, independently of the call
> shape — reused or per-frame coded buffers, `vaSyncSurface` or
> `vaSyncBuffer`, one thread or many all segfaulted alike. libavcodec's
> `h264_vaapi` runs the same hardware for hundreds of frames without a
> fault on the same machine, so the encoder boundary belongs to libavcodec.
> That is an engineering decision, not an aesthetic one.

## Pin overlay

`vshot pin` pins images to the screen as overlays, held by a **resident daemon**:

- **Drag** to move (across monitors; the copy on the other screen follows)
- **Wheel** zooms around the image center (0.1x–8x), with the factor briefly shown at the image's bottom-right
- **Double-click** closes that image
- **Left click** raises the image to the front, so a clicked one is always above the rest when they overlap
- Whichever image the pointer rests on gets a solid black outline; the others are light gray (2 logical pixels thick, not covering the image itself)
- **Its look is configurable**: corner radius, the shadow (its size, its drop and its darkness), border width and the two border colours live in the config file (square corners with a shadow by default) — see [`pin` — how a pinned image looks](#pin--how-a-pinned-image-looks)
- With the pointer over a pin and that screen holding the keyboard, press **Space** to enter the same annotation editor `vshot region` uses
- Right-clicking **any** pin opens a menu: a color card lists its formats first, and clicking one copies that value back to the clipboard (↑/↓ to move, Enter to copy, Esc to close; a badge flashes at the bottom-right once copied). The last row, **Save as…**, is there for every pin: it opens a save dialog and writes the image out as a PNG, then reports the result in the same badge

A new pin lands on the **active output**: the screen the pointer is on first, then the output holding keyboard focus when the pointer cannot be read, then the primary output.

What you can pin:

```sh
vshot pin a.png b.png          # image files
vshot pin --clipboard          # the clipboard's current contents
vshot pin a.png --clipboard    # the two mix freely
```

`--clipboard` contents are resolved in this order:

1. **Color** — when the clipboard carries `application/x-color`, a **color card** is pinned (this comes before images, because palette programs often put a color on the clipboard as a 1×1 swatch image too, and pinning that pixel would give you an invisible dot);
2. **Embedded image data** — `image/png`, `image/jpeg`, and the like;
3. **Copied files** — the first decodable local image in `text/uri-list`;
4. **Plain text** — whose content is a path to an existing local image;
5. **Plain text** — whose whole content is exactly a color literal, pinned as a color card;
6. **Plain text** — anything else is rendered as a text card, choosing the format by content: `text/html` present → rendered as HTML keeping syntax highlight colors; looks like markdown → GitHub-style rendering; looks like code → a monospace dark editor style; otherwise → a plain text card (following the system light/dark theme, with automatic wrapping).

A color card shows the color itself as a swatch on the left and, on the right, one line each for `HEX` / `RGB` / `HSL` / `HSV` / `CMYK` (plus `HEX8` and `RGBA` when translucent, and `NAME` when there is a named color), with a checkerboard behind the swatch to indicate transparency. Both text cards and color cards are rendered at the pixel density of their output, so they are not blurry on HiDPI.

An empty clipboard, content with neither an image nor usable text, or a missing `wl-paste` all produce a clear message and leave existing pins untouched.

### Pin sizing

A pin's size is decided by the **source density of the image**, and by default needs no arguments. The order is:

1. **Explicit** — `--density N` (1–4) or the `VSHOT_PIN_DENSITY=N` environment variable, which wins outright;
2. **vshot's own screenshot** — carrying the scale of the output it came from;
3. **The image's own statement** — the PNG `pHYs` chunk (192 DPI = 2x; 96 DPI is 1x). The decision looks at **whether the chunk is there** rather than at the value Qt reports, because a PNG with no `pHYs` is also read by Qt as roughly 96 DPI. Every PNG vshot writes carries this chunk;
4. **A record left by the screenshot tool** — a lone number in a `<image path>.scale` file, or a line `<image path> <scale>` in `$VSHOT_PIN_SOURCE_FILE` (default `/tmp/screenshot-path`; the path must match the image being pinned). A screenshot script only has to write one line after saving:

   ```sh
   echo 2 > "$shot.png.scale"
   ```

5. **Inferred from the image size and the output it lands on** — 1:1 when the image's pixels fit the landing output's native resolution; otherwise the scale of "the smallest screen that can hold it", finally capped once by the output's width (never enlarged).

So the same image pinned on a 2x 4K screen occupies half the logical size it does on a 1080p screen, and the **physical size is the same on both**. `--density` is the final manual fallback. `VSHOT_PIN_DEBUG=1` prints each pin's decision (pixel count, final density, source, landing screen's `devicePixelRatio`), and `VSHOT_PIN_FOCUS_DEBUG=1` prints every focus change of every render surface (for finding out why an outline color is wrong).

### Pin edit mode

With a pin focused, press **Space** and the daemon exports that image and brings up the full annotation editor (toolbar, text, mosaic, undo/redo), the same as region capture:

- The editor covers the whole screen the pin is on but **does not draw the image itself** — the picture on screen is the real pin, and the editor only overlays annotations on it; anything drawn beyond the image is cropped away;
- Dragging with the select tool on the image reuses the pin's own move logic through the daemon socket and **creates no copy**; arrow keys nudge, with Shift accelerating to 10px. The image cannot be dragged off screen;
- **Esc cancels**: annotations are discarded and the pixels stay as they were, but the position does not roll back — wherever you dragged it, there it stays. **Enter or OK confirms**, writing the annotations back with it;
- Only one pin can be in an editing session at a time.

### pin daemon

A layer-shell overlay surface belongs to the process that created it, so a resident process is needed:

- The Qt binary is reused: `vshot-qt-ui --pin-server <socket>` is the daemon, and the first `vshot pin` that cannot reach the socket starts it detached (not holding the terminal);
- The CLI is a thin client sending one-line JSON requests over a Unix socket; the socket defaults to `$XDG_RUNTIME_DIR/vshot-pin-<uid>.sock` and can be overridden with `VSHOT_PIN_SOCKET`
- About 0.5 s after the last pin closes (or after `--close-all`) the daemon exits on its own, and the next pin command starts it again. `vshot pin --quit` exits it manually at any time;
- **Never end the daemon with `pkill` / `kill -9`**: it holds layer-shell surfaces, and when killed hard some compositors (Hyprland 0.56 measured) leave the surface and its screencopy session behind, which makes **screencopy block forever on every output** (`vshot` and `grim` all time out, and `hyprctl reload` does not recover it — only restarting the session does). Always use `vshot pin --quit`, which unmaps every overlay before exiting; the daemon also handles `SIGTERM` / `SIGINT` through the same graceful path.

A Wayland client receives no global key presses, so "one-key show/hide" has to be bound to a compositor shortcut yourself, for example on Hyprland:

```
bind = SUPER, P, exec, vshot pin --toggle
bind = SUPER SHIFT, P, exec, vshot pin --close-all
```

## Images and output mapping

Internal frames are uniformly RGBA8 with a top-left origin. Multi-output composition supports negative logical origins and gaps between outputs; the scene canvas uses the highest output scale, and lower-scale outputs are enlarged with nearest-neighbor.

**A rectangle that falls inside a single output is always cropped from that output's own native frame and written with that screen's scale as its density**: `region`'s `--geometry` and interactive selections, `window active`, `window pick`, and rectangles from pixel detection all take this route, and only rectangles **crossing a seam** fall back to the composed scene; `all` is the whole desktop and can only come from the scene. This rule is necessary: cropping a rectangle on a 1080p screen out of the composed scene would give an image twice as large and blurry.

Positive integer scales, `transform=normal`, and a provably safe logical/pixel mapping are currently required; fractional scale, rotation, and mappings that cannot be proven fail clearly rather than producing a plausibly wrong screenshot. This validation applies only to the routes that **need to compose outputs into a scene**, so KWin's and niri's two routes that hand over window pixels directly still work on a rotated or flipped output.

## Capture backends and KDE authorization

Probed once at startup:

1. `wlr-screencopy-unstable-v1` is tried first. Only a failure of "missing `zwlr_screencopy_manager_v1`" means this compositor does not provide the protocol;
2. **KWin ScreenShot2** — KWin's private session-bus service `org.kde.KWin.ScreenShot2` (KWin has neither screencopy nor `ext-image-copy-capture`). vshot passes the write end of a pipe, and KWin writes the pixels into it and reports `width` / `height` / `stride` / `format` / `scale` in its reply. The pixels are premultiplied-alpha BGRA; output screenshots normalize alpha to 255, and window screenshots keep it as is.

When neither backend works, the error names both causes. Neither implements PipeWire, Portal ScreenCast, or DMA-BUF, and GNOME/Mutter provides none of the three (nor layer-shell), so **GNOME is not supported**.

### KDE authorization

KWin's `ScreenShot2` is a **restricted D-Bus interface and shows no dialog at all** — there is no "allow" button. From the caller's pid, KWin takes `/proc/<pid>/exe`, finds the installed desktop file whose `Exec=` first word matches that path, and requires it to declare:

```ini
X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2
```

- **A package-installed vshot needs nothing extra**: the packaged `/usr/share/applications/vshot.desktop` already declares it. If KWin still refuses after installing, run `kbuildsycoca6 --noincremental` once; a newly created desktop file may also take a few seconds to be picked up, so retry after a moment;
- **A development build run straight from `target/release/vshot` gets no authorization** — no desktop file's `Exec=` points at that path. Add one (write the current binary's absolute path into `Exec=`; vshot prints that path in its error):

  ```sh
  printf '[Desktop Entry]\nType=Application\nName=VShot\nExec=/path/to/vshot\nNoDisplay=true\nX-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2\n' \
    > ~/.local/share/applications/vshot.desktop
  kbuildsycoca6 --noincremental
  ```

- On the compositor side, `KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1` skips the whole check, **for development and testing only**.

## Compositor support and verification status

### What each feature uses

| Feature | Hyprland | niri | KWin/Plasma | Sway | labwc |
| --- | --- | --- | --- | --- | --- |
| Screen capture | wlr-screencopy | wlr-screencopy | KWin ScreenShot2 | wlr-screencopy | wlr-screencopy |
| `window active` | `hyprctl activewindow -j` | niri's own `screenshot-window` | KWin `CaptureActiveWindow` | `swaymsg -t get_tree` | ❌ none |
| `window pick` | `hyprctl clients -j` | niri's own crosshair picker | KWin scripting probe / `kdotool` | `swaymsg -t get_tree` | ❌ none |
| Which screen a pin lands on | `hyprctl cursorpos` + `monitors -j` | `focused-output` (keyboard focus only) | `org.kde.KWin.activeOutputName` | `swaymsg -t get_outputs` | ❌ none |
| Scroll injection | wlr virtual pointer | wlr virtual pointer | portal / uinput | wlr virtual pointer | uinput |

Where nothing is adapted, vshot **degrades automatically instead of erroring**: with no window list it falls back to pixel detection, and when it cannot ask which screen the pointer is on it falls back to the primary screen Qt reports.

### How far verification goes

- **Hyprland** — this machine's session is Hyprland and it is the main development and verification environment: capture, selection and annotation, windows, long screenshots, pins, and scroll injection have all run here.
- **niri** — both the tiled and floating capture paths were verified on a real session: tiled windows were measured with two side-by-side kitty windows (residual 0.45/0.51 per channel), and floating windows go through niri's `tile_pos_in_workspace_view` coordinates plus `matches_at_position` verification, with correct results on the real session. If a floating window capture comes out misaligned, `--no-blend` bypasses the locating step.
- **KWin/Plasma** — D-Bus capture (the size and opacity of `CaptureScreen`, `native-resolution`, format fields), the window list, and long screenshots have all been tested, where the capture and window list automation ran against a **headless `--virtual` KWin**, and therefore:
  - `--cursor` passes `include-cursor` but **whether a cursor is really drawn is unverified** (there is no pointer to draw on a headless output);
  - **Long screenshots are verified working**. KWin's `CaptureArea` is private API (argument order unchecked), so on KDE vshot captures the whole screen and crops, which costs noticeably more per frame than on other compositors;
  - **Scroll injection is unverified**. `portal` goes through KWin's own EIS server, but this machine's Hyprland portal does not implement RemoteDesktop, so **that path has never run on real hardware** and needs acceptance on KDE; `uinput` needs write access to `/dev/uinput`.
- **Sway** — probe implementations and unit tests only, **no live verification**.
- **labwc and other compositors providing wlr-screencopy** — basic capture should work in theory, but there is no window list and no pin-landing probe, and there is **no live verification**. Only the operations that read pixels (`region --geometry`, `monitor <name>`, `all`) are usable; `monitor current` and interactive selection need a pointer/keyboard seat, which usually works too.
- **GNOME** — unsupported, with no plan to implement it. Mutter provides neither wlr-screencopy nor KWin's D-Bus service, and not even layer-shell.

### The cursor (`--cursor`)

vshot never draws a cursor itself; `--cursor` only sets an "overlay the pointer" flag on the compositor's capture request (`wlr-screencopy`'s `overlay_cursor`, KWin's `include-cursor`), and whether, where, and when it is drawn is entirely up to the compositor. Two conclusions follow from measurement:

- **A capture with no cursor after typing a command in a terminal is not a bug.** A terminal (kitty measured, `mouse_hide_wait` defaults to 3.0 seconds) hides the pointer after a few seconds without mouse movement — it performs `set_cursor(null)` on the compositor, after which there **really is no cursor to draw** at the compositor level, and any tool going through screencopy sees the same thing (`grim -c` cannot grab it at that moment either). Move the mouse before capturing after pressing Enter, or set `mouse_hide_wait 0` for kitty (the pointer then no longer hides itself while typing). The same "hide the pointer while typing" behavior in other terminals and programs behaves identically.
- **On niri the pointer is drawn into the window capture, not the output frame.** On niri, `window active` / `window pick` go through niri's own `screenshot-window`, and `--cursor` maps to that call's `--show-pointer`, so the cursor lands in the window image; `--show-pointer` only exists after 25.11, and an older niri rejects the whole request, which vshot detects and retries without the argument, saying "the pointer cannot be drawn into the window". The output-level paths (`monitor`, `all`, `region`) still use screencopy's `overlay_cursor`.

`--cursor` has no effect on `long` (see the next entry); whether KWin really draws a cursor is unverified.

### Known rough edges

- **`--cursor` does nothing on `long`** — the frame grabbing in `src/longshot.rs` hardcodes the cursor argument to `false` (and `longshot::run` has no such parameter), so `vshot long --cursor` is accepted and silently ignored. On the other capture paths (`region`, `monitor`, `all`, `window active`) `--cursor` works as measured on this machine's Hyprland. On KWin `include-cursor` is passed, but **whether a cursor is really drawn is unverified**.
- **Pixel detection (`--pixel`)** — a seamless borderless tiling layout (no gaps, no shadows) and a completely uniform desktop offer no pixel signal, and vshot reports that honestly instead of guessing. When the compositor draws no colored border around the focused window, it answers with **the window under the pointer**, which may differ from the compositor's focused window. `VSHOT_PIXEL_DEBUG=1` shows each level's verdict.
- **niri translucent window locating** — template matching requires the window's content to stay put between the render and the frame grab; video and animation make the template stale (vshot retries with a fresh render up to 3 times), and a nearly fully transparent window or one hanging off the output's edge cannot be located at all (falling back to niri's own translucent render).
- **The KDE window list probe** — depends on journald receiving KWin's `console.info`. When KWin is started from a tty and its log goes only to that tty, no line can be retrieved no matter how long you wait. With `kdotool` installed vshot prefers it (the result comes back over D-Bus, not the journal), bypassing that dependency.
- **niri session detection** — niri sets neither `XDG_CURRENT_DESKTOP` nor `XDG_SESSION_DESKTOP` (they are empty when a session is started manually from a TTY), so detection uses the `NIRI_SOCKET` filename instead. When it gets this wrong the symptom is `window pick` opening the dimming overlay instead of niri's crosshair picker; `VSHOT_SESSION_DEBUG=1` shows what it decided.
- **niri pin landing** — it can only ask "the output the focused window is on" (niri's IPC has no pointer position query), so it **follows the keyboard focus, not the pointer**.
- **pin daemon and screencopy** — do not end the daemon with `pkill`/`kill -9` (see ["pin daemon"](#pin-daemon)), or some compositors leave the layer surface and its screencopy session behind, making screencopy block forever on every output.

## Configuration file

`vshot` remembers two things in `$XDG_CONFIG_HOME/vshot/config.json` (or `~/.config/vshot/config.json`): the **annotation editor's style**, and **defaults for some command-line flags**. The file is optional — missing, unreadable, or malformed all fall back to the built-in defaults and never affect a capture.

There are two ways to change it: **edit the file by hand**, or run **`vshot settings`** for a window over both sections, saved as you press Save. **Nothing a capture session does is written back**: the colour you picked, the width you dragged to, the tool you switched to are that session's working state. The config is the *reset* value every session starts from, and it changes only when you say so — a settings-window save or a hand edit.

```bash
vshot settings
```

The window is an ordinary window: it captures nothing and needs no compositor protocol, so it also works on a compositor vshot cannot otherwise capture. After installing the package you can also open it from the application menu as **VShot Settings** (see ["Application menu entry"](#application-menu-entry)). An empty or zero value in the `cli` section means "leave it unset, use the built-in default" rather than storing a zero; the `editor` section is always written whole. Saving **merges**: keys this build does not recognize — a newer vshot's, or your own — survive untouched instead of being wiped by a save.

The window has four pages, switched from the sidebar: **Annotation editor** (tool, color, width, line style, arrow, text, mosaic), **Command-line defaults** (compression, default monitor, pin density, the scrolling-capture settings, and the notification a finished recognition raises), **File dialogs** (corner radius, border width and colour, and the shadow) and **Pin appearance** (corner radius, the shadow, border width, and the two border colours). Each page is a column of cards, one setting per row with the label on the left and the control on the right, and they fit without a scrollbar at the default window size. The combo and spin boxes paint their own chevrons — the native ones are beveled triangles from a different decade — so the controls match the toolbar's look. Saving **does not close the window**: a "Saved." note appears in the corner, so a value can be changed, saved, looked at and changed again without reopening anything. Cancel is now only "close".

When drawing them, note that **the `QPainter` in a `paintEvent` already works in logical pixels**: Qt has folded the output's scale in, so dividing by `devicePixelRatioF()` as well would halve every coordinate on a scale-2 output and jam the chevron into the corner. That mistake is invisible at 1x, so it is worth looking at both scales.

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

### `editor` — the editor's style

This section is the style **every session starts from**. Nothing a session does is written back — this file holds the reset values, not the last session's leftovers; change them through `vshot settings` or a hand edit.

| Key | Values | Default |
| --- | --- | --- |
| `tool` | `select` / `rectangle` / `ellipse` / `arrow` / `pen` / `text` / `mosaic` | `select` |
| `color` | `#rrggbb` or `#rrggbbaa` | `#ff4040ff` |
| `width` | 1–64 | `2` |
| `textPixels` | 7–448 (a pixel height) | `14` |
| `dash` | `solid` / `dashed` / `dotted` | `solid` |
| `arrowSize` | 1–8 | `1` |
| `arrowStyle` | `open` / `filled` | `open` |
| `mosaicShape` | `rect` / `ellipse` / `brush` | `rect` |
| `mosaicStrength` | 1–3 | `2` |
| `font` | font family; an empty string uses the system default | `""` |

`tool` is the tool an **editing** session starts with and, like every style here, it lives **only in the config**: nothing a session does is written back. A fresh region capture always opens on Select — its first step is dragging the rectangle, and opening straight onto a drawing tool (say Text) makes the first click place a label; `window pick` and scrolling capture stay on Select for the same reason. Out-of-range integers are clamped, and an unrecognized name falls back to the default — a typo in a hand-edited file costs you that one setting, not an error.

**`textPixels` is a pixel height**: type 14 and the label is 14 pixels tall, exactly matching the number in the editor's size box. The legacy "glyph multiple" scale the JSON protocol still carries (1–64, one cell being 7 pixels) is derived once, on the way out, and never shown.

Earlier versions called this key `textSize` and stored that multiple. **An old file is migrated automatically**: a `textSize` is converted on read (`2` → 14 px, `3` → 21 px), and the next save writes it as `textPixels` and drops the old key. When both are present, `textPixels` wins. The key was renamed rather than reused because 7–64 is a legal value under either reading, so no guess could tell them apart.

### `cli` — command-line defaults

Supplies values for flags **not given on the command line**. The order is:

```
command line > environment > config file > built-in default
```

| Key | Flag | Built-in default |
| --- | --- | --- |
| `png-compression` | `--png-compression` | `fast` |
| `monitor` | the output name for `monitor [NAME]` | `current` |
| `long.notches` | `long --notches` | `1` |
| `long.max-height` | `long --max-height` | `30000` |
| `long.max-frames` | `long --max-frames` | `6000` |
| `long.timeout` | `long --timeout` | `120` |
| `long.ignore-top` | `long --ignore-top` | `0` |
| `long.inject` | `long --inject` | `auto` |
| `pin.density` | `pin --density` | inferred |
| `ocr.engine` | which engine `vshot ocr` uses | `builtin` |
| `ocr.external.command` | the program to run when `engine` is `"external"` (an array) | none |
| `ocr.external.stdin` | send the PNG on stdin instead of passing a path | `false` |
| `ocr.external.timeout` | the external program's timeout, in seconds | `30` |
| `ocr.notify` | a desktop notification when recognition ends | `true` |

`pin.density` follows the same order: `--density` > `VSHOT_PIN_DENSITY` > the config file. Unknown keys inside `cli` are ignored rather than making the whole file invalid — a misspelled key costs you that one setting, and the rest still apply.

`ocr.engine` accepts only `builtin` and `external`; any other name is an **error** rather than a default, because a misspelled `external` would otherwise look like a working GPU engine. Likewise `engine: "external"` with no `command`, or a command that will not run, is reported plainly (see [Using a GPU](#using-a-gpu-the-external-engine)). The settings window covers `editor`, the common `cli` entries and the `ocr.notify` switch; `ocr.engine` and `ocr.external` are edited by hand. That switch **only ever writes "off"**: an absent key already means on, so writing `true` would say nothing the file did not already say.

`color` uses the CSS spelling: `#rrggbb`, or `#rrggbbaa` with the alpha **last** when it is not opaque. Note that this differs from Qt's own eight-digit order (`#aarrggbb`); both `vshot settings` and the config file follow CSS.

### `dialog` — the file dialogs' look

The save and open windows (a pin's **Save as…**, pasting a local image in the editor) are **layer surfaces**, and a compositor draws no decoration on one — so the rim and the shadow this section describes are the only things separating them from whatever is behind.

| Key | Values | Default |
| --- | --- | --- |
| `radius` | 0–48, logical pixels | `12` |
| `borderWidth` | 0–8, logical pixels; 0 draws no rim at all | `1` |
| `borderColor` | `#rrggbb`; **leave it out** to derive one from the colour scheme | none |
| `shadow` | `true` / `false` | `true` |
| `shadowSize` | 0–64, logical pixels | `14` |
| `shadowOffset` | -32–32, logical pixels | `3` |
| `shadowOpacity` | 0–255 | `120` |

With no `borderColor` the rim is derived from the dialog's own colours — a stroke a little darker than the surface — so it reads as an edge under a light or a dark scheme without a second value to keep in step. Write one and that is what is used.

The shadow's four keys are **the same set the `pin` section has**, with the same meaning, the same ranges and the same defaults (see below). A layer surface can only be the size it asks the compositor for, so the shadow is painted in a ring the window keeps inside itself: the window is the dialog plus that ring on every side, the dialog's contents are inset by the same amount, and the shadow lands in what is left. `shadowSize` is therefore also how much smaller the dialog is than its window — it is the ring's width, not something added outside it.

### `pin` — how a pinned image looks

A pin is a layer surface with nothing but the image in it, so its corners, the shadow behind it and the line around it are all vshot's to draw. This section is **not** the same thing as `cli.pin`: that one is a pin's *size* (its source density, a command-line default), this one is how it *looks*.

| Key | Values | Default |
| --- | --- | --- |
| `radius` | 0–512, logical pixels; 0 is a square corner | `0` |
| `shadow` | `true` / `false` | `true` |
| `shadowSize` | 0–64, logical pixels; 0 means no blur | `14` |
| `shadowOffset` | -32–32, logical pixels; negative lifts the shadow above | `3` |
| `shadowOpacity` | 0–255 | `120` |
| `borderWidth` | 0–8, logical pixels; 0 draws no border at all | `2` |
| `borderColor` | the border on an idle pin; the built-in light grey `#c0c0c0` when absent | none |
| `activeBorderColor` | the border on the pin the pointer is over while that output holds the keyboard; the built-in black when absent | none |

The default is **square corners with a shadow**: a screenshot is a picture of a window, and rounding it would cut into what it shows — while a screenshot pinned over a window of its own colour has no visible edge at all without one.

The shadow's three numeric keys: `shadowSize` is how far the blur reaches past the edge, which is what its softness is; `shadowOffset` is how far the whole shadow is dropped — light comes from above, hence the default of 3, and a negative value puts it above the pin instead. `shadowOpacity` is the shadow's darkness; the blur spreads it rather than adding to it. `shadow` is the master switch, and **turning it off keeps the numbers**, so it can be turned off for a moment and back on with the size still there. A `shadowSize` of 0 is no blur, and paints nothing either.

`radius` is a ceiling rather than a promise: what is actually painted is clamped to half the image's shorter side, past which a corner stops being a corner and becomes a lozenge. A pin's size changes with every wheel step, so that can only be decided at paint time. The border is **centred on the image edge**, half outside and half in, so changing its width leaves a rounded pin and a square one the same size.

The shadow is built once and cached — blurred at a third of the size and scaled back up, since a soft shadow has no detail to lose and a full-size blur of a 4K pin would cost more per frame than drawing the pin did. A drag re-uses it; a zoom step or a config change rebuilds it. **A config change takes effect on the next `vshot pin`**: the daemon stays up while anything is pinned, but it re-reads the file every time a pin is added, so there is no need to restart it by hand.

## Environment variables

| Variable | Purpose |
| --- | --- |
| `VSHOT_LANG` | UI and `--help` language: a value starting with `zh` selects Chinese, any other non-empty value selects English, unset follows the system |
| `VSHOT_QT_HELPER` | Path to `vshot-qt-ui` |
| `VSHOT_PIXEL_DEBUG=1` | What each level of window pixel detection saw |
| `VSHOT_SESSION_DEBUG=1` | Which compositor this session was judged to be, and on what basis |
| `VSHOT_LONG_DEBUG_DIR=<dir>` | Write every long-screenshot frame and every stitch decision to disk |
| `VSHOT_OCR_MODELS=<dir>` | OCR model directory, overriding `/usr/share/vshot/models` and the search beside the executable |
| `VSHOT_PIN_SOCKET` | The socket path the pin daemon listens on |
| `VSHOT_PIN_DENSITY=N` | The source density of every pinned image, same as `--density` |
| `VSHOT_PIN_DEBUG=1` | The daemon prints every pin's density decision |
| `VSHOT_PIN_FOCUS_DEBUG=1` | The daemon prints every focus change of every pin render surface |
| `VSHOT_PIN_SOURCE_FILE` | Overrides the screenshot tool's record path (default `/tmp/screenshot-path`) |
| `VSHOT_RECORD_PIDFILE` | The pid file `vshot record stop` reads (default `$XDG_RUNTIME_DIR/vshot-record-<uid>.pid`) |
| `VSHOT_RECORD_DEBUG=1` | The recording loop traces each frame's stage (grab/encode/mux) and the libavcodec version in use |

> As soon as any `VSHOT_PIN_*_DEBUG` is set for the daemon, it stops sending its stderr to `/dev/null`, so the traces are readable. The variables must be in place when the daemon starts; for one already resident, run `vshot pin --quit` first.

## Known limitations

- **KWin's frozen overlay still depends on `zwlr_layer_shell_v1`** (which KWin provides). Authorization is not a dialog and only recognizes desktop files, so a build run straight from `target/` will always get `NoAuthorized` under Plasma;
- **Several compositors can coexist under one `XDG_RUNTIME_DIR`**, and when `WAYLAND_DISPLAY` is unset libwayland uses the default `wayland-0` (which is the case for a shell in a tty or over ssh), so a command may connect to "the other compositor". For every compositor-related failure vshot additionally prints which display it connected to and which displays exist;
- **Long screenshots**: the selection must lie entirely within one screen; a fixed bar that only appears at some scroll positions (a floating toolbar) is still treated as page content, so exclude it with `--ignore-top N`; a lazily loading page may duplicate or miss a few rows; the frame rate on KDE is noticeably lower than on other compositors (it goes through D-Bus capturing the whole screen and cropping, and that path is not verified live); the time for grabbing and aligning grows linearly with the region's area, so **use a release build for long screenshots**;
- **Pixel detection**: a seamless borderless tiling layout and a completely uniform desktop have no pixel signal, and vshot reports that honestly instead of guessing; when the compositor draws no colored border around the focused window, `window active --pixel` answers with "the window under the pointer";
- **Text**: the Qt text box accepts arbitrary Unicode (including CJK submitted by an input method); a result from an old helper that carries no bitmap falls back to Rust's built-in 5x7 font, which only supports printable ASCII;
- **`monitor current`** depends on receiving pointer enter/motion on the overlay; generic Wayland has no readable global mouse position, so vshot never guesses with the first output;
- Native screencopy waits up to 10 seconds for the compositor to return a frame, and times out with an error instead of blocking forever.

## Verification

```sh
cargo fmt --check
cargo test --locked
cargo clippy --locked --all-targets --all-features -- -D warnings
cargo build --release --locked
```

The Qt helper has no test framework, only **offscreen checks that need no compositor** (not built by default; add `-DVSHOT_BUILD_CHECKS=ON`), covering config file reads and writes and the settings window, the text size conversion, clipboard color parsing and color card rendering, a pin's self-declared density, pin outlines, text card padding, the color card's right-click menu, and the export format of a pasted image:

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

`vshot-paste-check` pins the **cross-language file channel** an image paste travels over: the helper writes the pixels as raw RGBA into `image-N.rgba` and leaves only the path in the JSON, and Rust reads them back by that path. The two ends are two languages with two type systems, so what is worth locking down is the bytes themselves — the channel order (straight-alpha RGBA8888, not BGRA and not premultiplied), whether the declared dimensions agree with the file length (the Rust reader checks exactly that), and whether the rect in the JSON is the one the pixels were rasterized for. A mistake in any of them is invisible on the helper side and shows up only as wrong colours or a stretched image in the final PNG. It covers the placement rules as well: an image larger than the selection is shrunk to fit and centred, a smaller one keeps its own size, the paste arrives selected so its handles work, and undo/redo does not lose the pixels — a paste is one history step, and the annotation that comes back still carries its own pixels and rect. The last section goes through the toolbar: it creates a real region overlay (created only, never shown — no layer surface is touched), runs `beginPresetEdit` to put the command bar up, and finds the *Image* button on it by object name. Without that section the feature would be reachable from Ctrl+V alone, and nothing here would go red.

`vshot-text-size-check` pins the font-size conversion: the number on the panel *is* the pixel height, and writing it into the legacy protocol projects it back onto the whole glyph multiples the 5x7 fallback font can draw (`ui/text_size.hpp`). The two ends have to line up (7–448 is exactly scale 1–64), whole multiples have to round-trip, and a value in between has to round to the *nearest* multiple rather than truncate — truncating would shrink a 15 px label to 14 px whenever the fallback rendered it.

`vshot-config-check` covers the part most likely to fail silently: whether a save really **merges** (keeping keys this build does not recognize), whether clearing a value really removes it, and whether `#rrggbbaa` parses as CSS (Qt itself reads that as `#aarrggbb`, turning "opaque orange" into purple). `vshot-settings-check` builds the real settings window, drives every one of its widgets, and reads the config file back — a field wired to the wrong member is visible only that way. It is worth running whenever the window's layout changes: it finds widgets by object name, so a re-layout or a switch to a different widget class does not hide anything, and it only goes red when a field is genuinely mis-wired.

`vshot-pin-outline-check` pins down a pin's **pixels**: that the stroke is a solid, fully opaque line rather than a two-tone ring with a translucent outer edge, that both of its states share one geometry (so a focus change can only recolour and never shift the edge or change its weight), and everything the config file can change — that a corner radius really cuts the corner away, that a 4px stroke lands exactly 2px either side of the edge, and that the shadow paints outside the pin and nowhere when it is off. **This is the one place that goes red when the shadow arrives**: the other two pin checks measure the right-click menu as "whatever is painted outside the pins", so they turn the shadow off explicitly — without that, a shadow that is on by default makes them fail for reasons that have nothing to do with what they test, which is exactly how this was found.

There are also 5 integration tests that are **not run** by default (`#[ignore]`), needing a real environment: KWin's D-Bus capture and backend selection (needs a running KWin; a headless KWin suffices, see the comments in `src/capture/kwin.rs`, with a virtual output named `Virtual-0`, 1024x768, no pointer capability, so it only covers the D-Bus capture layer), the active output probe (needs any real session), `/dev/uinput` scroll injection (needs write access), and **the built-in OCR engine reading drawn text** (needs those 30 MB of models on disk, which `cargo test` has nowhere to fetch them from). To run them:

```sh
cargo test -- --ignored              # all of them
cargo test --release ocr:: -- --ignored --nocapture   # just the OCR one
```

The OCR test looks for the models in the source tree's `models/` by default, and `VSHOT_OCR_MODELS=<dir>` points it elsewhere. It covers what no unit test can: that the models load, that the pipeline is wired the right way round, and that the characters come back in one piece. The text is **drawn** with the editor's own software renderer rather than typed, so it travels the path a screenshot's text does.

Starting a headless KWin:

```sh
mkdir -p /tmp/kwin-e2e/{cfg,data,cache}
KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1 XDG_CONFIG_HOME=/tmp/kwin-e2e/cfg \
  XDG_DATA_HOME=/tmp/kwin-e2e/data XDG_CACHE_HOME=/tmp/kwin-e2e/cache \
  kwin_wayland --virtual --socket wayland-ke2e --no-lockscreen \
  --no-global-shortcuts --no-kactivities &
XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=wayland-ke2e \
  cargo test -- --ignored --nocapture
```

`VSHOT_KWIN_E2E_OUTPUT` / `_WIDTH` / `_HEIGHT` / `_COLOR=R,G,B` override the default output name, size, and center pixel color assertion.

## License

MIT (see `LICENSE`).
