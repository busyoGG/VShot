# vshot

[中文](README.md) | **English**

> **This project is pure vibe coding**: requirements come from a human, code and docs are written by AI.

A Wayland screenshot tool written in Rust, with a Qt interactive UI and a resident pin overlay. Captures are **strictly frozen**: the desktop is photographed into a still frame first, and every selection and annotation happens on that frame, so nothing on screen moves while you are choosing.

Works with Hyprland, niri, KWin/Plasma, Sway, and basic capture on any compositor providing `wlr-screencopy` (such as labwc).

> **Verification varies**: every Hyprland feature was tested live; niri's tiled and floating window capture paths, and its **window recording and replay**, were verified on a real session; KWin/Plasma's D-Bus capture, window list, and long screenshots were tested, but `--cursor` and scroll injection were not; Sway and labwc are **untested on a live session**. See ["Compositor support and verification status"](#compositor-support-and-verification-status).

## Features

- **Region capture** — drag on the frozen desktop, or give a fixed geometry; eight resize handles, with a magnifier and size readout following the pointer
- **Annotation editing** — rectangles, ellipses, arrows, freehand, text, mosaic; undo/redo, move and resize a selection, colors and line styles, arrow head styles, font sizes, system font picker
- **monitor / all** — capture one output by name or by pointer position, or compose the whole desktop by logical position
- **window active / pick** — the focused window, or click one on the live desktop; KWin and niri hand over the window's own pixels
- **Long screenshot** — frame a scrolling region, and vshot sends the wheel, grabs frames, aligns them by content, and stitches one long image
- **Screen recording** — one output, the whole desktop, a rectangle or a window's own pixels to an MP4, GPU-encoded (VAAPI or NVENC), with an optional microphone track or the recorded window's own audio
- **Replay** — encode continuously but keep only the last N seconds in memory; a key writes that stretch to an MP4 (a stream copy, no re-encode)
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
vshot record region --geometry '0,0 800x600'    # a rectangle, desktop coordinates
vshot record window                             # one window's own pixels, occlusion and all
vshot record stop                               # stop the recording that runs
vshot record monitor current --duration 30      # stop itself after 30 seconds

# Replay: keep the last stretch in memory, write it out on demand
vshot replay start monitor --background         # run detached; keeps the last 30s by default
vshot replay save                               # write that stretch to an MP4
vshot replay save /tmp/clip.mp4 --seconds 10    # only the last 10 seconds
vshot replay status                             # how much history it holds
vshot replay stop                               # end the session
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

A finished recognition raises a **desktop notification** by default: the text it read (cut at 160 characters), or why it failed — when `vshot ocr` is started from a keybinding, nothing else says it is done. The notification is handed to whatever owns `org.freedesktop.Notifications` on the session bus, and **a session with no notification daemon still works**: it just misses the note (a line on stderr says so, and the text arrives all the same). The switch is on the settings window's *Text recognition* page, or in the file:

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
vshot record region --geometry '0,0 800x600'    # a rectangle (desktop coords, like `vshot region --geometry`)
vshot record region                             # drag the rectangle on the frozen desktop
vshot record window                             # one window's own pixels (not the area it covers)
vshot record window firefox                     # by app id or title
vshot record window --pick                      # click the window to record
vshot record monitor --encoder hevc             # codec: h264 (default) / hevc / av1
vshot record monitor --fps 120                  # aim for 120 fps (1-240, default 60)
vshot record monitor --duration 60              # stop by itself after 60 seconds
vshot record monitor --portal                   # through the desktop portal: its picker chooses
vshot record window --portal                    # the same, with windows in the picker
vshot record monitor --mic                      # record the microphone into the same MP4
vshot record monitor --mic alsa_input.pci-0000_2f_00.4.analog-stereo
vshot record monitor --no-mic                   # refuse the microphone the config remembers
vshot record window --app-audio                 # record that window's own sound (may combine with --mic)
vshot record window --mic --app-audio           # microphone + the window's own sound, summed into one track
vshot record window --follow GameA --follow GameB   # record whichever of the two has the focus
vshot record window --no-follow                 # do not follow, though the config remembers windows
vshot record monitor --encoder-backend nvenc    # NVIDIA encode (default auto: VAAPI, then NVENC)
vshot record mics                               # list this session's audio inputs
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

- **Region recording (`record region`).** Records a rectangle of one output,
  in desktop logical coordinates in the same form `vshot region --geometry`
  takes (`x,y widthxheight`), and it has to sit inside a single output (no
  compositor call copies a region spanning two; crossing one is an explicit
  error). Without `--geometry` the rectangle is dragged out on the frozen
  desktop through the same picker `vshot region` uses — the microphone opens
  and the file is created only after a rectangle is confirmed, so a cancelled
  pick leaves nothing behind. On a wlroots session the compositor renders just
  that rectangle into a dma-buf, zero-copy into the encoder exactly like
  `record monitor` (measured: 800×600 at a steady 241 frames over 2 seconds at
  120 fps); the encoded size is the rectangle's logical size times that
  output's scale (400×300 on a 2x screen records as 800×600). The recording
  follows the content inside the rectangle, but the rectangle itself stays
  where it was drawn. `--portal` and `region` are mutually exclusive: the
  portal's picker offers whole screens and whole windows, never a rectangle.
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
- **Resizing a window mid-recording.** A window *resized* while recording does
  not end the recording: the compositor re-sends its buffer constraints at the
  new size, vshot rebuilds the capture pool for them, and the new frames are
  fitted into the recording's own canvas — scaled down when larger, centred at
  their own size when smaller, letterboxed — so one MP4 keeps one frame size
  from its first packet to its trailer, and the file is the window's whole
  history. On the VAAPI (dma-buf) route the fit is a GPU `scale_vaapi` filter
  chain; on NVENC's software route the CPU does the same scaling, which is the
  compatible choice that route already is. A window that is *closed* ends the
  recording there: the file is finished properly (trailer written) and stderr
  says why. If the window's output is off, disabled or disconnected, no frame
  ever arrives; that is reported after a few seconds rather than waited on
  forever. Measured: a floating window dragged to 500×400, 900×700 and then
  320×240 while recording was fitted into the canvas it opened with each time —
  the file's size never changed, frames and duration matched, and the
  recording never broke.

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
- **The encoder backend (`--encoder-backend`).** One of `auto` (the default),
  `vaapi`, `vulkan` or `nvenc`. `auto` goes zero-copy first — VAAPI (AMD/Intel),
  then Vulkan, then NVENC — and a fixed choice uses only that one and reports the
  layer that failed (for example `no CUDA device for NVENC`). Each route sets its
  own encoder name, pixel format and private options, and `--encoder
  h264/hevc/av1` applies to all of them. **Every route encodes in hardware**, on
  the GPU's own media engine (`h264/hevc/av1_vaapi`, `_vulkan` or `_nvenc`);
  vshot has no CPU encoder (no x264/x265). What differs is how a frame reaches
  the encoder: VAAPI and Vulkan import the compositor's dma-buf, so nothing is
  copied; **NVENC has no dma-buf import**, so its frames are carried through the
  CPU (read back to memory, converted to NV12, uploaded) — more CPU than the
  zero-copy routes, with the encode itself still on the GPU. **On NVIDIA,
  zero-copy means `vulkan`**: there `*_vulkan` drives the same NVENC hardware
  unit, while `*_nvenc` can never import a dma-buf (ffmpeg's CUDA hwcontext maps
  CUDA memory only, with no DRM PRIME import). On a multi-GPU machine NVENC uses
  the first CUDA device (`VSHOT_NVENC_DEVICE=<index>` picks another) and Vulkan
  the first physical device (`VSHOT_VULKAN_DEVICE=<index>`). An NVIDIA machine
  also needs an `ffmpeg` built with `nvenc` (most distributions ship one).
- **What the Vulkan route trades.** The letterbox is composed with
  `overlay_vulkan` — there is **no `pad_vulkan` filter**, so there is no pad
  chain to fall back to the way VAAPI has one; a chain that will not configure
  reports the failure instead of recording a wrongly composed frame. When a
  window is resized mid-recording, the encoder is drained before the chain is
  rebuilt: it still holds frames from the old chain's pools, and freeing those
  pools under it is a use-after-free (measured as a segfault inside
  `avcodec_send_frame`, reproducible with a single resize).
- **The microphone (`--mic`).** Records an input into the same MP4, encoded as AAC
  (ffmpeg's own encoder, the same libavcodec route the video takes). A bare
  `--mic` takes the session's default source; a name or node serial records
  another input (`wpctl status` lists them — a serial like `--mic 55` is that
  monitor). The microphone is opened before the video encoder, because its rate
  and channel count are declared in the MP4's header, and its samples are
  drained once per video frame, so the two tracks share one clock; the file
  ends up with one video stream and one AAC stream. `--no-mic` refuses the
  microphone even when the config's `cli.record.mic` remembers one; neither
  flag means the config's answer (silent by default). It goes through
  PipeWire, so like `--portal` it needs libpipewire. Measured (48 kHz stereo):
  recording this machine's sink monitor through it gave the same level as a
  reference `pw-cat` capture (-24.1 dB against -24.3 dB). A session with no
  default input says so and names `wpctl status` instead of quoting PipeWire's
  bare "no target node available".
- **Per-application audio (`--app-audio`).** Additionally records the sound the
  recorded window is *playing itself* — nothing another application is playing
  gets in. It only means something on `vshot record window` /
  `vshot replay start window` (other targets have no window to attach it to and
  are refused). It can be used **on its own** (that window's sound and nothing
  else) or **together with `--mic`**: the microphone is the room, the
  application audio is the window, and the two are **summed sample-for-sample**
  in Rust into the MP4's **one** audio track — not two tracks most players
  would play only the first of. The window's pid comes from the compositor —
  Hyprland, niri and (since the probe grew a `pid` field) KWin all report one,
  so this works on Plasma too; vshot takes that pid to PipeWire's client table,
  finds that process's playback node and connects only to it. Measured
  (Hyprland): two mpv players at 880 Hz and 220 Hz produced a
  `--app-audio`-only file whose dominant frequency is 880 Hz — the isolation
  holds. A session whose compositor reports no pid says so rather than quietly
  falling back to the microphone. A window playing nothing keeps the source it
  had (a recording just starting goes video-only), not an error. `--portal` and
  `--app-audio` conflict (the portal's compositor decides which window, and
  vshot has no pid mapping for it). The audio track is AAC, sharing the encode
  and mux with the microphone route.
- **Following the focus (`--follow`).** Give the windows to follow (`--follow
  NAME`, repeated) and the recording moves between them as the focus does —
  whichever of them has the focus is the one being recorded, and while the
  focus is anywhere else the recording **stays on the last one**: no gap, no
  interruption, and the other window's pixels never appear. `--follow` is
  mutually exclusive with a window `NAME` and with `--pick` (it is itself a
  way of choosing windows), and only `record window` and `replay start window`
  can follow. A **bare `record window` that gives no `--follow` at all**
  follows the windows `cli.record.follow` remembers (`cli.replay.follow` for
  the replay side), and `--no-follow` turns a remembered list off for one
  recording — as `--no-mic` does a remembered microphone. A switch is the **same path a resize takes**: the new window's
  pixels are fitted into the canvas the file was opened with, so one MP4 keeps
  one frame size and the timeline is continuous. With `--app-audio` the
  application stream follows too, to the new window's own application — while
  the microphone beside it **keeps running**; a switch replaces only the
  application half (both are 48 kHz stereo — the format does not change, so the
  audio stream declared in the MP4's header stays valid; a new window whose
  application is silent keeps the previous one rather than falling back to the
  microphone). The focus is asked for at most every 250 ms, and
  only when `--follow` was given. Measured (Hyprland): A (900x500, 880 Hz) to
  B (600x340, 220 Hz) and back to A over a 14-second recording produced a
  14.0 s file, 900 wide throughout, whose audio analysed by time window
  (Goertzel) is 880 Hz, then 220 Hz, then 880 Hz again. A compositor that
  cannot report the focus is **refused plainly** rather than never switching
  (`--follow` is a request for the focus by definition). Note that it follows
  *the focus*, not "the game being played": switching to a window outside the
  list leaves the recording on the last one, but what you did in that moment
  is not in the file.
- **Width limit 4096.** That is the hardware H.264 encoder's limit (measured
  on this machine's 7900 XT VCN), so `record all` across two 4K screens
  (5760 wide) is refused with that explanation — record one output instead, or
  switch to `--encoder hevc`, which encodes the 7680-wide desktop here. A
  single 4K screen (3840) is fine.
- **Remembered defaults.** When `--encoder`, `--encoder-backend`, `--fps`, `--portal`,
  `--mic` or `--follow` is not given, the value comes from the config file's
  `cli.record` section (`cli.replay` for the replay side; see
  [`cli` — command-line defaults](#cli--command-line-defaults)). The settings
  window's Recording page (its Recording and Replay cards) writes all of those
  keys. `--no-portal`,
  `--no-mic` and `--no-follow` turn a remembered value off for one recording. A
  remembered `follow` list applies only to a bare `record window` (no NAME, no
  `--pick`); every other target ignores it, so a remembered list does not put
  `record monitor` out of reach. `vshot record mics` lists the
  audio inputs this session has, one per line as a serial, a node name and a
  description, tab-separated; the node name is what `--mic` takes, and that
  listing is what the settings window's microphone row offers. A session with no
  inputs says so rather than failing. (`--app-audio` is command-line only and is
  not a config key — it means "this window's sound", which a different window
  would change.)

- **`--portal` records through the desktop portal**
  (`org.freedesktop.portal.ScreenCast`). A compositor's own capture protocols
  — wlr-screencopy, KWin's ScreenShot2, `ext_image_copy_capture_v1` — each
  exist on some desktops and not others; the portal is the one every desktop
  has, and the price is that **the compositor decides what is recorded**: it
  shows its own picker, and whatever is chosen there is what gets recorded.
  `record monitor --portal` asks it to offer screens, `record window --portal`
  to offer windows, but a name or `--pick` cannot decide which one — every
  session shows the picker again. One stream per session, so
  `record all --portal` is refused. A screen cast produces a frame when the
  screen changes and none while it does not, so a still screen becomes one
  long frame and `--fps` is the rate asked of the compositor rather than the
  rate the file has. Frames that arrive as dma-bufs go to the encoder without
  a copy; otherwise they are converted to RGBA on the CPU.
  `VSHOT_PORTAL_SHM=1` forces the memory path, which is the fallback for a
  compositor whose buffers the encoder cannot import. Needs
  `xdg-desktop-portal` and `libpipewire`: its headers to build against, its
  library `dlopen`ed at run time, so a machine without it simply has no
  `--portal`.

> Why not libva directly? We tried; the first implementation was exactly
> that. On this machine (mesa-git 26.3.0-devel, radeonsi) the encoder
> dereferences a null pointer inside `vaEndPicture` (`mov 0xb0(%rdi),%rax`
> with rdi = NULL) in about half of all runs, independently of the call
> shape — reused or per-frame coded buffers, `vaSyncSurface` or
> `vaSyncBuffer`, one thread or many all segfaulted alike. libavcodec's
> `h264_vaapi` runs the same hardware for hundreds of frames without a
> fault on the same machine, so the encoder boundary belongs to libavcodec.
> That is an engineering decision, not an aesthetic one.

## Replay

`vshot replay` is a recording that keeps its last stretch of history in memory
instead of on disk. The screen is encoded continuously and the packets go into
an in-memory ring; `vshot replay save` copies what the ring holds into an MP4 —
a stream copy, no re-encode — so the trigger costs almost nothing and nothing
is written to disk until it is asked for. Leave it running while you game, hit
a key, and the last stretch is on disk.

```sh
vshot replay start monitor --background     # run detached; keeps the last 30s by default
vshot replay save                           # write to the videos directory, timestamped
vshot replay save /tmp/clip.mp4 --seconds 10
vshot replay status                         # how many seconds it holds, how many saves served
vshot replay stop
```

`start` takes the same targets `record` does: `monitor [NAME]` (a bare `replay
monitor` means the output you are on), `all`, `region` (`--geometry`, or dragged
out on the frozen desktop), `window` (the focused one, a name, or `--pick`). The
frames come from the same capture backends and go to the same libavcodec GPU
encoder, and the zero-copy dma-buf path is used wherever `record` uses it.

### Why encode continuously, not on trigger

A replay has to be able to reach back through the last stretch — frames that
existed before you pressed the key. Keeping them as raw frames is what the
numbers rule out: 4K60 NV12 is ~12 MB a frame, so 30 seconds is ~22 GB, more
than any GPU or system memory wants to spare. The same 30 seconds as encoded
packets at 30 Mbps is ~110 MB. So a replay must encode continuously; that is the
physical price of being able to look back, and it cannot be avoided. What can be
optimized is the encoding itself: hardware, a lower rate when asked, a bounded
GOP.

### The ring and key-frame alignment

- **Bounded GOP.** The encoder runs with a `--gop`-second key-frame distance
  (default 1) instead of the recording's all-intra stream. The ring is therefore
  a fraction of an all-intra stream, and every GOP boundary is a place a save
  can start from.
- **The ring keeps `--window` + one GOP.** A save starts at the newest key frame
  at or before `now - seconds`; that key frame can sit up to a GOP before the
  edge, so the ring holds that extra interval — otherwise it would evict exactly
  the key frame the save needs.
- **A save starts on a key frame.** An MP4 whose first video packet is not a key
  frame shows nothing until the next one. So the start is key-frame aligned: the
  file holds at least the seconds asked for and decodes from its first byte.
  Asking for more than the window holds gives everything there is.

### Options

| Option | Meaning |
| --- | --- |
| `--window N` | Seconds of history to keep (1–3600, default 30); the config's `cli.replay.window` when the flag is not given |
| `--fps N` | A replay defaults to **30** (a recording to 60): it is left running for long stretches, and 30 fps halves the encoder's work |
| `--gop N` | Key-frame distance in seconds (1–10, default 1). Smaller starts a save closer to the requested edge, at the cost of a bigger ring |
| `--encoder` | h264 (default), hevc or av1, as for `record` |
| `--encoder-backend` | `auto` (default), `vaapi` or `nvenc`, as for `record` |
| `--mic [DEVICE]` | Keep the microphone in the ring too, as `record --mic` does; `--no-mic` refuses it |
| `--app-audio` | Additionally keep the recorded window's own sound (`replay start window`), as `record --app-audio`; may combine with `--mic`, summed into one track |
| `--follow NAME` | Switch source as the focus moves between these windows (`replay start window`), as `record --follow`; a bare `replay start window` follows `cli.replay.follow` |
| `--no-follow` | Do not follow the focus, even when `cli.replay.follow` remembers windows |
| `--save-dir DIR` | Where a `replay save` with no path lands (strftime-expanded; the videos directory, or `cli.replay.save-dir`) |
| `--background` | `replay start` only: detach the session from the terminal so it outlives the shell |

### Keybindings

Control goes through `$XDG_RUNTIME_DIR/vshot-replay-<uid>.sock`, and
`save`/`status`/`stop` need no display, so they bind directly:

```
bind = SUPER, R, exec, vshot replay start monitor --background
bind = SUPER SHIFT, R, exec, vshot replay save
bind = SUPER ALT, R, exec, vshot replay stop
```

A `save` raises a desktop notification saying where the file went and how long
it is.

### Configuration

The config file's `cli.replay` section supplies the defaults (the settings
window's **Replay** card writes the same keys, or edit the file by hand):

```jsonc
"cli": {
  "replay": {
    "window": 30,            // seconds of history
    "fps": 30,
    "encoder": "h264",       // h264 / hevc / av1
    "encoder-backend": null, // auto / vaapi / nvenc; null means auto
    "gop": 1,                // key-frame distance in seconds (1-10)
    "mic": null,             // "" is the default input, a name is a PipeWire node
    "follow": null,          // windows to follow, e.g. ["game", "chat"]; null means none
    "portal": false,         // replay does not support the portal yet
    "save-dir": null,        // null uses the videos directory
    "notify": true           // whether a save raises a notification
  }
}
```

A flag always wins over the file. A value the file gets wrong (an unknown
encoder name, a rate out of range) falls back to the built-in default rather
than failing the whole replay. One session runs at a time (the pid file refuses
a second); `VSHOT_REPLAY_SOCKET` overrides the control socket and
`VSHOT_REPLAY_PIDFILE` the pid file `replay stop` reads.

### What replay does not do yet

- **Portal.** The portal's own frame loop is not wired to the ring;
  `replay start --portal` refuses with a sentence telling you to record through
  the portal instead.
- **Window replay's idle semantics.** `window` goes through
  `ext_image_copy_capture_v1`, whose compositor holds the copy until the
  window's content changes (the same as `record window`). A still window can
  therefore have very few frames in the ring for a long while; a window that
  changes is fine. This is the protocol's on-demand copying, not a replay
  defect.

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
| **Recording/replaying a window** (`record window`, `replay start window`) | `ext_image_copy_capture_v1` (the window's own dma-buf) | **the compositor's own screen-cast service** (`org.gnome.Mutter.ScreenCast`, captured by window id, PipeWire stream) | **KWin `ScreenShot2.CaptureWindow`** (captured by the window's `QUuid`, CPU pixels) | ❌ none | ❌ none |
| Following the focus (`--follow`) | ✅ `hyprctl activewindow` | ⚠️ the query exists (`niri msg focused-window`), but a window recording through the screen-cast service cannot be retargeted | ✅ scripting probe / `kdotool` | ⚠️ the query exists, but window recording itself is unsupported | ❌ none |
| Window pid for `--app-audio` | `hyprctl clients -j` | ✅ `niri msg windows` reports one | ✅ the scripting probe's `pid` field | ❌ none | ❌ none |
| Which screen a pin lands on | `hyprctl cursorpos` + `monitors -j` | `focused-output` (keyboard focus only) | `org.kde.KWin.activeOutputName` | `swaymsg -t get_outputs` | ❌ none |
| Scroll injection | wlr virtual pointer | wlr virtual pointer | portal / uinput | wlr virtual pointer | uinput |

Where nothing is adapted, vshot **degrades automatically instead of erroring**: with no window list it falls back to pixel detection, and when it cannot ask which screen the pointer is on it falls back to the primary screen Qt reports.

### How far verification goes

- **Hyprland** — this machine's session is Hyprland and it is the main development and verification environment: capture, selection and annotation, windows, long screenshots, pins, and scroll injection have all run here.
- **Recording encoder backends** — this machine (7900 XT) is VAAPI: zero-copy dma-buf, h264/hevc/av1, `--fps`, mid-recording window resizes, the portal and replay have all been measured on that route. **The Vulkan route has been measured on this machine too** (same card, `--encoder-backend vulkan`): `record monitor` at 4K (3840×2160) holds 60fps with all three codecs and copies nothing (the shim's `frame (dmabuf)` trace shows map and add at 0.0ms); `record window` resized mid-recording rebuilt the chain 24 times and still produced a full 12-second file, with the letterbox geometry checked on extracted frames (a 2400×1800 window fitted into a 1000×800 canvas came out as 999×748 centred with 26px above and below, matching the aspect-preserving fit, and the restored size filled the canvas again with no stale pixels); the `replay start window` path works on it; and a deliberately wrong `VSHOT_VULKAN_DEVICE` fails cleanly and names Vulkan, which is the proof it did not fall back to VAAPI. **But no NVIDIA hardware has ever run it** (the development machine is AMD): `*_vulkan` drives the same NVENC unit there and the code path is identical, so what is untested is the driver, not the route. NVENC's **routing and failure path** are verified (`--encoder-backend nvenc` fails cleanly on a machine with no NVIDIA, with the specific reason, e.g. `no CUDA device for NVENC`), but **a real NVENC encode has never run on NVIDIA hardware** — the CPU-carried route (dma-buf readback, NV12 conversion, upload — the encode itself is still the NVENC hardware) and its window-fit code are written from code review with no live data.
- **Per-application audio** — measured on Hyprland: two mpv players at 880 Hz and 220 Hz produced a `record window --app-audio` file whose dominant frequency is 880 Hz, so the isolation holds; a window playing nothing degrades to a video-only recording. The window pid has a source on every compositor where a window can be recorded at all (Hyprland and niri report it directly, KWin through the scripting probe's `pid` field) — so `--app-audio` works on Plasma too, and niri reports one as well, which a window recording through its screen-cast service uses the same way (no isolation measured live) — and Sway and labwc have no pid source and refuse plainly instead of quietly falling back to the microphone. The niri and KWin pid paths have unit tests and headless live runs but no isolation measurement against a real audio session.
- **Microphone + application audio together** — measured on Hyprland: a virtual microphone source at 440 Hz and an mpv window at 880 Hz produced, from `record window --mic --app-audio`, **one** AAC track in which Goertzel analysis finds both 440 Hz and 880 Hz across the whole length, matching the video (8.000 s against 8.000 s). On a `--follow` switch the microphone runs **unbroken throughout** (440 Hz at a steady 0.212 amplitude every second) while only the application stream moves to the new window (880 Hz to 660 Hz). The replay path (`replay start window --mic --app-audio`) carries both too.
- **Focus following (`--follow`)** — measured on Hyprland with two mpv windows at different sizes and tones: switching focus A -> B -> A produced one 14.0 s file, 900 wide throughout (B's 600x340 fitted into A's canvas), whose audio is 880 Hz, then 220 Hz, then 880 Hz by time window. The focus query, the whitelist match and the "stay put" cases have unit tests; a compositor that cannot report its focus is untested here (Hyprland can).
- **niri** — both the tiled and floating **screenshot** paths were verified on a real session: tiled windows were measured with two side-by-side kitty windows (residual 0.45/0.51 per channel), and floating windows go through niri's `tile_pos_in_workspace_view` coordinates plus `matches_at_position` verification, with correct results on the real session. If a floating window capture comes out misaligned, `--no-blend` bypasses the locating step. niri's focus query (`niri msg --json focused-window`) and its window list are wired into the shared window table, which feeds the picker and the pixel detector's candidates. **A window recording and a window replay go through the compositor's own screen-cast service** (`org.gnome.Mutter.ScreenCast`), not the wlroots protocol: measured against niri 26.04 (`v26.04-347-gcd434f86`), its Wayland globals now offer `ext_image_copy_capture_manager_v1`, `ext_output_image_capture_source_manager_v1` and `ext_foreign_toplevel_list_v1`, but **not** `ext_foreign_toplevel_image_capture_source_manager_v1` (the capture source for a window), so the wlroots route still fails at the connect step with a message saying so (25.11 lacked even the first three). niri implements GNOME's screen-cast interface itself (`src/dbus/mutter_screen_cast.rs`, the `xdp-gnome-screencast` feature is on by default): `Session.RecordWindow(window-id)` creates the stream, `Stream.PipeWireStreamAdded` names the PipeWire node, and vshot connects to the session's PipeWire daemon to read the frames — **no portal, no picker, no permission prompt**, and because vshot talks to the D-Bus name directly it is unaffected by environments where several compositors share one session bus and xdg-desktop-portal is bound to another backend. The window id comes from `ext_foreign_toplevel_handle_v1`'s identifier: niri encodes it as the window id in decimal (`MappedId::to_protocol_identifier`, documented as reversible precisely so a client can match a toplevel handle to an IPC window id), so a window named on the command line or picked with `--pick` maps straight onto the id the service takes. The service is only registered in a **session instance** (a display manager or `niri-session`); an instance started by hand from a TTY needs `debug { dbus-interfaces-in-non-session-instances }` for it to appear at all (the check is niri's `src/dbus/mod.rs:72`). `--follow` is not available on that route: the service casts the window it was started on. One correction to an earlier claim: niri's **portal does implement ScreenCast** — `niri-portals.conf` leaves it out because `default=gnome;gtk;` already covers it, the provider is the gnome portal, and niri's own documentation lists `xdg-desktop-portal-gnome` as required for screencasting. **Measured on this machine's niri 26.04 (`v26.04-347-gcd434f86`)**: `record window` produced a correct file for a 4K window (3792×2024, dma-buf zero-copy, AV1/VAAPI); a window resized mid-recording (940×2022 to 1882×2038) had its fit chain rebuilt and the recording ran to its full length, with the letterbox geometry verified on an extracted frame (content 1018 tall, 502 from the top, matching a 0.4995 scale centred); closing the window **ended the recording and said why**; and `replay start window`'s ring, `status`, `save` and `stop` all worked. On an active session, **240 frames in 5 seconds (48 fps, limited by the window's own repaint rate)** at ~0.2 ms of muxing per frame. **One throttling caveat on niri's side**: it draws the cast inside its **output render loop** (`render_windows_for_screen_cast` at `niri.rs:4861`), so an inactive session (`Active=no` in `loginctl`) barely renders at all — measured at about 1.3 fps. For a normal frame rate, niri's VT has to be the active one.
- **KWin/Plasma** — D-Bus capture (the size and opacity of `CaptureScreen`, `native-resolution`, format fields), the window list, and long screenshots have all been tested, where the capture and window list automation ran against a **headless `--virtual` KWin**. On top of that, **window recording and replay (`record window`, `replay start window`) were verified live against a headless `--virtual` KWin 6.7.5 on this machine**:
  - the route is `org.kde.KWin.ScreenShot2.CaptureWindow`, capturing the window's own pixels by its `QUuid` (the `internalId` the scripting probe reports), decorations included;
  - this interface is one of KWin's **restricted** ones, and the authorization is looked up **per call**: KWin resolves the caller's pid to its executable path and then hunts the desktop-file database for the `.desktop` whose `Exec=` names it and which declares `X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2`. That hunt goes through KService/ksycoca, and while ksycoca is being rewritten it comes back empty — so a client that was authorized a moment ago is **refused transiently**. Measured: on this machine's Plasma session `~/.cache/ksycoca6_*` was rewritten about 2.5 times a second with **no vshot process running at all**, and a 42-second window recording died outright on ten refusals in a row because of it. Such a refusal is therefore **no longer counted as a dropped frame**: vshot retries at the frame rate for up to ten seconds, logging at most one line a second (the full explanation once), keeps recording if the authorization comes back, and gives up after ten seconds carrying KWin's own explanation. Both directions were verified live: a refusal that lasted about 5.5 seconds ended and the recording carried on and finished properly (a 90-second recording was unaffected), and a refusal that outlasted ten seconds stopped the session with the reason spelled out. This applies to every KWin recording and replay loop, window and screen alike;
  - the scripting probe was extended to eight tab-separated fields (`x y width height pid class title handle`), so KDE now **has a window pid** — `--app-audio` is no longer refused on Plasma, and finds the application audio by pid as everywhere else;
  - the window capture **turns the shadow off** (`include-shadow=false`): a recording is NV12 with no alpha, so a shadow's transparent surround would turn into a black band and grow the canvas (measured: a 941x768 window came back as 1072x898 with its shadow);
  - the canvas is **rounded up to even**: `ScreenShot2` reports the window's client geometry, which can be odd, and NV12 has no odd form (an odd size asserts inside libavutil's converter), so the recording/replay canvas is even and the fit path pads the extra column and row. Measured: a 941x768 window records to a 942x768 h264 file;
  - `--follow` was verified: moving the focus between two windows switched the recording, one file per session, continuous timeline;
  - replay (`replay start window`) was verified: the ring, a save, and status queries all work;
  - **still unverified**: `--cursor` (a headless output has no pointer to draw), the full `--pick` interaction on a window click, and scroll injection (below).
  - **Long screenshots are verified working**. KWin's `CaptureArea` is private API (argument order unchecked), so on KDE vshot captures the whole screen and crops, which costs noticeably more per frame than on other compositors;
  - **Scroll injection is unverified**. `portal` goes through KWin's own EIS server, but this machine's Hyprland portal does not implement RemoteDesktop, so **that path has never run on real hardware** and needs acceptance on KDE; `uinput` needs write access to `/dev/uinput`.
- **Sway** — probe implementations and unit tests only, **no live verification**.
- **labwc and other compositors providing wlr-screencopy** — basic capture should work in theory, but there is no window list and no pin-landing probe, and there is **no live verification**. Only the operations that read pixels (`region --geometry`, `monitor <name>`, `all`) are usable; `monitor current` and interactive selection need a pointer/keyboard seat, which usually works too.
- **GNOME** — unsupported, with no plan to implement it. Mutter provides neither wlr-screencopy nor KWin's D-Bus service, and not even layer-shell.

### The cursor (`--cursor`)

vshot never draws a cursor itself; `--cursor` only sets an "overlay the pointer" flag on the compositor's capture request (`wlr-screencopy`'s `overlay_cursor`, KWin's `include-cursor`), and whether, where, and when it is drawn is entirely up to the compositor. Three conclusions follow from measurement:

- **A capture with no cursor after typing a command in a terminal is not a bug.** A terminal (kitty measured, `mouse_hide_wait` defaults to 3.0 seconds) hides the pointer after a few seconds without mouse movement — it performs `set_cursor(null)` on the compositor, after which there **really is no cursor to draw** at the compositor level, and any tool going through screencopy sees the same thing (`grim -c` cannot grab it at that moment either). Move the mouse before capturing after pressing Enter, or set `mouse_hide_wait 0` for kitty (the pointer then no longer hides itself while typing). The same "hide the pointer while typing" behavior in other terminals and programs behaves identically.
- **On niri the pointer is drawn into the window capture, not the output frame.** On niri, `window active` / `window pick` go through niri's own `screenshot-window`, and `--cursor` maps to that call's `--show-pointer`, so the cursor lands in the window image; `--show-pointer` only exists after 25.11, and an older niri rejects the whole request, which vshot detects and retries without the argument, saying "the pointer cannot be drawn into the window". The output-level paths (`monitor`, `all`, `region`) still use screencopy's `overlay_cursor`.
- **On Hyprland running `hypr-dynamic-cursors` the pointer gets baked into the frame, and vshot steps around it.** While the cursor is magnified — shake to find, or the plugin's own magnify dispatcher — the plugin holds the compositor's software-cursor lock, and the compositor then draws the pointer into the frame it composites, which is the very frame screencopy hands out — so **the pointer is there without `--cursor` too**. Once the magnification ends, the pixels it left behind stay on screen (a `grim` grab at the same moment shows exactly the same residue, so this is compositor-side behavior rather than something vshot causes). Before reading a whole scene, vshot switches the plugin off and warps the pointer to where it already was — that pointer event is what makes the compositor fall back to its hardware cursor and repaint the rectangle the software cursor had occupied — then switches the plugin back on as soon as the frames are read. A session without the plugin, or with it switched off, is left alone entirely. Captures therefore carry no leftover cursor, and the residue on screen is cleared along the way.

`--cursor` has no effect on `long` (see the next entry); whether KWin really draws a cursor is unverified.

### Known rough edges

- **`--cursor` does nothing on `long`** — the frame grabbing in `src/longshot.rs` hardcodes the cursor argument to `false` (and `longshot::run` has no such parameter), so `vshot long --cursor` is accepted and silently ignored. On the other capture paths (`region`, `monitor`, `all`, `window active`) `--cursor` works as measured on this machine's Hyprland. On KWin `include-cursor` is passed, but **whether a cursor is really drawn is unverified**.
- **A capture taken mid-magnification can still carry a pointer** — `hypr-dynamic-cursors` holds the software-cursor lock while the cursor is magnified, and switching the plugin off does not release it immediately, so a frame grabbed in that instant keeps a **normal-sized** pointer in it (the magnified one never makes it in). The plugin releases the lock when the magnification ends, so the problem heals itself: any capture taken after that is clean.
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

The window is an ordinary window: it captures nothing and needs no compositor protocol, so it also works on a compositor vshot cannot otherwise capture. After installing the package you can also open it from the application menu as **VShot Settings** (see ["Application menu entry"](#application-menu-entry)). An empty or zero value in the `cli` section means "leave it unset, use the built-in default" rather than storing a zero, and **the settings window shows those defaults directly** — the spin boxes open on the real numbers (`60`, `30000`) and each combo box's first entry reads `fast (built-in default)` — so the table above is not something you have to consult to find out what will happen. A default left where it is still stays out of the file (the test is "the value is still the built-in one"), so if a later version changes that default, whoever never touched it follows along; the `editor` section is always written whole. Saving **merges**: keys this build does not recognize — a newer vshot's, or your own — survive untouched instead of being wiped by a save.

The window has seven pages, switched from the sidebar, **one feature per page**: **Annotation editor** (tool, color, width, line style, arrow, text, mosaic), **Output** (PNG compression, default monitor), **Scrolling capture** (scroll notches, the height/frame ceilings, timeout, ignore-top, scroll backend), **Text recognition** (the notification a finished recognition raises), **Recording** (every default for `record` and `replay`: codec, hardware backend, frame rate, portal, microphone, follow list and notification switch, plus the replay's history window, key-frame distance and save directory), **File dialogs** (corner radius, border width and colour, and the shadow) and **Pin appearance** (its size, i.e. the pin density, then corner radius, the shadow, border width, and the two border colours). The command-line defaults used to share one page, so reaching a recording option meant scrolling past three unrelated cards; they are split by feature now. Each page is a column of cards, one setting per row with the label on the left and the control on the right, and every page but **Recording** fits without a scrollbar at the default window size — the recording page carries both the `record` and the `replay` defaults, fourteen rows, some two hundred pixels more than the viewport, which is the trade this split accepted (splitting them further would put the replay's defaults a page away from the recording's). The combo and spin boxes paint their own chevrons — the native ones are beveled triangles from a different decade — so the controls match the toolbar's look. Saving **does not close the window**: a "Saved." note appears in the corner, so a value can be changed, saved, looked at and changed again without reopening anything. Cancel is now only "close".

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
| `record.encoder` | `record --encoder` | `h264` |
| `record.encoder-backend` | `record --encoder-backend` | `auto` |
| `record.fps` | `record --fps` | `60` |
| `record.portal` | `record --portal` | `false` |
| `record.mic` | `record --mic` | none |
| `record.follow` | the windows a bare `record window` follows (an array) | none |
| `record.notify` | a desktop notification when a recording is written | `true` |
| `replay.window` | `replay --window` | `30` |
| `replay.fps` | `replay --fps` | `30` |
| `replay.encoder` | `replay --encoder` | `h264` |
| `replay.encoder-backend` | `replay --encoder-backend` | `auto` |
| `replay.gop` | `replay --gop` | `1` |
| `replay.mic` | `replay --mic` | none |
| `replay.follow` | the windows a bare `replay start window` follows (an array) | none |
| `replay.portal` | `replay --portal` | `false` |
| `replay.save-dir` | `replay --save-dir` | the videos directory |
| `replay.notify` | whether `replay save` raises a notification | `true` |
| `ocr.engine` | which engine `vshot ocr` uses | `builtin` |
| `ocr.external.command` | the program to run when `engine` is `"external"` (an array) | none |
| `ocr.external.stdin` | send the PNG on stdin instead of passing a path | `false` |
| `ocr.external.timeout` | the external program's timeout, in seconds | `30` |
| `ocr.notify` | a desktop notification when recognition ends | `true` |

`pin.density` follows the same order: `--density` > `VSHOT_PIN_DENSITY` > the config file. Unknown keys inside `cli` are ignored rather than making the whole file invalid — a misspelled key costs you that one setting, and the rest still apply.

`ocr.engine` accepts only `builtin` and `external`; any other name is an **error** rather than a default, because a misspelled `external` would otherwise look like a working GPU engine. Likewise `engine: "external"` with no `command`, or a command that will not run, is reported plainly (see [Using a GPU](#using-a-gpu-the-external-engine)). The settings window covers `editor`, the common `cli` entries, the `ocr.notify` switch, and both the `record` and `replay` sections (each section's codec, hardware backend, frame rate, portal and microphone; the replay's `window`, `gop` and `save-dir` too; the `follow` list of each; and both `notify` switches); `ocr.engine` and `ocr.external` are edited by hand. The two `notify` switches **only ever write "off"**: an absent key already means on, so writing `true` would say nothing the file did not already say. The microphone rows are filled from the running session, with a button beside each to ask again: a session with no inputs is not an error there, the row simply offers the two answers that always exist. The `follow` rows are **one comma-separated line of window names** in the window and an **array** in the file — hand-edit it as `["game", "chat"]`.

`record.follow` / `replay.follow` apply only to a **bare `record window` / `replay start window`** — no window NAME and no `--pick` — because that is the one place "follow" means anything. Every other target (`monitor`, `all`, `region`, or a window named on the command line) **ignores** a remembered list rather than failing on it, since otherwise a remembered list would make `record monitor` unusable. `--no-follow` turns a remembered list off for one recording, the way `--no-mic` does a remembered microphone.

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
| `VSHOT_NVENC_DEVICE=N` | Use the Nth CUDA device for NVENC (multi-GPU machines; the first by default) |
| `VSHOT_VULKAN_DEVICE=N` | Use the Nth physical device for the Vulkan encoder (multi-GPU machines; the first by default) |
| `VSHOT_OCR_MODELS=<dir>` | OCR model directory, overriding `/usr/share/vshot/models` and the search beside the executable |
| `VSHOT_PIN_SOCKET` | The socket path the pin daemon listens on |
| `VSHOT_PIN_DENSITY=N` | The source density of every pinned image, same as `--density` |
| `VSHOT_PIN_DEBUG=1` | The daemon prints every pin's density decision |
| `VSHOT_PIN_FOCUS_DEBUG=1` | The daemon prints every focus change of every pin render surface |
| `VSHOT_PIN_SOURCE_FILE` | Overrides the screenshot tool's record path (default `/tmp/screenshot-path`) |
| `VSHOT_RECORD_PIDFILE` | The pid file `vshot record stop` reads (default `$XDG_RUNTIME_DIR/vshot-record-<uid>.pid`) |
| `VSHOT_RECORD_DEBUG=1` | The recording/replay loop traces each frame's stage (grab/encode/mux) and the libavcodec version in use |
| `VSHOT_PORTAL_SHM=1` | `record --portal` asks for memory frames instead of dma-bufs (the fallback when a compositor's buffers cannot be imported) |
| `VSHOT_REPLAY_SOCKET` | The replay control socket path (default `$XDG_RUNTIME_DIR/vshot-replay-<uid>.sock`) |
| `VSHOT_REPLAY_PIDFILE` | The pid file `vshot replay stop` reads (default `$XDG_RUNTIME_DIR/vshot-replay-<uid>.pid`) |

> As soon as any `VSHOT_PIN_*_DEBUG` is set for the daemon, it stops sending its stderr to `/dev/null`, so the traces are readable. The variables must be in place when the daemon starts; for one already resident, run `vshot pin --quit` first.

## Known limitations

- **KWin's frozen overlay still depends on `zwlr_layer_shell_v1`** (which KWin provides). Authorization is not a dialog and only recognizes desktop files, so a build run straight from `target/` will always get `NoAuthorized` under Plasma;
- **Several compositors can coexist under one `XDG_RUNTIME_DIR`**, and when `WAYLAND_DISPLAY` is unset libwayland uses the default `wayland-0` (which is the case for a shell in a tty or over ssh), so a command may connect to "the other compositor". For every compositor-related failure vshot additionally prints which display it connected to and which displays exist;
- **Long screenshots**: the selection must lie entirely within one screen; a fixed bar that only appears at some scroll positions (a floating toolbar) is still treated as page content, so exclude it with `--ignore-top N`; a lazily loading page may duplicate or miss a few rows; the frame rate on KDE is noticeably lower than on other compositors (it goes through D-Bus capturing the whole screen and cropping, and that path is not verified live); the time for grabbing and aligning grows linearly with the region's area, so **use a release build for long screenshots**;
- **Pixel detection**: a seamless borderless tiling layout and a completely uniform desktop have no pixel signal, and vshot reports that honestly instead of guessing; when the compositor draws no colored border around the focused window, `window active --pixel` answers with "the window under the pointer";
- **Text**: the Qt text box accepts arbitrary Unicode (including CJK submitted by an input method); a result from an old helper that carries no bitmap falls back to Rust's built-in 5x7 font, which only supports printable ASCII;
- **`monitor current`** depends on receiving pointer enter/motion on the overlay; generic Wayland has no readable global mouse position, so vshot never guesses with the first output;
- **Replay**: `replay start --portal` is not supported yet (the portal's frame loop is not wired to the ring, and it refuses with a sentence saying so); a `window` replay is subject to the protocol's on-demand copying, so a still window can have very few frames in the ring for a long while; one session runs at a time;
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

**GPL-3.0-or-later** (see `LICENSE`): vshot is released under the GNU General Public License, version 3 or any later version; redistributing it, modified or not, requires providing the complete corresponding source under the same terms. Earlier releases (through v0.1.2) were MIT; the move to the GPL removes every gray area in the dependency chain:

- **Qt 6 / LayerShellQt** — dynamically linked and used under the GPL options both provide (Qt also offers LGPL-3.0, LayerShellQt LGPL-2.0-or-later). The Qt libraries can be replaced freely: the `vshot` CLI does not link Qt at all, and the interface lives in the separate `vshot-qt-ui` helper.
- **FFmpeg** (recording, optional dependency) — Arch's build is GPL-3.0 and is loaded with dlopen at run time. Under MIT, whether dlopen makes one combined work was an open question; with vshot itself under the GPL, either answer is compatible.
- **Rust crates** — MIT / Apache-2.0 / BSD and similar permissive licenses, all GPL-compatible.
- **OCR models** (`/usr/share/vshot/models`) — from RapidOCR / PaddleOCR, Apache-2.0; the dictionary from oar-ocr, Apache-2.0.

`LICENSE` carries the full GPLv3 text and `NOTICE` the third-party notices; both are installed to `/usr/share/licenses/vshot/`. For users: use, modification and redistribution stay free; redistributing a modified version now also means shipping its source, and vshot code can no longer be folded into a closed-source product.
