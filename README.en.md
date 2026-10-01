# vshot

[中文](README.md) | **English**

> **This project is pure vibe coding**: requirements come from a human, code and docs are written by AI.

A Wayland screenshot tool written in Rust, with a Qt interactive UI and a resident pin overlay. Captures are **strictly frozen**: the desktop is photographed into a still frame first, and every selection and annotation happens on that frame, so nothing on screen moves while you are choosing. Works with Hyprland, niri, KWin/Plasma, Sway, and basic capture on any compositor providing `wlr-screencopy` (such as labwc).

> **Verification varies**: every Hyprland feature was tested live; niri's tiled and floating window capture, and its **window recording and replay**, were verified on a real session; KWin/Plasma's D-Bus capture, window list, and long screenshots were tested, but `--cursor` and scroll injection were not; Sway and labwc are **untested on a live session**. See ["Compositor support and verification status"](#compositor-support-and-verification-status).

## Features
- **Region capture** — drag on the frozen desktop, or give a fixed geometry; eight resize handles, with a magnifier and size readout following the pointer
- **Annotation editing** — rectangles, ellipses, arrows, freehand, text, mosaic; undo/redo, move and resize a selection, colors and line styles, arrow head styles, font sizes, system font picker
- **monitor / all** — capture one output by name or by pointer position, or compose the whole desktop by logical position
- **window active / pick** — the focused window, or click one on the live desktop; KWin and niri hand over the window's own pixels
- **Long screenshot** — frame a scrolling region, and vshot sends the wheel, grabs frames, aligns them by content, and stitches one long image
- **Screen recording** — one output, the whole desktop, a rectangle or a window's own pixels to an MP4, GPU-encoded (VAAPI / Vulkan / NVENC), with an optional microphone track or the recorded window's own audio
- **Replay** — encode continuously but keep only the last N seconds in memory; a key writes that stretch to an MP4 (a stream copy, no re-encode)
- **Pin overlay** — pin images or clipboard content to the screen: drag, wheel to zoom, double-click to close, one-key show/hide, Space to annotate
- **Clipboard pinning** — colors, images, copied image files, plain text (rendered as a card as HTML / markdown / code / plain text)
- **OCR** — frame a region and get its text back (Chinese, English and Japanese), through `vshot ocr` or the editor toolbar's *Text+* button, which selects the recognized text in place rather than copying all of it; a desktop notification when it finishes
- **Translation** — translate the recognized text into another language through `vshot translate`: interactively the translation is drawn over the original, and the same feature can emit a translated envelope for a program to place at the original positions; nine providers, five of them needing no key of your own (Google / Microsoft / Volcengine / Tencent Transmart / Caiyun Lingocloud, the last one on a borrowed token) and the rest Bing / Baidu / any OpenAI-compatible endpoint / a program of your own
- **Output targets** — file (with strftime paths), stdout, clipboard, or an on-screen pin; exactly one
- **Bilingual UI** — the interface and `--help` follow the system language

## Install (Arch Linux)
`PKGBUILD` builds the Rust CLI and the Qt helper into one package, so a single install provides `/usr/bin/vshot`, `/usr/bin/vshot-qt-ui`, the `/usr/share/applications/vshot.desktop` that authorizes KWin, and an icon-bearing launcher entry `/usr/share/applications/vshot-settings.desktop` (see ["Application menu entry"](#application-menu-entry)):

```sh
./scripts/build-arch-package.sh
sudo pacman -U dist/vshot-0.1.2-1-x86_64.pkg.tar.zst
```

The script snapshots the current working tree into a temporary directory and runs `makepkg`, writing the result to `dist/` (`makepkg -si` works directly too). Runtime dependencies are `glibc`, `wayland` (uses `libwayland-client` through dlopen), `qt6-base`, `layer-shell-qt`, and **`onnxruntime`** (the OCR inference engine); file output, `--clipboard`, and `vshot pin --clipboard` need the optional `wl-clipboard` (writes via `wl-copy`, reads via `wl-paste`). The package also installs about 30 MB of OCR models under `/usr/share/vshot/models/`, which `makepkg` fetches and SHA-256-verifies; other distributions build from source as below. The models are cached in `$XDG_CACHE_HOME/vshot/makepkg-sources` (usually `~/.cache/vshot/makepkg-sources`) and every later build takes them from there, so only the first one needs a network — set `SRCDEST` and that is used instead, or delete the directory to force a re-download. If `models/` already holds the three files, copying them in skips even the first download, since the checksums match:

```sh
mkdir -p ~/.cache/vshot/makepkg-sources && cp models/* ~/.cache/vshot/makepkg-sources/
```

`onnxruntime` is a **virtual package** on Arch: all six variants (`onnxruntime-cpu`, `onnxruntime-cuda`, `onnxruntime-rocm`, …) declare `Provides: onnxruntime` and conflict with each other, so a system has exactly one. Depending on the virtual name rather than on `onnxruntime-cpu` means **someone who already has a GPU variant installed does not have to tear it out** for vshot — removing it would take `rccl`, `migraphx` and `rocm-hip-sdk` with it. vshot uses only the shared library and the `.pc` file, which every variant ships, and nothing here selects a GPU provider, so a GPU variant runs the OCR on the CPU exactly like the CPU one. A fresh install lets pacman pick; `onnxruntime-cpu` is the recommended choice (about 46 MB with cpuinfo and protobuf, against well over a gigabyte for the GPU builds).

## Application menu entry
The package installs a `vshot-settings.desktop` that shows up in the application menu as **VShot Settings** and opens the graphical settings window, `vshot settings`; its icon is `icons/vshot.svg`, installed under `hicolor/scalable/apps/` — one scalable SVG rather than a size ladder. Wayland has **no per-window icon**: a compositor takes the application id the client declares, finds `<id>.desktop`, and draws whatever `Icon=` names, so the name declared in `ui/main.cpp` has to match the installed desktop file's name. That id is also **registered with the host portal by Qt**, which prints `Failed to register with host portal ...` on stderr when no such file is installed; the id is therefore **declared only when the file is actually installed** (`QStandardPaths::locate` on `ApplicationsLocation`). A copy run straight from the source tree or `build-qt/` declares nothing and stays quiet.

The `vshot.desktop` that authorizes KWin is a **separate file** and cannot be merged into this one: its `Exec=` has to name `/usr/bin/vshot` exactly (KWin resolves the caller's pid to `/proc/<pid>/exe` and compares it against `Exec=`'s first word), and it is `NoDisplay=true`, because running `vshot` with no subcommand only reports that no output destination was given.

## Build
```sh
cargo build --release --locked                                # Rust CLI
cmake -S . -B build-qt -DCMAKE_BUILD_TYPE=Release             # Qt helper
cmake --build build-qt --parallel
```

OCR needs **ONNX Runtime's development files**: `ort-sys` finds `libonnxruntime.pc` through `pkg-config` (on Arch every one of the six variants ships it), and links against the system copy rather than downloading another at build time; without it the build fails and says why. The models live in the source tree's `models/` (`det.onnx` / `rec.onnx` / `dict.txt`, see [OCR](#ocr)); the build works without them, but `vshot ocr` then reports at runtime that it cannot find them. Interactive features look for the helper in this order: the `VSHOT_QT_HELPER` environment variable, the directory holding the `vshot` executable, its relative `../build-qt/` and `../../build-qt/`, then `PATH` (you can also point at it explicitly):

```sh
VSHOT_QT_HELPER="$PWD/build-qt/vshot-qt-ui" target/release/vshot region --output shot.png
```

At runtime you need a Wayland session, `wl_compositor`, `wl_shm`, at least one `wl_output`, `zxdg_output_manager_v1`, and `zwlr_layer_shell_v1`. **The seat is required only where it is actually read**: `monitor <name>`, `all`, `window active`, and `region --geometry` never touch it; `monitor current` needs a `seat pointer`, and interactive selection needs both pointer and keyboard. Whichever is missing exits non-zero and names it.

## Usage
```sh
vshot region --output shot.png              # Region capture: freeze, then drag
vshot region --clipboard
vshot region --geometry '100,200 800x600' --output shot.png

vshot monitor eDP-1 --output shot.png       # Outputs: by name
vshot monitor current --clipboard           # the one the pointer is on
vshot all --output desktop.png              # whole desktop (gaps between outputs stay transparent)

vshot window active --output window.png     # Windows: focused window
vshot window active --pixel                 # skip compositor metadata, use pixel detection
vshot window pick --output window.png       # click one on the live desktop
vshot window pick --pixel

vshot long --output long.png                # Long screenshot
vshot long --geometry '100,200 900x700' --output long.png
vshot long --ignore-top 48 --clipboard

vshot all --output - > desktop.png          # To stdout, logs go to stderr only

vshot region --pin                          # Pin straight to the screen, nothing written to disk

vshot pin shot.png another.png              # Pin management
vshot pin --clipboard                       # pin the clipboard contents
vshot pin --density 2 shot.png              # force the source density
vshot pin --toggle                          # show/hide all
vshot pin --show
vshot pin --hide
vshot pin --close-all
vshot pin --list
vshot pin --quit

vshot annotate toggle                       # Screen annotation: flip the overlay (starts a daemon)
vshot annotate clear                        # forget every annotation
vshot annotate quit                         # quit the daemon, drawing and all

vshot settings                              # Settings: a window for the editor style and the command-line defaults

vshot ocr                                   # OCR: frame a region, text to stdout
vshot ocr --json                            # OCR: frame a region, the text and each character's position as JSON
vshot ocr --clipboard                       # the same, onto the clipboard
vshot ocr --input shot.png                  # read an existing image file

vshot translate                             # Translation: frame a region and it translates at once; Enter accepts, text to stdout
vshot translate --output out.png            # write the composited translated PNG
vshot translate --clipboard                 # copy the composited translated PNG
vshot translate --input shot.png            # read an image file and translate its text
vshot translate --to en --from ja           # choose the target / source language
vshot translate --provider bing             # choose a different service
vshot translate --provider auto             # try the usable providers in order
vshot ocr --json | vshot translate --stdin-ocr --json   # translate an OCR envelope

vshot record monitor eDP-1 --output clip.mp4    # Recording: one output
vshot record monitor --fps 30                   # the output you are on (NAME defaults to current), 30 fps
vshot record all                                # every output, default video dir
vshot record region --geometry '0,0 800x600'    # a rectangle, desktop coordinates
vshot record window                             # one window's own pixels, occlusion and all
vshot record stop                               # stop the recording that runs
vshot record monitor current --duration 30      # stop itself after 30 seconds

vshot replay start monitor --background         # Replay: run detached; keeps the last 30s by default
vshot replay save                               # write that stretch to an MP4
vshot replay save /tmp/clip.mp4 --seconds 10    # only the last 10 seconds
vshot replay status                             # how much history it holds
vshot replay stop                               # end the session
```

The global options apply to every capture:

| Option | Meaning |
| --- | --- |
| `-o, --output PATH` | Write the PNG to PATH, expanding strftime formats like `%Y%m%d`; afterwards the file's `file://` URI is copied to the clipboard. A directory in PATH that does not exist is created, so a pattern like `%Y%m` (a directory per month) works as written. `-` writes to stdout without copying. `record` reads the same flag as the video path (strftime expanded, `.mp4` added when missing, `-` refused, no URI copied) |
| `--clipboard` | Copy the PNG data to the clipboard |
| `--pin` | Pin the image to the screen instead of writing it (the daemon deletes the temporary file once it is in memory) |
| `-c, --cursor` | Ask the compositor to draw the cursor into every output frame. **Not supported by `long`** (see ["Known rough edges"](#known-rough-edges)); for the most common reason a capture has no cursor, see ["The cursor (`--cursor`)"](#the-cursor---cursor) |
| `--png-compression LEVEL` | `none` / `fastest` / `fast` (default) / `balanced` / `high`, all lossless, differing only in time and size |
| `--hdr-format FORMAT` | Format of the second file beside the PNG when the capture carries HDR content: `avif` (the default) or `hdr`. See [HDR](#hdr) |
| `--tone-map MODE` | How the SDR half is mapped down from the HDR content: `auto` (the default), `fixed` or `normalize`. See [HDR](#hdr) |
| `--tone-map-white LEVEL` | Where SDR white lands in the output range, 0.5 to 0.95, default 0.8. Read by `fixed` and `auto` |
| `--hdr-area-test BOOL` | Whether HDR content is judged by **area** (default `true`). Off falls back to "any one pixel over counts" |
| `--hdr-area-ratio SHARE` | How much of the frame has to be brighter than SDR white to count as HDR content, 0 to 1, default 0.0005. `0` means always HDR. Read only while the switch is on |

Every capture must name exactly one output target. `region --geometry` and `--interactive` are mutually exclusive; with no geometry the default is interactive selection (`--interactive` states that intent explicitly). Path format examples:

```sh
vshot region --output "$HOME/Pictures/vshot-%Y-%m-%d_%H-%M-%S.png"
vshot all --output 'shots/capture-%Y%m%d-%H%M%S.final.png'
```

`%Y` `%m` `%d` `%H` `%M` `%S` are year, month, day, hour, minute, and second; `%%` is a literal `%`. Prefixes and suffixes combine freely. Both file and clipboard output need `wl-copy`.

## Region capture and annotation editing
When `vshot region` gets no `--geometry`, the frozen frame fills each output and everything outside the selection is dimmed by a translucent mask:

- Drag to draw the rectangle, eight handles around it resize, **hold the middle button** to drag inside and move it, arrow keys nudge (Shift accelerates to 10 logical pixels); while dragging or resizing an 8x magnifier and native pixel coordinates appear next to the cursor, and the selection's `width × height` sits outside the region (beside its left edge, level with its top; above or below it when there is no room there) instead of over the pixels being captured. With no tool armed, a drag inside the selection frames a new one rather than moving it
- **Enter**, a double-click inside the selection, or the toolbar's OK confirms; **Esc** or right-click cancels the whole capture (Esc inside a text box only closes that box)
- The toolbar is two rows of buttons with a pinned corner: the buttons are laid out in order — the drawing tools Rect, Ellipse, Arrow, Line, Wave, Bezier, Draw, Text, Number, Mosaic and Pick, then the actions that work on the capture, Image, Text+, Translate, Scroll and Pin — and cut into two rows where the wider of them comes out narrowest, so the rows stay about the same length and the card does not grow wider with every tool added; the corner holds Undo and Redo on the first row and OK and Cancel on the second, against the panel's right-hand edge rather than at the end of the longest row; style sub-panels appear according to the current tool and follow the selection. Scroll hands the selection to the scrolling capture (`vshot long`) instead of keeping it, and is greyed out when the selection spans two outputs
- **Pin** finishes the edit like OK, but the result goes straight onto the screen (as `--pin` does) instead of to disk; the pin editor does not offer it again
- **Pick** is the eyedropper: with the pointer resting on the picture a magnifier follows it and the pixel under the cursor is shown below as a swatch and its hex value; a click hands the colour to whichever tool was armed before Pick was chosen (with **no tool armed** it goes to Draw) and returns to that tool. Only the RGB is taken — the opacity stays whatever that tool had — and Pick has no style entries of its own, so the style row shows that tool's values
- Style entries: a color palette (with a custom picker: HSV gradient plus hex input), line style Solid/Dash/Dot, arrow head Open V/Filled, thickness 1-64, arrow size 1-8, font size 7-448 (the number *is* the pixel height), mosaic shape Rect/Ellip/Brush, mosaic strength 1-3, and a system font list (each entry previewed in its own glyphs). Arrow draws a straight arrow from press to release; Draw is freehand; the mosaic strength controls both the pixel block size and the brush radius
- No *Select* tool has to be armed first: **a click on any annotation selects it**, a drag on its edge moves it (text too), shapes/lines/mosaics resize by their handles, Delete/Backspace removes it; style changes apply to the selected annotation immediately; **Ctrl+Z / Ctrl+Y** (or Ctrl+Shift+Z) undo/redo. Annotations come back to Rust in global logical coordinates and the final PNG is redrawn by the built-in software renderer, matching the preview
- **Pasting and reading text**: the toolbar's *Image* button picks an image from disk, or **Ctrl+V** pastes whatever image the clipboard holds — it lands centred at its own size, shrunk to fit when it is larger than the selection, and comes up selected so it can be dragged and resized by its handles; the *Text+* button recognizes the text in the selection and selects it in place rather than copying all of it (see [OCR](#ocr); `cli.ocr.notify` turns it off)

The UI language follows the system by default (`QLocale::system()`) and can be overridden with `VSHOT_LANG`: a value starting with `zh` selects Chinese, any other non-empty value selects English. The language is fixed when the helper starts, so switching needs a rerun. The Rust CLI's `--help` uses the same rule, so `VSHOT_LANG=zh vshot --help` is Chinese.

## OCR
`vshot ocr` reads the text out of a region of the screen. With no arguments it asks you to frame it:

```sh
vshot ocr                    # frame a region, text to stdout
vshot ocr --json             # the same, but each line and each character's position as JSON
vshot ocr --clipboard        # the same, onto the clipboard
vshot ocr --geometry '0,0 800x200'
vshot ocr --input shot.png   # read an existing image file
```

A finished recognition raises a **desktop notification** by default: the text it read (cut at 160 characters), or why it failed — when `vshot ocr` is started from a keybinding, nothing else says it is done. The notification is handed to whatever owns `org.freedesktop.Notifications` on the session bus, and **a session with no notification daemon still works**: it just misses the note. The switch is on the settings window's *Text recognition* page, or the `cli.ocr.notify` key.

What lands on the clipboard is the recognized text itself, **with no trailing newline added**; one is added only for stdout, so the shell prompt does not end up on the last line of the output.

**`--json`** prints no plain text; it writes the recognized lines, and where each character sat, as JSON on stdout, so a program can place the text rather than read it. It conflicts with `--clipboard` and **suppresses the desktop notification** — a caller that asked for JSON is a program, not a person.

The editor's *Text+* button recognizes the selection's text too, but does not put the whole thing on the clipboard: the recognized text becomes a **selectable layer drawn where the characters actually were**, with **all of it selected to begin with**, so the common case — wanting the lot — is one **Enter** away. Drag across the text to narrow the range, double-click takes the word under the pointer, triple-click widens that to the whole line, **Ctrl+A** takes everything, **Enter** or **Ctrl+C** copies only what is selected and leaves the mode, and **Esc** leaves the mode and returns to ordinary editing (it does **not** cancel the capture; a second **Esc** does). The button itself says where it is: *OCR…* while the recognition runs, then *Copied* or *Failed*. While the mode is up the tools are not offered, because the layer describes the capture's own selection and a tool change would invalidate it.

An **external** engine (the GPU escape hatch) reports text and no character positions, so there is nothing to select: its whole text is copied as before and a line goes to stderr saying so.

Recognition uses **PaddleOCR's PP-OCR models** (the official models converted to ONNX) on ONNX Runtime, **on the CPU**, in this process. The models are the `PP-OCRv6_small` tier, about 30 MB:

| File | Size | Role |
|---|---|---|
| `det.onnx` | 9.4 MB | text detection (language-independent) |
| `rec.onnx` | 20.3 MB | text recognition |
| `dict.txt` | 73 KB | the 18708-character alphabet |

The package installs them under `/usr/share/vshot/models/`; a source checkout keeps them in `models/` (`vshot ocr` walks up from the executable, so `target/release/vshot` and `target/release/deps/vshot-*` both find it). **They are not committed** — the PKGBUILD fetches and SHA-256-verifies them through `source=()`.

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

- `command`: the program and its arguments, as an **array** (no shell, so no escaping)
- `stdin`: `true` sends the PNG on stdin; absent or `false` appends the temporary PNG's **path** as the last argument
- `timeout`: seconds, 30 by default; on expiry the child is killed rather than left to hang the capture

What that program is does not matter — a Python `rapidocr` on the ROCm wheels, a `curl` to a service on another machine, an ONNX Runtime build with CUDA. Here is one with Python rapidocr:

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

**A misconfiguration is an error, never a silent fall back to the CPU**: `engine: "external"` with no `command`, a command that will not start, or a program exiting non-zero each produce a specific message (including whatever the program wrote to stderr).

## Translation
`vshot translate` translates the text of a region of the screen into another language. With no arguments it opens the Qt overlay in its translate mode: frame a region and, the moment the drag ends, vshot recognizes and translates it and draws the translation over the original; Enter accepts it (after an Escape, Enter translates the same frame again):

The translation is drawn **over the original, in place**: each line's fill is the most common colour of the frame a couple of pixels above and below it, and every fill goes down before any text is written, so a later line's fill cannot repaint the glyphs above it. The glyphs are drawn in **the desktop's own font** — the Qt application font, which is the system font — and the built-in CJK list is only the fallback for a desktop font that cannot draw the text. A line that does not fit is shrunk first (never below half the line's height), and past that its fill grows sideways rather than spilling off the frame.

```sh
vshot translate                    # frame a region, the translation drawn in place; text to stdout
vshot translate --output out.png   # write the composited translated PNG
vshot translate --clipboard        # copy the composited translated PNG
vshot translate --input shot.png   # read an image file and translate its text
vshot translate --geometry '0,0 800x200'
vshot translate --to en --from ja  # choose the target / source language
vshot translate --provider bing    # choose a different service
vshot translate --provider auto    # try the usable providers in order
```

`--stdin-ocr` is the primitive the editor calls: it reads the **OCR JSON envelope** `vshot ocr --json` prints from stdin, translates only its `lines[].text`, and writes a **translated envelope** to stdout — no capture, no OCR run, no compositor, and **no notification**. This is exactly how the editor calls it:

```sh
vshot ocr --json | vshot translate --stdin-ocr --json
```

`--json` writes the envelope back unchanged except that each line's `text` becomes the translation and the original is kept alongside as `source`; `geometry` and each line's `rect` survive, while `chars` is dropped — per-character boxes exist only to **select the recognized text**, and the translated overlay never selects the original, so it has no use for them. `geometry` still reads `true`, because the line rects are real and are what the Qt text layer places the translation with. That is exactly the shape the editor's text layer already parses, so the translation lands where the original was. A line that fails to translate is still kept: its `text` stays the source and it gains an `error` field — **one bad line never loses the rest of the batch**. The `--stdin-ocr` route always emits the envelope, with or without `--json`, so the flag is implied there.

Without `--json` the translation is printed one line per line, the way `vshot ocr` prints recognized text. `cli.translate.notify` (off by default) controls the desktop notification when a translation finishes; the `--stdin-ocr` route never notifies.

### Providers
The nine services are chosen by `--provider` or `cli.translate.provider`; **five of them need no key of your own**:

| provider | what it needs | notes |
| --- | --- | --- |
| `google` | nothing | the keyless endpoint the official Android app uses; it splits on sentences and does not map them back to the input lines, so vshot asks one line per request (4 in flight) and puts them back in order |
| `microsoft` | nothing | Microsoft's keyless edge endpoint; several lines go in one request as a JSON array, and an empty `from` means auto-detect |
| `volcengine` | nothing | Volcengine's endpoint, called with the vendor's own Chrome extension's `Origin`; one line per request |
| `transmart` | nothing | Tencent Transmart; several lines go in one request (`text_list`) |
| `lingocloud` | nothing (borrows Caiyun's token; `token` overrides it) | Caiyun's interpreter; several lines go in one request, and `trans_type` is `<from>2<to>`; simplified Chinese is spelled `zh`, traditional `zh-Hant` is taken as written |
| `bing` | `api-key` (optional `region`) | Azure Translator; several lines go in one request |
| `baidu` | `app-id` + `secret-key` | Baidu fanyi; the lines are joined with `\n` into one `q`, and `sign` is `md5(appid + q + salt + secret_key)` |
| `ai` | `endpoint` + `api-key` + `model` | any OpenAI-compatible chat endpoint; `prompt` replaces the built-in system prompt |
| `external` | nothing (your own program) | reads the source lines on stdin and writes one translated line per line on stdout |

`google`, `microsoft`, `volcengine` and `transmart` all need no key: no account, no token, nothing to put in the config. `lingocloud` needs nothing configured either, but it runs on **a token borrowed from Caiyun's web app** (`9sdftiq37bnv410eon2l`) — the same constant turns up in dozens of third-party client forks, and Caiyun can revoke it at any time. To stop borrowing, put a Caiyun token of your own in `cli.translate.lingocloud.token`; if the borrowed one is ever revoked, that is the fix, not a code change.

`bing`'s `endpoint` defaults to `https://api.cognitive.microsofttranslator.com`; `ai`'s `endpoint` is used verbatim when it already ends in `/chat/completions`, and gains that suffix otherwise. A missing credential is an **error that names the key**, never a silent switch to another provider.

`--provider auto` (or `"provider": "auto"` in the config) names no service: it tries the providers in the built-in order **google, microsoft, volcengine, transmart, lingocloud, bing, baidu, ai, external** — the five credential-free ones first, `lingocloud` last of them because it borrows somebody else's credential — skipping any that has nothing to run with (`bing`, `baidu` and `ai` need their credentials and `external` needs a command; the five credential-free ones always run). The first one that answers is used, and the envelope's `provider` reports the one that actually did.

`cli.translate.fallback` is an optional array of provider names to try, in order, after the primary — or after the whole `auto` order when the primary is `auto`. It is **empty by default**, so nothing runs that you did not ask for. A provider counts as having answered when **at least one line translated**: a provider that fails every line — the throttled Google case, where each line comes back with its own `error` and the command still exits 0 — is passed over for the next, and once any line succeeds the whole result is kept, per-line failures and all. When a chain of two or more is exhausted, the error names each provider tried and the reason it gave. A name that repeats one already in the chain is skipped, and an unknown name is an error.

With **no `fallback`** the chain is a single provider, and it is not treated as a chain: its result comes back as it always has, per-line `error` fields included, so nothing about a plain `--provider google` run changes. The chain — and its all-failed error — only exists once you name a second provider.

`external` is the universal escape hatch — a shell script is all it takes:

```json
{"cli": {"translate": {"provider": "external",
                       "external": {"command": ["/usr/local/bin/my-translate"], "timeout": 30}}}}
```

It writes the source lines to the child's stdin and reads the translations back from stdout; **the line count has to match the input**, and a difference is an error naming the expected and actual counts, along with whatever the program wrote to stderr. The timeout is 30 seconds by default; on expiry the child is killed.

### Language tags
Languages are written the way vshot writes them (BCP-47-ish): `zh-Hans`, `zh-Hant`, `en`, `ja`, `ko`, … and `auto` detects the source. Each provider has codes of its own, applied before the request goes out:

- **Google** takes `zh-CN`/`zh-TW` and spells detection `auto`;
- **Microsoft** takes vshot's tags as they are, `zh-Hans`/`zh-Hant` included, and spells detection by leaving `from` empty;
- **Volcengine** passes most tags through, but **spells simplified Chinese `zh`** (`zh-Hans` makes the live endpoint answer in English); `zh-Hant` is taken as written, and the request carries no source field at all;
- **Transmart** collapses both Chinese scripts to `zh`, and sends the source exactly as asked, **`auto` included** — the live service accepts `auto` and detects the source, while the `en` some clients substitute makes it echo a non-English line untranslated;
- **Lingocloud** splices the two tags into one `<from>2<to>`: **only `zh-Hans` is rewritten**, to `zh` (the endpoint rejects `zh-Hans` as a source and as a target alike), while `zh-Hant` is taken as written and **really does yield traditional Chinese** (a live `ja2zh-Hant` comes back 「今天天氣真好啊。」, not the simplified 「今天天气真好啊。」; a `zh-Hant2ja` source is normalised by the service to `zh2ja`). Everything else — `de`, `ko`, any tag the old six-language client would have refused — passes through, and the service's own `rc=-1` is the error;
- **Bing** takes vshot's tags as they are, and spells detection by **omitting** the `from` parameter;
- **Baidu** takes `zh`/`cht`/`jp`/`kor`/`fra` and spells detection `auto`;
- **`ai`** and **`external`** pass the tag through unchanged.

**A tag the table does not know passes through unchanged**, never dropped: a service that does not understand it will say so, which is better than translating with the wrong language.

## Capturing windows
### window active
The focused window is obtained in this order:

- **KWin** (`CaptureActiveWindow`) and **niri** (`niri msg action screenshot-window`) are the compositor drawing the window itself — the only route that answers directly instead of cropping: KWin comes with decorations and shadow (the shadow's outer ring is transparent) and the `scale` in its reply is the density; niri renders the window to a PNG at a temporary path which vshot reads back — niri's IPC reports no absolute position for a tiled window, so on niri this is the only workable route. The render excludes the border (niri draws it on the tile) and vshot adds it back; for a translucent window it captures that output once more, locates the render on the frame and crops the frame, so the translucent parts show the real background, checking stability before retrying at most 3 times. `--no-blend` skips the whole locating step and hands over niri's render as it is: never misaligned, but the translucent parts are empty and the border is not included;
- Everything else falls back in order: Hyprland `hyprctl activewindow -j` → Sway's focused node `rect` from `swaymsg -t get_tree` → KDE Plasma's `kdotool` or a one-shot KWin scripting probe (reads `workspace.activeWindow.frameGeometry`) → **pixel detection** (detecting the window on the captured frame when none of the above is available).

`--pixel` skips all of the above and does pixel detection on the frame directly (for testing the detector, and the only choice when all you have is a rectangle). Detection runs **per output** on that output's own native pixels, with candidates ranked by confidence (border band → flood segmentation → closed outline contour → the whole output), and it **includes the border the compositor drew**; when the compositor draws no colored border around the focused window, candidates are ordered by "pointer's output → hit by the pointer → area", which answers with the window you are pointing at. A seamless borderless tiling layout (no gaps, no shadows) has no pixel signal at all, and vshot reports that honestly instead of guessing. Only **the current session's own compositor** is asked (decided by `XDG_CURRENT_DESKTOP` / `XDG_SESSION_DESKTOP`, with every probe tried when neither names one; niri ignores both and is recognized by the `NIRI_SOCKET` filename `niri.$WAYLAND_DISPLAY.$PID.sock`). `VSHOT_PIXEL_DEBUG=1` prints each level's verdict and `VSHOT_SESSION_DEBUG=1` the session verdict and its basis.

### window pick
First you pick a window on the **live desktop**: moving the pointer highlights the window under it and dims the rest, with a hint bar at the top-left giving the window title and size, a left click finishing the pick and Esc or right-click cancelling; then vshot **captures a fresh frame**, re-resolves the click position into a window rectangle against the current window list, and opens the same editing session `region` uses on that frame — switching workspaces or moving windows during the pick therefore never leaves the result on a stale picture. Candidates come from the compositor's window list (Hyprland `hyprctl clients`, Sway `swaymsg -t get_tree`, the KDE KWin scripting probe), ordered bottom-to-top in the compositor's own stacking order, and a pointer hit takes **the last window in the list containing the pointer**; candidates refresh with the pointer and every 300 ms even when it does not move; an empty list or a failed query falls back to pixel detection, while `--pixel` skips the window list, does pixel detection on the frozen frame and lets the user pick from all candidates.

**niri is the exception**: it has no usable window list, so vshot uses niri's own crosshair picker and niri renders the pixels itself — hence no dimming overlay and no annotation editor. Only `--pixel` returns to the overlay plus pixel detection.

## Long screenshots
`vshot long` stitches a scrolling region into one long image. Without `--geometry` it first asks for a selection (the same selection interaction as region capture, but confirming does **not** open the annotation editor). Then:

1. A hint bar appears in a screen corner reporting the stitched height and frame count, accepting **Enter / Space / left click** (keep the result) and **Esc / right click** (discard);
2. vshot sends the wheel at a fixed cadence (one batch every 120 ms by default, with `--notches` deciding how many notches per batch) while grabbing frames continuously at up to about 50 fps, aligning each frame with the previous one and appending newly revealed rows to the bottom of the long image whenever the shift is greater than zero; at the end it writes through the usual output routes (`--output` / `--clipboard` / `--pin`). The result does not pass through the annotation editor; to annotate it, pin it first and use the pin's editing feature.

**Stopping conditions**: six consecutive wheel batches with no shift (the page is at its end), `--max-height`, `--max-frames`, `--timeout`, or a key press; when a frame's shift is beyond the measurable range the wheel is halved (down to a floor of one notch), and only when several frames in a row still fail to line up does it stop and report why. Alignment uses only the rows that **belong to the page**: runs of rows at the top and bottom that stay put (title bars, fixed toolbars, status bars) are chrome, never probed and never repeated; each frame is compared only with the previous one, so error does not accumulate.

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
`vshot record` writes the screen to an MP4: frames come from the same capture backends the screenshots use (`zwlr-screencopy` on wlroots sessions, KWin's ScreenShot2 on Plasma) and encoding happens on the GPU's media engine.

```sh
vshot record monitor eDP-1 --output clip.mp4    # one output, NAME as in `vshot monitor`; defaults to `current` (where you are)
vshot record all                                # every output, composed at its position
vshot record region --geometry '0,0 800x600'    # a rectangle (desktop coords); without it, drag one on the frozen desktop
vshot record window                             # one window's own pixels (not the area it covers)
vshot record window --pick                      # click the window to record, or name it by app id or title
vshot record monitor --encoder hevc             # codec: h264 (default) / hevc / av1
vshot record monitor --encoder-backend nvenc    # backend: auto (default) / vaapi / vulkan / nvenc (NVIDIA encode)
vshot record monitor --fps 120                  # aim for 120 fps (1 or more, no ceiling, default 60); `--duration 60` stops by itself after 60 seconds
vshot record monitor --portal                   # through the desktop portal: its picker chooses (`record window --portal` offers windows)
vshot record monitor --mic                      # record the microphone (session default source, or name one); `--no-mic` refuses it
vshot record window --app-audio                 # record that window's own sound (may combine with --mic, summed into one track)
vshot record window --follow GameA --follow GameB   # record whichever of them has the focus (`--no-follow` turns it off)
vshot record mics                               # list this session's audio inputs
vshot record stop                               # stop the running recording
```

- **Which output `current` is.** A recording has nothing of its own on screen and a Wayland client sees no pointer at all, so `current` cannot ask the seat the way a screenshot does and asks the compositor instead — `hyprctl`, `swaymsg`, `niri msg` or KWin's D-Bus, the same query a pin uses to land on the right monitor: the output under the pointer where the compositor reports that (Hyprland), the focused output otherwise; when nothing answers, vshot says so and points at `monitor NAME` and `all`.
- **Region recording (`record region`).** Records a rectangle of one output, in desktop logical coordinates in the same form `vshot region --geometry` takes (`x,y widthxheight`), and it has to sit inside a single output (no compositor call copies a region spanning two; crossing one is an explicit error). Without `--geometry` the rectangle is dragged out on the frozen desktop through the same picker, and the microphone opens and the file is created only after a rectangle is confirmed. On a wlroots session the compositor renders just that rectangle into a dma-buf, zero-copy into the encoder exactly like `record monitor`; the encoded size is the rectangle's logical size times that output's scale (400×300 on a 2x screen records as 800×600). `--portal` and `region` are mutually exclusive: the portal's picker offers whole screens and whole windows, never a rectangle.
- **A window recording is not a recording of the area a window covers.** `record window` records the window's *own pixels*, which the compositor copies out itself (`ext_image_copy_capture_v1`). So a window covered by another one, one dragged half off the screen, and one on a workspace that is not even visible all record whole, and nothing behind the window ever appears. Name the window by app id or title (whole name first, then a case-insensitive substring), `--pick` it with a click, or leave it out for the focused one; a compositor without these protocols is told to record a screen instead.
- **Resizing a window mid-recording.** A window *resized* while recording does not end the recording: the new frames are **fitted into the recording's own canvas** — scaled down when larger, centred at their own size with letterboxing when smaller — so one MP4 keeps one frame size throughout, and the file is that window's whole history. A window that is *closed* ends the recording there, with the file finished properly and stderr saying why; if the window's output is off, disabled or disconnected, no frame ever arrives and that is reported after a few seconds rather than waited on forever.
- **Stopping / where it goes / frame rate.** `vshot record stop` (no display needed, so it binds to a compositor keybinding) or Ctrl+C in the terminal that started it; both finish the file properly first, so the result is always a seekable MP4, and `--duration` ends a recording on its own. The global `-o/--output` expands strftime; with no path the file lands in the videos directory as `vshot-%Y%m%d-%H%M%S.mp4` — `$XDG_VIDEOS_DIR`, else the one `~/.config/user-dirs.dirs` names (xdg-user-dirs keeps the localised name there, `~/视频` on a Chinese desktop), else `~/Videos` — and that directory is created when missing, as is the one a path you name with `-o` points into. A missing `.mp4` suffix is added; `-` (stdout) is refused. `--fps` is what the loop *aims* for; each frame carries the wall time it was on screen into the MP4 (variable frame rate), so a recording that drops frames plays back short rather than slow; a single 4K output measures at a steady 120 fps here.
- **The codec (`--encoder`).** h264 (the default), hevc or av1, all on the GPU's media engine. Encoding *and* muxing run on ffmpeg's libraries (`libavcodec` + `libavformat`, the same route wf-recorder takes), loaded at run time with `dlopen`, so a machine without the ffmpeg libraries still takes screenshots and only `record` reports what is missing; needs `ffmpeg` and `libva` (for AMD/Intel VAAPI). Every frame is an IDR (all-intra), so any player reads it and any position is seekable.
- **The encoder backend (`--encoder-backend`).** One of `auto` (the default), `vaapi`, `vulkan` or `nvenc`. `auto` goes zero-copy first — VAAPI (AMD/Intel), then Vulkan, then NVENC — and a fixed choice uses only that one and reports the layer that failed (for example `no CUDA device for NVENC`). **Every route encodes in hardware** (`h264/hevc/av1_vaapi`, `_vulkan` or `_nvenc`); vshot has no CPU encoder (no x264/x265). What differs is how a frame reaches the encoder: VAAPI and Vulkan import the compositor's dma-buf, so nothing is copied, while **NVENC has no dma-buf import** and carries its frames through the CPU (more CPU, with the encode itself still on the GPU). **On NVIDIA, zero-copy means `vulkan`**: there `*_vulkan` drives the same NVENC hardware unit, while `*_nvenc` can never import a dma-buf. On a multi-GPU machine NVENC uses the first CUDA device (`VSHOT_NVENC_DEVICE=<index>` picks another) and Vulkan the first physical device (`VSHOT_VULKAN_DEVICE=<index>`). An NVIDIA machine also needs an `ffmpeg` built with `nvenc` (most distributions ship one).
- **The microphone (`--mic`).** Records an input into the same MP4, encoded as AAC (ffmpeg's own encoder). A bare `--mic` takes the session's default source; a name or node serial records another input (`wpctl status` lists them — a serial like `--mic 55` is that monitor). The microphone is opened before the video encoder (its rate and channel count go into the MP4's header) and its samples are drained once per video frame, so the two tracks share one clock. `--no-mic` refuses a remembered microphone even when `cli.record.mic` is set; neither flag means the config's answer (silent by default). It goes through PipeWire, so like `--portal` it needs libpipewire; a session with no default input says so and names `wpctl status`.
- **Per-application audio (`--app-audio`).** Additionally records the sound the recorded window is *playing itself* — nothing another application is playing gets in. It only means something on `vshot record window` / `vshot replay start window` (other targets are refused). It can be used **on its own** or **together with `--mic`**: the microphone is the room, the application audio is the window, and the two are **summed sample-for-sample** in Rust into the MP4's **one** audio track (not two). The window's pid comes from the compositor — Hyprland, niri and KWin (through the scripting probe's `pid` field) all report one — and vshot takes that pid to PipeWire's client table to connect only to that process's playback node; Sway and labwc report no window pid and say so rather than quietly falling back to the microphone. A window playing nothing keeps the source it had (a recording just starting goes video-only), not an error. `--portal` and `--app-audio` conflict; the track is AAC.
- **Following the focus (`--follow`).** Give the windows to follow (`--follow NAME`, repeated) and the recording moves between them as the focus does — whichever of them has the focus is the one being recorded, and while the focus is anywhere else the recording **stays on the last one** (no gap, no interruption, and the other window's pixels never appear). `--follow` is mutually exclusive with a window `NAME` and with `--pick` (it is itself a way of choosing windows), and only `record window` and `replay start window` can follow. A **bare `record window` that gives no `--follow`** follows the windows `cli.record.follow` remembers (`cli.replay.follow` for the replay side), and `--no-follow` turns a remembered list off for one recording. A switch is the **same path a resize takes**: the new window's pixels are fitted into the canvas the file was opened with, so one MP4 keeps one frame size and the timeline is continuous. With `--app-audio` the application stream follows too, to the new window's own application, while the microphone beside it **keeps running**; a new window whose application is silent keeps the previous one rather than falling back to the microphone. The focus is asked for at most every 250 ms, and only when `--follow` was given; a compositor that cannot report the focus is **refused plainly** rather than never switching. Note that it follows *the focus*, not "the game being played": switching to a window outside the list leaves the recording on the last one, but what you did in that moment is not in the file.
- **Width limit 4096.** That is the hardware H.264 encoder's limit (measured on this machine's 7900 XT VCN), so `record all` across two 4K screens (5760 wide) is refused with that explanation — record one output instead, or switch to `--encoder hevc`, which encodes the 7680-wide desktop here. A single 4K screen (3840) is fine.
- **Remembered defaults.** When `--encoder`, `--encoder-backend`, `--fps`, `--portal`, `--mic` or `--follow` is not given, the value comes from the config file's `cli.record` section (`cli.replay` for the replay side; see [`cli` — command-line defaults](#cli--command-line-defaults)); `--no-portal`, `--no-mic` and `--no-follow` turn a remembered value off for one recording. A remembered `follow` list applies only to a bare `record window` (no NAME, no `--pick`); every other target ignores it rather than failing on it. `vshot record mics` lists the audio inputs this session has, one per line as a serial, a node name and a description, tab-separated (the node name is what `--mic` takes), and that listing is what the settings window's microphone row offers. (`--app-audio` is command-line only and is not a config key.)
- **`--portal` records through the desktop portal** (`org.freedesktop.portal.ScreenCast`). A compositor's own capture protocols each exist on some desktops and not others; the portal is the one every desktop has, and the price is that **the compositor decides what is recorded**: it shows its own picker, and whatever is chosen there is what gets recorded. `record monitor --portal` asks it to offer screens, `record window --portal` to offer windows, but a name or `--pick` cannot decide which one; every session shows the picker again. One stream per session, so `record all --portal` is refused. A screen cast produces a frame when the screen changes and none while it does not (a still screen becomes one long frame), and `--fps` is the rate asked of the compositor rather than the rate the file has. Frames that arrive as dma-bufs go to the encoder without a copy; otherwise they are converted to RGBA on the CPU, and `VSHOT_PORTAL_SHM=1` forces the memory path. Needs `xdg-desktop-portal` and `libpipewire` (headers to build against, its library `dlopen`ed at run time), so a machine without it simply has no `--portal`.

## Replay
`vshot replay` is a recording that keeps its last stretch of history in memory instead of on disk. The screen is encoded continuously and the packets go into an in-memory ring; `vshot replay save` copies what the ring holds into an MP4 — a stream copy, no re-encode — so the trigger costs almost nothing and nothing is written to disk until it is asked for. Leave it running while you game, hit a key, and the last stretch is on disk.

```sh
vshot replay start monitor --background     # run detached; keeps the last 30s by default
vshot replay save                           # write to the videos directory, timestamped
vshot replay save /tmp/clip.mp4 --seconds 10
vshot replay status                         # how many seconds it holds, how many saves served
vshot replay stop
```

`start` takes the same targets `record` does: `monitor [NAME]` (a bare `replay monitor` means the output you are on), `all`, `region` (`--geometry`, or dragged out on the frozen desktop), `window` (the focused one, a name, or `--pick`). The frames come from the same capture backends and go to the same libavcodec GPU encoder, and the zero-copy dma-buf path is used wherever `record` uses it.

A replay runs with a `--gop`-second key-frame distance (default 1) instead of the recording's all-intra stream, so the ring is a fraction of an all-intra stream; a save starts at the newest key frame at or before `now - seconds`, so the file holds at least the seconds asked for and decodes from its first byte (asking for more than the ring holds gives everything there is). The ring therefore keeps one extra GOP — otherwise it would evict exactly the key frame the save needs.

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

Control goes through `$XDG_RUNTIME_DIR/vshot-replay-<uid>.sock`, and `save`/`status`/`stop` need no display, so they bind directly:

```
bind = SUPER, R, exec, vshot replay start monitor --background
bind = SUPER SHIFT, R, exec, vshot replay save
bind = SUPER ALT, R, exec, vshot replay stop
```

A `save` raises a desktop notification saying where the file went and how long it is.

### Configuration

The defaults come from the config file's `cli.replay` section (see [`cli` — command-line defaults](#cli--command-line-defaults); the settings window's Replay card writes the same keys); a flag always wins over the file, and a value the file gets wrong (an unknown encoder name, a rate out of range) falls back to the built-in default rather than failing the whole replay. One session runs at a time (the pid file refuses a second); `VSHOT_REPLAY_SOCKET` overrides the control socket and `VSHOT_REPLAY_PIDFILE` the pid file `replay stop` reads. A compositor sends no new frame while the window's content does not change, so the replay sends the frame it is holding once more at the session's own frame rate, keeping the ring's timeline in step with the clock: after ten seconds of a still window, a save still reaches back over the last ten seconds. **A recording does not do this** — there a still picture is one long frame whose length the end of the loop supplies, so there is nothing to repeat and no encoding cost while nothing moves.

## Pin overlay
`vshot pin` pins images to the screen as overlays, held by a **resident daemon**:

- **Drag** to move (across monitors; the copy on the other screen follows); **wheel** zooms around the image center (0.1x–8x), with the factor briefly shown at the image's bottom-right; **double-click** closes that image; **left click** raises the image to the front, so a clicked one is always above the rest when they overlap. Whichever image the pointer rests on gets a solid black outline, the others light gray (2 logical pixels thick, not covering the image itself)
- **Its look is configurable**: corner radius, the shadow, border width and the two border colours live in the config file (square corners with a shadow by default) — see [`pin` — how a pinned image looks](#pin--how-a-pinned-image-looks)
- With the pointer over a pin and that screen holding the keyboard, press **Space** to enter the same annotation editor `vshot region` uses
- Right-clicking **any** pin opens a menu: a color card lists its formats first, and clicking one copies that value back to the clipboard (↑/↓ to move, Enter to copy, Esc to close; a badge flashes at the bottom-right once copied). Six rows follow, there for every pin: **Copy image** puts the pin's own pixels (its annotations included) on the clipboard as a PNG; **Save as…** opens a save dialog and writes the image out as a PNG, then reports the result in the same badge; **Edit** opens the annotation editor on the pin's marks; **Reset zoom** puts a pin the wheel has resized back at the size it arrived at; **Recognize text…** opens the pin editor already in the text-selection mode above (see [OCR](#ocr)), so the text of a pinned image can be read the same way; **Close** closes the pin, which a double-click on it also does

A new pin lands on the **active output**: the screen the pointer is on first, then the output holding keyboard focus when the pointer cannot be read, then the primary output. **A region or window capture pins back onto the place it was taken from**: the capture carries that rectangle's global coordinates, so the pin lands exactly there — neither centred nor cascaded — which is what makes pinning a window over itself need no dragging. It looks like every other pin, rim and shadow included as the config asks (set the settings' border width to 0 to leave the rim off): that rim is what tells the user the thing on the desktop is a pin and not the window. Files, clipboard images, cards and whole-output captures have no "own place" (a whole-output pin lands on its own output either way), so they open in the middle of the active output. What you can pin:

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

A color card shows the color itself as a swatch on the left and, on the right, one line each for `HEX` / `RGB` / `HSL` / `HSV` / `CMYK` (plus `HEX8` and `RGBA` when translucent, and `NAME` when there is a named color), with a checkerboard behind the swatch to indicate transparency. Both text cards and color cards are rendered at the pixel density of their output, so they are not blurry on HiDPI. An empty clipboard, content with neither an image nor usable text, or a missing `wl-paste` all produce a clear message and leave existing pins untouched.

### Pin sizing
A pin's size is decided by the **source density of the image**, and by default needs no arguments. The order is:

1. **Explicit** — `--density N` (1–4) or the `VSHOT_PIN_DENSITY=N` environment variable, which wins outright;
2. **vshot's own screenshot** — carrying the scale of the output it came from;
3. **The image's own statement** — the PNG `pHYs` chunk (192 DPI = 2x; 96 DPI is 1x). The decision looks at **whether the chunk is there** rather than at the value Qt reports (a PNG with no `pHYs` is also read by Qt as roughly 96 DPI), and every PNG vshot writes carries it;
4. **A record left by the screenshot tool** — a lone number in a `<image path>.scale` file, or a line `<image path> <scale>` in `$VSHOT_PIN_SOURCE_FILE` (default `/tmp/screenshot-path`; the path must match the image being pinned). A screenshot script only has to write one line after saving:

   ```sh
   echo 2 > "$shot.png.scale"
   ```

5. **Inferred from the image size and the output it lands on** — 1:1 when the image's pixels fit the landing output's native resolution; otherwise the scale of "the smallest screen that can hold it", finally capped once by the output's width (never enlarged). So the same image pinned on a 2x 4K screen occupies half the logical size it does on a 1080p screen, and the **physical size is the same on both**; `--density` is the final manual fallback. `VSHOT_PIN_DEBUG=1` prints each pin's decision (pixel count, final density, source, landing screen's `devicePixelRatio`), and `VSHOT_PIN_FOCUS_DEBUG=1` prints every focus change of every render surface (for finding out why an outline color is wrong).

### Pin edit mode
With a pin focused, press **Space** and the daemon exports that image and brings up the full annotation editor (toolbar, text, mosaic, undo/redo), the same as region capture:

- The editor covers the whole screen the pin is on but **does not draw the image itself** — the picture on screen is the real pin, and the editor only overlays annotations on it, cropping anything drawn beyond the image; dragging with the select tool on the image reuses the pin's own move logic through the daemon socket and **creates no copy**, arrow keys nudge (Shift to 10px), and the image cannot be dragged off screen. Walking the cursor with the arrow keys **moves the pointer on the screen here too**: the pin editor's session describes only the pin's screen, so the daemon writes the whole layout's bounding box into the session as well and the CLI converts against that — without it the pointer would land a fraction of the way to where it belongs;
- The frame the editor draws around the image is **drawn only while this pin is the live one**. It is the same Qt chrome a region selection gets, and every other pin is painted by a Wayland surface one layer below — a layer's surfaces stack by map order and the protocol has no restack, so once the user clicks another pin this frame would sit on top of every other pin on the screen, and no ordering on the editor's side could put it back underneath. The daemon knows which pin is live (its surface holds the keyboard and the pointer is over it) and sends that answer over the pin socket (`watch-active`); the editor drops the frame on it. **The edit itself is unaffected** — the marks, the input region and the drag all carry on, and the frame comes back when the user returns to the pin;
- **Clicking back onto the pin raises it to the front**. A click normally raises a pin through the pin's own surface, but while an edit is open the editor's layer surface covers the output and holds the keyboard, so the click never reaches the surface underneath — the editor sends a `raise` on the user's behalf instead, on a press on the image or on its border. Without it, clicking back onto the pin being annotated would bring the frame back but leave the picture under the other pins;
- **Esc cancels**: annotations are discarded and the pixels stay as they were, but the position does not roll back — wherever you dragged it, there it stays. **Enter or OK confirms**, writing the annotations back with it; only one pin can be in an editing session at a time.

### pin daemon
A layer-shell overlay surface belongs to the process that created it, so a resident process is needed:

- The Qt binary is reused: `vshot-qt-ui --pin-server <socket>` is the daemon, and the first `vshot pin` that cannot reach the socket starts it detached (not holding the terminal); the CLI is a thin client sending one-line JSON requests over a Unix socket, which defaults to `$XDG_RUNTIME_DIR/vshot-pin-<uid>.sock` and can be overridden with `VSHOT_PIN_SOCKET`. About 0.5 s after the last pin closes (or after `--close-all`) the daemon exits on its own and the next pin command starts it again; `vshot pin --quit` exits it manually at any time;
- **Never end the daemon with `pkill` / `kill -9`**: it holds layer-shell surfaces, and when killed hard some compositors (Hyprland 0.56 measured) leave the surface and its screencopy session behind, which makes **screencopy block forever on every output** (`vshot` and `grim` all time out, and `hyprctl reload` does not recover it — only restarting the session does). Always use `vshot pin --quit`, which unmaps every overlay before exiting; the daemon also handles `SIGTERM` / `SIGINT` through the same graceful path.

A Wayland client receives no global key presses, so "one-key show/hide" has to be bound to a compositor shortcut yourself, for example on Hyprland:

```
bind = SUPER, P, exec, vshot pin --toggle
bind = SUPER SHIFT, P, exec, vshot pin --close-all
```

## Screen annotation
`vshot annotate` turns the desktop into a canvas you can draw on: a **resident daemon** lays one layer-shell overlay surface (Overlay layer, the same kind the pin overlay uses) on every output, and the mouse draws straight onto the **live desktop**. Each output has its own canvas and its own toolbar, so a stroke stays on the output it was made on. The daemon is spawned detached as `vshot-qt-ui --annotate-server <absolute socket path>` the first time `show` / `toggle` cannot reach the socket, exactly like the pin daemon; `show` and `toggle` start a daemon, while `hide`, `clear`, `quit` and `status` never do.

```sh
vshot annotate toggle     # flip the overlay (starts a daemon if none runs)
vshot annotate show       # show it (also starts a daemon)
vshot annotate hide       # hide it, keeping everything drawn
vshot annotate clear      # forget every annotation, on every output
vshot annotate quit       # quit the daemon, drawing and all
vshot annotate status     # is the daemon running, how many strokes it holds
```

The toolbar is a small horizontal palette sitting centred along its output's top edge; a grip on its left drags it anywhere. It holds Draw (freehand pen), Erase, Rect, Arrow, Text, six colours, three line widths, Undo, Redo, Clear, and the ✕ at the right that quits the daemon. **Erase removes a whole stroke it is dragged across** (undoable) rather than rubbing out pixels; **Text** opens an inline text box where you click, Enter commits and Escape cancels, and whatever the input method produces — Chinese included — is taken. Undo/redo cover erasing and clearing too, so a stray Clear costs one undo.

While the overlay is shown it **takes every click on that output** — that is what "draw anywhere" means — so getting your pointer back means binding a second compositor hotkey that runs `toggle` or `hide` (a Wayland client receives no global keys; on KWin/Plasma make a custom shortcut, on niri add a `binds` entry). The overlay deliberately **does not hold the keyboard**: only the inline text box raises keyboard interactivity, and it gives it back when it closes, so typing in other windows keeps working.

```
bind = SUPER, A, exec, vshot annotate toggle
bind = SUPER SHIFT, A, exec, vshot annotate quit
```

The annotations are meant to be in the picture: a screenshot or a recording hides the **toolbar** by itself (not the drawing), so what is frozen has the strokes and not the toolbar; even a killed recorder gets the toolbar back, because the daemon watches the requesting process's pid. **A replay session (`vshot replay start`) is not covered by that hiding** — it grabs continuously for as long as it runs, and hiding the toolbar for its whole life would make the overlay unusable, so annotate and replay are not meant to run together. The feature was verified on **Hyprland** only, and like the pin overlay it needs `wlr-layer-shell`, so a compositor without layer shell cannot draw it at all. Finally, the same warning the pin daemon carries, for the same reason: **never end the annotation daemon with `pkill` / `kill -9`** — it owns layer-shell surfaces, and a force-killed client can leave a mapped surface behind that wedges screencopy on some compositors (measured for the pin daemon on Hyprland 0.56). Use `vshot annotate quit`, which unmaps everything first; the daemon also handles `SIGTERM` / `SIGINT` the same way.

## Images and output mapping
**A rectangle that falls inside a single output is always cropped from that output's own native frame and written with that screen's scale as its density**: `region`'s `--geometry` and interactive selections, `window active`, `window pick`, and rectangles from pixel detection all take this route, and only rectangles **crossing a seam** fall back to the composed scene; `all` is the whole desktop and can only come from the scene. Internal frames are uniformly RGBA8 with a top-left origin, and multi-output composition supports negative logical origins and gaps between outputs (the scene canvas uses the highest output scale, with lower-scale outputs enlarged by nearest-neighbor). Positive integer scales, `transform=normal`, and a provably safe logical/pixel mapping are currently required; fractional scale, rotation, and mappings that cannot be proven fail clearly rather than producing a plausibly wrong screenshot. This validation applies only to the routes that **need to compose outputs into a scene**, so KWin's and niri's two routes that hand over window pixels directly still work on a rotated or flipped output.

### HDR
**None of this has been run against real hardware.** The machine it was written on has neither an HDR display nor a compositor that offers HDR output, so nothing described below has ever met a real HDR frame: the protocol handling and the pixel arithmetic are covered only by the offline checks (`vshot-pin-hdr-check`, `vshot-backdrop-check`), and HDR capture, the tone map, the `.hdr` write and the HDR pin are written to the protocol rather than measured. SDR output is unaffected — a compositor that describes no HDR writes no `.hdr` and lays no backdrop surface.

When the compositor describes the output itself as HDR (PQ or HLG), one capture produces **two** files: `<name>.png` is an SDR tone map of the same content and the second holds the HDR content itself, beside the PNG under the same name and a different suffix (`<name>.avif` by default; `--hdr-format hdr` makes it Radiance RGBE, `<name>.hdr`). Annotations are composited **in linear light** onto the HDR one, and the SDR one is mapped down from it, so both describe one set of marks over one set of light. The SDR half does **not** take the picture the compositor prepares for an ordinary client: Hyprland writes that against `DEFAULT_SRGB_IMAGE_DESCRIPTION` (peak 80 cd/m²), while the white an SDR image means is the output's own reference (203 here), so it puts ordinary SDR content below white instead of on it — measured at sRGB 220 rather than 255 (145 cd/m²) — dimming the whole capture, not just the highlights. VShot therefore maps it itself. **A frame that holds nothing above SDR white is an SDR image and is shown exactly as it was**: white lands on 255 and every code below it keeps its own light. Only a frame that really does carry light above white moves its white down — to `SDR_WHITE_LEVEL` (0.8), so the codes above it have somewhere to carry the highlights — and there light up to white is scaled by that level, keeping its ratios and its hue, while light above it is rolled off by a Reinhard curve toward `ROLL_OFF_PEAK` (ten times SDR white), monotonically and separated, so a brighter highlight is not flattened to white (8× SDR white lands on 252, 21 codes above white) — which is exactly what the old peak-normalising map could not do: it put everything above white on one code, which is what made the `HDR`/`WCG` text in testufo's HDR test indistinguishable from the SDR around it. The price is that the white point is then the frame's and not a fixed level: the same light lands on a different code depending on whether a highlight shares the frame. That is the trade the other order makes — a fixed level dims *every* SDR capture taken on an HDR output to sRGB 231, which is a screenshot of an SDR window coming out wrong. **The gamut is converted with the chromaticities the output itself reports**: Hyprland describes a P3 panel with coordinates that are neither BT.709's nor BT.2020's, and guessing the nearer of those two converts every colour of the frame wrongly. A colour the target gamut cannot hold is **desaturated toward the white point** instead of having its negative components clamped, which would move its hue. Content that never rises above SDR white gets no second file — see the next paragraph for how that is judged.

**How "the frame carries light above SDR white" is decided**: inside the program, from the frame the daemon is holding, never from an external file — so the tag, the second file and the pin's route all move together. The outer gate is the **output's own declaration**: only an output the compositor describes as PQ or HLG ever hands out a ten-bit buffer, so an SDR output has no second file to write at all. Past that gate lies a different question — **does this rectangle hold light above white?** — which the declaration cannot answer: an SDR window and an HDR video on one HDR output are declared identically. So it is judged by **area**: the pixels above SDR white by `HDR_WHITE_EPSILON` have to make up `--hdr-area-ratio` of the frame (0.0005, five thousandths of a percent, by default) for the capture to count as HDR content. Why not the single brightest pixel: ten-bit PQ puts **ordinary SDR white** on codes a little above 1.0 (against a 203 cd/m² reference, codes 594/595/596/597 decode to 0.99958/1.00897/1.01845/1.02801), so a plain SDR desktop holds thousands of pixels "above white" — one 3.7 MP desktop measured 17 489 pixels over 1.02; the old test was "the peak is over 1.02", so **one** such pixel reclassified the whole frame, moved the white point down and dimmed the capture about **18 %** (measured mean 49573 against 60606). Raising the threshold to 0.05 does not save it either: that same frame still holds about 37 pixels above it (measured share 0.00001), past any "one is enough" test. A real highlight is a **patch**; quantization noise is **scattered** — so the test weighs the share, not the peak. Two switches put it back: `--hdr-area-test false` restores "any one pixel over counts" (the old behaviour), while `--hdr-area-test true --hdr-area-ratio 0` means "every capture of an HDR output is HDR content", with no region test at all. The threshold itself (`HDR_WHITE_EPSILON`, 0.05) is not configurable: it has to clear the ten-bit PQ quantization step, and a 1.5× highlight decodes to 1.5, far above it.

The second file's format is `--hdr-format` (or `cli.hdr-format` in the config), defaulting to `avif`: ten bits per channel, BT.2020 with the PQ transfer, with the AV1 sequence header and the container's `colr` box declaring the same CICP triple, so every reader that understands AVIF shows it right — at the cost of being **lossy**. `hdr` is Radiance RGBE, which **keeps the gamut it was captured in** — nothing is converted, so the wide gamut survives — and declares it in a `PRIMARIES=` header. RGBE has no colorimetry field of its own, and readers that ignore the line (ffmpeg and ImageMagick both do) assume Rec.709 and show a wide gamut over-saturated; the SDR half beside it is the one converted for them. Choose it to archive without loss. AVIF goes through `rav1e` + `avif-serialize`: `image`'s own AVIF encoder cannot carry HDR (it always writes eight bits, and its colour description is fixed at sRGB / BT.709) — see `src/model/avif.rs`.

Colour is asked of the display, never guessed from the pixels: a 10-bit buffer on an HDR output *is* that output's own pixels, decoded with the transfer function and reference white it publishes (`wp_color_manager_v1`'s output description, whose reference is the output's own SDR white). On Hyprland that holds only while `misc:screencopy_hdr` is on — with it off the compositor hands over 8-bit sRGB, and no second file is written.

That map's behaviour is chosen by `--tone-map` (or `cli.tone-map` in the config), and there are three:

- **`auto` (the default)** — the white point is the frame's, exactly as the paragraph above describes: a frame with nothing above SDR white is shown as it was, and only one that really carries highlights moves its white down. "Really carries highlights" is the area test below, the same answer that decides whether a second file is written at all, so a screenshot of an SDR window comes out **exact**.
- **`fixed`** — the white point is always `--tone-map-white` (0.8 by default), whatever the frame holds. The cost is that *every* SDR capture taken on an HDR output comes out slightly dim (white lands on sRGB 231); what it buys is that **a pixel's code does not depend on what else shares the frame**, which is what lets a pinned copy match the content it was taken from.
- **`normalize`** — the map normalises to the frame's own peak, so SDR white lands on `1 / peak` and the brightest point lands exactly on white. Highlights keep their *ordering* but not their separation: everything above white is compressed into whatever the reciprocal leaves, so a bright gradient flattens. Only right when the frame's peak is known to be a highlight worth normalising to.

`--tone-map-white` is where SDR white lands in the output range, 0.5 to 0.95; a value outside that span is **clamped** rather than refused, because it is a number the user meant and the map has a defined answer for it. It is read by `fixed` (always) and by `auto` (only for a frame that really carries highlights); `normalize` works its own out and ignores it. In the settings window it is a percentage box (80 % by default), greyed out while `normalize` is selected.

The frozen frame is shown the same way in the interactive overlay: VShot puts up a surface *underneath* it carrying **the output's own image description** — not one built to resemble it — so the compositor neither converts nor tone-maps it, and the selection is the light the screen actually showed. The overlay draws only the veil (cut open at the selection), the annotations, and the toolbar. A description that merely resembles the output's is not enough: the compositor treats it as a different space and tone-maps it into the panel's own range, dimming the whole picture.

**A pinned HDR image is shown in HDR too**, for the same reason: the pin daemon starts a small process of its own (`vshot --pin-hdr-server`), which puts one Overlay-layer surface on every output carrying that output's own image description — the very one the frozen frame uses — and writes the annotated capture into it re-encoded as ten-bit PQ. The compositor neither converts nor tone-maps it, so what lands on the screen is the light the pixels stand for. A Qt pin surface cannot do this: it is a Qt window, its description is built from a `QColorSpace` and carries no luminances, and the compositor treats it as another space and darkens it. So **every pinned picture is drawn by that helper**, not only the HDR ones — and the reason is the stacking order rather than HDR itself: one layer's surfaces stack in map order and the protocol has no restack request, and the helper's surface has to be mapped before the daemon's first one (it reports in first), so it always sits below every Qt surface. Anything a Qt surface painted would land above every pin the helper draws, including the pins in front of it — a rim would show over the pin that was meant to cover it. The whole stack therefore belongs to the helper — picture, shadow and rim, one surface and one commit: an HDR pin passes through as its own PQ codes, and an SDR pin is handed over as a PNG the daemon writes and the helper encodes against that output's own reference white (below). The Qt surface keeps only the **badges, the right-click menu, the `HDR` tag and the input mask**, and leaves the image's rect transparent. This also fixes, in passing, an SDR pin being darkened by the compositor's tone map on an HDR output: what the helper is given is a light, not an sRGB image waiting to be converted. The helper has to be mapped before the daemon's first surface, so it is started with the daemon. A capture that never rises above SDR white (by the area test above) gets no second half and its pin is an ordinary SDR one — travelling the same PNG path. On a compositor with no `wp_color_manager_v1` the helper maps nothing at all and the whole stack falls back to the Qt surface painting it itself, rim and shadow included. Each picture file is read once (cached by path), a drag sends only coordinates, and only the last position in a batch is composed — so a fast drag does not repaint the output for every motion event.

## Capture backends and KDE authorization
Probed once at startup: `wlr-screencopy-unstable-v1` is tried first, and only a failure of "missing `zwlr_screencopy_manager_v1`" means this compositor does not provide the protocol; then **KWin ScreenShot2** — KWin's private session-bus service `org.kde.KWin.ScreenShot2` (KWin has neither screencopy nor `ext-image-copy-capture`). vshot passes the write end of a pipe, and KWin writes the pixels into it and reports `width` / `height` / `stride` / `format` / `scale` in its reply; the pixels are premultiplied-alpha BGRA, output screenshots normalize alpha to 255, and window screenshots keep it as is.

When neither backend works, the error names both causes. Neither implements PipeWire, Portal ScreenCast, or DMA-BUF, and GNOME/Mutter provides none of the three (nor layer-shell), so **GNOME is not supported**.

### KDE authorization
KWin's `ScreenShot2` is a **restricted D-Bus interface and shows no dialog at all** — there is no "allow" button. From the caller's pid, KWin takes `/proc/<pid>/exe`, finds the installed desktop file whose `Exec=` first word matches that path, and requires it to declare:

```ini
X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2
```

- **A package-installed vshot needs nothing extra** (the packaged `/usr/share/applications/vshot.desktop` already declares it); if KWin still refuses after installing, run `kbuildsycoca6 --noincremental` once, since a newly created desktop file may take a few seconds to be picked up. **A development build run straight from `target/release/vshot` gets no authorization** — no desktop file's `Exec=` points at that path — so add one (write the current binary's absolute path into `Exec=`; vshot prints that path in its error):

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
| **Screen annotation** (`annotate`) | ✅ wlr-layer-shell (verified) | ⚠️ untested | ⚠️ untested | ⚠️ untested | ⚠️ untested |

Where nothing is adapted, vshot **degrades automatically instead of erroring**: with no window list it falls back to pixel detection, and when it cannot ask which screen the pointer is on it falls back to the primary screen Qt reports.

### How far verification goes
- **Hyprland** — this machine's session is Hyprland and it is the main development and verification environment: capture, selection and annotation, windows, long screenshots, pins, and scroll injection have all run here. **Recording encoder backends** (this machine, 7900 XT): both the VAAPI and the Vulkan route were measured (zero-copy dma-buf, h264/hevc/av1, 4K at a full 60fps, mid-recording window resizes, the portal, replay); NVENC's **routing and failure path** are verified (it fails cleanly with a named reason on a machine with no NVIDIA), but **a real NVENC encode has never run on NVIDIA hardware**.
- **Per-application audio / microphone** — the isolation was measured on Hyprland (two mpv players at 880 Hz and 220 Hz, `--app-audio` capturing only the recorded window's stream) and so was the mix (`--mic --app-audio` produces **one** AAC track carrying both); the niri and KWin pid paths have unit tests and headless runs but no isolation measurement against a real audio session; Sway and labwc have no pid source and refuse plainly. **Focus following (`--follow`)** was measured on Hyprland (two mpv windows of different size and tone, one continuous file across a focus switch); a compositor that cannot report its focus is untested here.
- **niri** — both the tiled and floating **screenshot** paths were verified on a real session (`--no-blend` bypasses the floating-window locating step); **a window recording and a window replay go through the compositor's own screen-cast service** (`org.gnome.Mutter.ScreenCast` — no portal, no picker, no permission prompt), and a 4K window recording, a mid-recording resize, the end of a recording on window close, and the replay's `status`/`save`/`stop` were all measured; `--follow` is unavailable on that route (the service casts the window it was started with). **One throttling caveat on niri's side**: it draws the cast inside its **output render loop**, so an inactive session barely renders at all (measured at about 1.3 fps); for a normal frame rate, niri's VT has to be the active one. **KWin/Plasma**: D-Bus capture, the window list, long screenshots, and window recording and replay were all measured against a **headless `--virtual` KWin** (including the retry rule for authorizations transiently refused while ksycoca is rewritten); **still unverified**: `--cursor`, the full `--pick` interaction on a window click, and scroll injection (that needs acceptance on KDE).
- **Sway / labwc / GNOME** — Sway is probe implementations and unit tests only, with **no live verification**; **labwc and other compositors providing wlr-screencopy** should work for basic capture in theory, but there is no window list and no pin-landing probe, and there is **no live verification**; **GNOME** is unsupported, with no plan to implement it (Mutter provides neither wlr-screencopy nor KWin's D-Bus service, and not even layer-shell).
- **Screen annotation (`annotate`)** — verified on **Hyprland** only (drawing, erasing, text, undo/redo, a separate canvas and toolbar per output, and the toolbar hiding itself during a capture or recording); niri, KWin/Plasma, Sway and labwc are all **untested**.

### The cursor (`--cursor`)
vshot never draws a cursor itself; `--cursor` only sets an "overlay the pointer" flag on the compositor's capture request (`wlr-screencopy`'s `overlay_cursor`, KWin's `include-cursor`), and whether, where, and when it is drawn is entirely up to the compositor; it has no effect on `long` (see the next entry), and whether KWin really draws a cursor is unverified.

- **A capture with no cursor after typing a command in a terminal is not a bug.** A terminal (kitty measured, `mouse_hide_wait` defaults to 3.0 seconds) hides the pointer after a few seconds without mouse movement — it performs `set_cursor(null)` on the compositor, after which there **really is no cursor to draw** at the compositor level, and any tool going through screencopy sees the same thing. Move the mouse before capturing after pressing Enter, or set `mouse_hide_wait 0` for kitty.
- **On niri the pointer is drawn into the window capture, not the output frame.** On niri, `window active` / `window pick` go through niri's own `screenshot-window`, and `--cursor` maps to that call's `--show-pointer`; that argument only exists after 25.11, and an older niri rejects the whole request, which vshot detects and retries without the argument. The output-level paths (`monitor`, `all`, `region`) still use screencopy's `overlay_cursor`.
- **On Hyprland running `hypr-dynamic-cursors` the pointer gets baked into the frame, and vshot steps around it**: while the cursor is magnified the plugin holds the compositor's software-cursor lock, and the compositor then draws the pointer into the frame it composites, which is the very frame screencopy hands out — so **the pointer is there without `--cursor` too**. Before reading a whole scene, vshot switches the plugin off and warps the pointer back to where it already was, then switches it back on as soon as the frames are read; a session without the plugin, or with it switched off, is left alone entirely.

### Known rough edges
- **`--cursor` does nothing on `long`** — the frame grabbing in `src/longshot.rs` hardcodes the cursor argument to `false`, so `vshot long --cursor` is accepted and silently ignored. On the other capture paths (`region`, `monitor`, `all`, `window active`) `--cursor` works as measured on this machine's Hyprland; on KWin `include-cursor` is passed, but **whether a cursor is really drawn is unverified**.
- **A capture taken mid-magnification can still carry a pointer** — `hypr-dynamic-cursors` holds the software-cursor lock while the cursor is magnified, and switching the plugin off does not release it immediately, so a frame grabbed in that instant keeps a **normal-sized** pointer in it. The plugin releases the lock when the magnification ends, so the problem heals itself.
- **Pixel detection (`--pixel`)** — a seamless borderless tiling layout (no gaps, no shadows) and a completely uniform desktop offer no pixel signal, and vshot reports that honestly instead of guessing. When the compositor draws no colored border around the focused window, it answers with **the window under the pointer**, which may differ from the compositor's focused window. `VSHOT_PIXEL_DEBUG=1` shows each level's verdict.
- **Three niri details** — translucent window locating: template matching requires the window's content to stay put between the render and the frame grab, video and animation make the template stale (vshot retries with a fresh render up to 3 times), and a nearly fully transparent window or one hanging off the output's edge cannot be located at all (falling back to niri's own translucent render); session detection uses the `NIRI_SOCKET` filename instead, and when it gets this wrong the symptom is `window pick` opening the dimming overlay instead of niri's crosshair picker (`VSHOT_SESSION_DEBUG=1` shows what it decided); pin landing can only ask "the output the focused window is on", so it **follows the keyboard focus, not the pointer**.
- **The KDE window list probe** — depends on journald receiving KWin's `console.info`. When KWin is started from a tty and its log goes only to that tty, no line can be retrieved no matter how long you wait. With `kdotool` installed vshot prefers it, bypassing that dependency.
- **pin daemon and screencopy** — do not end the daemon with `pkill`/`kill -9` (see ["pin daemon"](#pin-daemon)), or some compositors leave the layer surface and its screencopy session behind, making screencopy block forever on every output.

## Configuration file
`vshot` remembers two things in `$XDG_CONFIG_HOME/vshot/config.json` (or `~/.config/vshot/config.json`): the **annotation editor's style**, and **defaults for some command-line flags**; the file is optional, and missing, unreadable, or malformed all fall back to the built-in defaults without affecting a capture. There are two ways to change it: **edit the file by hand**, or run **`vshot settings`** for a window over both sections. **Nothing a capture session does is written back** — the colour you picked, the width you dragged to, the tool you switched to are that session's working state, and the config is the *reset* value every session starts from.

```bash
vshot settings
```

The window is an ordinary window: it captures nothing and needs no compositor protocol, so it also works on a compositor vshot cannot otherwise capture; after installing the package you can also open it from the application menu as **VShot Settings** (see ["Application menu entry"](#application-menu-entry)). An empty or zero value in the `cli` section means "leave it unset, use the built-in default" rather than storing a zero, and **the settings window shows those defaults directly** (a default left where it is still stays out of the file, so if a later version changes that default, whoever never touched it follows along), while the `editor` section is always written whole; saving **merges**, so keys this build does not recognize survive untouched. The window has eight pages, switched from the sidebar, **one feature per page**: **Annotation editor**, **Output**, **Scrolling capture**, **Text recognition**, **Recording** (every default for `record` and `replay`), **File dialogs**, **Pin appearance** and **Keyboard**; every page but **Recording** fits without a scrollbar at the default window size. A page's settings are grouped by **subject**, told apart inside a card by a hairline and a small heading — the Output page splits into PNG, HDR and which output — and ordered by **how often a user reaches for them**: the ones changed often first, the set-once ones after. The microphone row **does not hold the window up**: filling it means starting a `vshot record mics` child to ask PipeWire, which takes about a second, so the window opens on the two answers that always exist — do not record, the session's default — and the device list drops in when it lands; the Detect button asks again. Saving **does not close the window**: a "Saved." note appears in the corner, and Cancel is now only "close".

```json
{
  "editor": {
    "tool": "arrow", "color": "#ff8800ff", "width": 4, "textPixels": 28,
    "dash": "dotted", "arrowSize": 3, "arrowStyle": "filled",
    "mosaicShape": "brush", "mosaicStrength": 3, "font": "Noto Sans",
    "selectMode": "loose"
  },
  "cli": {
    "png-compression": "high",
    "hdr-format": "hdr",
    "tone-map": "fixed",
    "tone-map-white": 0.75,
    "hdr-area-test": false,
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
This section is the style **every session starts from**. Nothing a session does is written back — this file holds the reset values, not the last session's leftovers.

| Key | Values | Default |
| --- | --- | --- |
| `tool` | `select` / `rectangle` / `ellipse` / `arrow` / `pen` / `text` / `mosaic` | `select` |
| `selectMode` | `precise` / `loose` | `precise` |
| `color` | `#rrggbb` or `#rrggbbaa` | `#ff4040ff` |
| `width` | 1–64 | `2` |
| `textPixels` | 7–448 (a pixel height) | `14` |
| `dash` | `solid` / `dashed` / `dotted` | `solid` |
| `arrowSize` | 1–8 | `1` |
| `arrowStyle` | `open` / `filled` | `open` |
| `mosaicShape` | `rect` / `ellipse` / `brush` | `rect` |
| `mosaicStrength` | 1–3 | `2` |
| `font` | font family; an empty string uses the system default | `""` |

`tool` is the tool an **editing** session starts with and, like every style here, it lives **only in the config**. A fresh region capture does **not** take it: its first step is dragging the rectangle, and opening straight onto a drawing tool (say Text) makes the first click place a label. It does not take it once the frame is drawn either, so the drag that follows a fresh frame re-frames rather than inking, and the user can adjust the region without first putting a tool down. What does take it is a session that **opens with a selection already made**: `window pick`'s follow-up edit and a pin re-entered for editing, whose whole image is preselected. Those have no frame left to drag, so they open on the tool the config remembers. Scrolling capture stays unarmed for the same reason it always did — it has no annotation step for a tool to belong to. Out-of-range integers are clamped, and an unrecognized name falls back to the default, so a typo in a hand-edited file costs you that one setting. **`textPixels` is a pixel height**: type 14 and the label is 14 pixels tall, exactly matching the number in the editor's size box; the legacy "glyph multiple" scale the JSON protocol still carries (1–64, one cell being 7 pixels) is derived once, on the way out. Earlier versions called this key `textSize` and stored that multiple, and **an old file is migrated automatically** (`2` → 14 px, `3` → 21 px): the next save writes it as `textPixels` and drops the old key, and when both are present `textPixels` wins.

`selectMode` decides **what a press picks up once a mark is already selected**. `precise` (the default) needs the pointer on the mark itself; `loose` is "select first, then drag from anywhere" — with a mark selected, a press anywhere on screen moves it, so a one-pixel stroke never has to be aimed at twice. The cost is that the capture selection gives way to the mark while one is selected (its handles still win, being small deliberate targets): to drag the selection again, **click once on empty space** to let the mark go — a click only drops the selection, it never moves the frame, and everything is as it was after that.

### `cli` — command-line defaults
Supplies values for flags **not given on the command line**. The order is:

```
command line > environment > config file > built-in default
```

| Key | Flag | Built-in default |
| --- | --- | --- |
| `png-compression` | `--png-compression` | `fast` |
| `hdr-format` | `--hdr-format` | `avif` |
| `tone-map` | `--tone-map` | `auto` |
| `tone-map-white` | `--tone-map-white` | `0.8` |
| `hdr-area-test` | `--hdr-area-test` | `true` |
| `hdr-area-ratio` | `--hdr-area-ratio` | `0.0005` |
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
| `translate.provider` | `translate --provider` | `google` |
| `translate.from` | `translate --from` | `auto` |
| `translate.to` | `translate --to` | `zh-Hans` |
| `translate.timeout` | timeout of a translation HTTP request, in seconds | `20` |
| `translate.notify` | a desktop notification when a translation ends | `false` |
| `translate.fallback` | providers to try, in order, after the primary produces nothing (an array) | empty |
| `translate.bing.endpoint` / `api-key` / `region` | Bing (Azure Translator) endpoint and credentials | global endpoint / none / none |
| `translate.baidu.app-id` / `secret-key` | Baidu fanyi credentials | none |
| `translate.ai.endpoint` / `api-key` / `model` / `prompt` | OpenAI-compatible endpoint, credentials, model and system prompt (empty uses the built-in one) | none |
| `translate.external.command` / `timeout` | the external translation program (an array) and its timeout, in seconds | none / `30` |
| `translate.lingocloud.token` | the Caiyun (`lingocloud`) token; the built-in borrowed one is used when absent | the built-in borrowed token |

`google`, `microsoft`, `volcengine` and `transmart` are keyless and have no config keys at all — just name one in `translate.provider`. `lingocloud` needs no config either, and its only optional key is `translate.lingocloud.token` (absent uses the built-in borrowed token).

`pin.density` follows the same order: `--density` > `VSHOT_PIN_DENSITY` > the config file; unknown keys inside `cli` are ignored rather than making the whole file invalid. `ocr.engine` accepts only `builtin` and `external`, and any other name is an **error** rather than a default, because a misspelled `external` would otherwise look like a working GPU engine (`engine: "external"` with no `command`, or a command that will not run, is reported plainly too — see [Using a GPU](#using-a-gpu-the-external-engine)). The settings window covers `editor`, the common `cli` entries (the HDR format and the tone map among them), the `ocr.notify` switch, and both the `record` and `replay` sections, while `ocr.engine`, `ocr.external` and the whole `cli.translate` section are edited by hand (`translate.provider` accepts the nine names plus `auto`, and any other is an error). The two `notify` switches **only ever write "off"**, because an absent key already means on; `translate.notify` is the other way round — it is off by default, so it has to be written out to turn it on. The microphone rows are filled from the running session; the `follow` rows are **one comma-separated line of window names** in the window and an **array** in the file — hand-edit it as `["game", "chat"]`.

`record.follow` / `replay.follow` apply only to a **bare `record window` / `replay start window`** — no window NAME and no `--pick`; every other target (`monitor`, `all`, `region`, or a window named on the command line) **ignores** a remembered list rather than failing on it. `--no-follow` turns a remembered list off for one recording, the way `--no-mic` does a remembered microphone. `color` uses the CSS spelling: `#rrggbb`, or `#rrggbbaa` with the alpha **last** when it is not opaque — note that this differs from Qt's own eight-digit order (`#aarrggbb`); both `vshot settings` and the config file follow CSS.

### `dialog` — the file dialogs' look
The save and open windows (a pin's **Save as…**, pasting a local image in the editor) are **layer surfaces**, and a compositor draws no decoration on one — so the rim and the shadow this section describes are the only things separating them from whatever is behind.

| Key | Values | Default |
| --- | --- | --- |
| `radius` | 0–1000000, logical pixels | `12` |
| `borderWidth` | 0–1000000, logical pixels; 0 draws no rim at all | `1` |
| `borderColor` | `#rrggbb`; **leave it out** to derive one from the colour scheme | none |
| `shadow` | `true` / `false` | `true` |
| `shadowSize` | 0–512, logical pixels | `14` |
| `shadowOffset` | -512–512, logical pixels | `3` |
| `shadowOpacity` | 0–255 | `120` |

With no `borderColor` the rim is derived from the dialog's own colours — a stroke a little darker than the surface — so it reads as an edge under a light or a dark scheme, and one you write is used as it is. The shadow's four keys are **the same set the `pin` section has** (same meaning, same ranges and same defaults; see below). A layer surface can only be the size it asks the compositor for, so the shadow is painted in a ring the window keeps inside itself, and `shadowSize` is therefore also how much smaller the dialog is than its window — it is the ring's width, not something added outside it.

### `pin` — how a pinned image looks
A pin is a layer surface with nothing but the image in it, so its corners, the shadow behind it and the line around it are all vshot's to draw. This section is **not** the same thing as `cli.pin`: that one is a pin's *size* (its source density, a command-line default), this one is how it *looks*.

| Key | Values | Default |
| --- | --- | --- |
| `radius` | 0–1000000, logical pixels; 0 is a square corner | `0` |
| `shadow` | `true` / `false` | `true` |
| `shadowSize` | 0–512, logical pixels; 0 means no blur | `14` |
| `shadowOffset` | -512–512, logical pixels; negative lifts the shadow above | `3` |
| `shadowOpacity` | 0–255 | `120` |
| `borderWidth` | 0–1000000, logical pixels; 0 draws no border at all | `2` |
| `borderColor` | the border on an idle pin; the built-in light grey `#c0c0c0` when absent | none |
| `activeBorderColor` | the border on the pin the pointer is over while that output holds the keyboard; the built-in black when absent | none |

The default is **square corners with a shadow**: a screenshot is a picture of a window, and rounding it would cut into what it shows — while a screenshot pinned over a window of its own colour has no visible edge at all without one. The shadow's three numeric keys: `shadowSize` is how far the blur reaches past the edge, which is what its softness is; `shadowOffset` is how far the whole shadow is dropped — light comes from above, hence the default of 3, and a negative value puts it above the pin instead; `shadowOpacity` is the shadow's darkness, with the blur spreading it rather than adding to it; `shadow` is the master switch, and **turning it off keeps the numbers**, so it can be turned off for a moment and back on with the size still there. `radius` is a ceiling rather than a promise: what is actually painted is clamped to half the image's shorter side, past which a corner stops being a corner and becomes a lozenge — a pin's size changes with every wheel step, so that can only be decided at paint time. The border is **centred on the image edge**, half outside and half in, so changing its width leaves a rounded pin and a square one the same size. The corner radius and the border width have **no ceiling worth the name** — the million logical pixels in the table is a thousand screens, and neither of them costs an allocation — while the shadow's two numbers do, at 512: the blur is built over the shape grown by its reach, and the file dialog's own window grows by it too, so past that it is a hundred megabytes of haze for something off the screen. The shadow is built once and cached — a drag re-uses it, a zoom step or a config change rebuilds it. **A config change takes effect on the next `vshot pin`**: the daemon stays up while anything is pinned, but it re-reads the file every time a pin is added, so there is no need to restart it by hand.

### `shortcuts` — the keyboard bindings

Which key does what while a capture is on the screen. **Every one of these is also a toolbar button**, so a key that is in the way can be cleared outright (written as an empty string) rather than only moved elsewhere. Change them from the **Keyboard** page of `vshot settings`: clicking a row opens that action's key editor — a record button at the top, which you click and then press the key you want, and one row per key the action already answers to, each with a button that removes it. A hand-written string means guessing `QKeySequence`'s spelling, and a wrong guess is read as "this action has no key" rather than reported.

Recording a key **another action already holds** asks first whether to move it (the default answer is No, so a stray Enter does not take someone else's key away); saying yes takes the key off the action that had it and puts it on this one. Recording a key **this action already has** does nothing at all — that is the user repeating themselves, not a conflict, and a second row would only be a second remove button for one key. The editor's Cancel puts the whole binding back the way it opened.

**The two colour keys only live while the picker is up** — that is, the magnifier the **right button** holds. `copy-color` (`C`) and `adopt-color` (`V`) are actions *about a pixel*, and the loupe a drag brings up is a coordinate readout: what it is for is telling you where the cursor is, not what colour it is over. That is not only a matter of meaning: while the picker is up the right button is held, so the pointer is still the thing being moved and the cursor keys are exactly what the user needs — a letter the cursor walk also uses (`A` is `cursor-left`) would be shadowed by the picker for as long as it is up, which is the one moment it must not be. `V` is not a cursor key, so both keep working. The magnifier itself still comes up for **any drag** (except the ones that lay ink down), just without the picker on it.

**The picker follows the cursor while the right button is held**: it reads the pixel the cursor is over *now* rather than the one the button went down on, so dragging with it held changes the reading as it goes.

**The picker's readout is two lines.** The top line is the colour code, on a ground that **is the colour itself**, with the ink chosen between pure black and pure white by Rec. 601 luma so it reads on any ground (a fixed white would vanish on a white pixel, which is exactly the pixel a colour picker gets pointed at). The bottom line is the hint for the `C` and `V` keys on a fixed dark ground — it is a caption, and a caption that changed colour every time the cursor moved would only flicker.

| Key (the action's id) | Default | What it does |
| --- | --- | --- |
| `confirm` | `Return, Enter` | Accepts the capture and writes it out |
| `cancel` | `Esc` | Throws the capture away |
| `undo` | `Ctrl+Z` | Takes back the last change to the marks |
| `redo` | `Ctrl+Y, Ctrl+Shift+Z` | Puts the last undone change back |
| `copy` | `Ctrl+S` | Composites the capture and its marks onto the clipboard |
| `copy-text` | `Ctrl+C` | Copies the text the text-selection mode or a translation picked |
| `paste` | `Ctrl+V` | Pastes an image from the clipboard into the selection |
| `select-all` | `Ctrl+A` | Picks up every mark |
| `select-none` | `Ctrl+D` | Puts every mark down |
| `next-mark` | `Tab, Ctrl+Tab` | Moves the focus to the next mark |
| `previous-mark` | `Shift+Backtab, Ctrl+Shift+Backtab` | Moves it to the previous one |
| `delete` | `Del, Backspace` | Deletes the mark the focus is on |
| `copy-color` | `C` | While the magnifier is up: copies that pixel's colour code |
| `adopt-color` | `V` | While the magnifier is up: makes that colour the current tool's |
| `magnifier` | `M` | Shows the magnifier for two seconds, without dragging |
| `cursor-left` / `cursor-right` / `cursor-up` / `cursor-down` | `Left, A` / `Right, D` / `Up, W` / `Down, S` | Walks the cursor one pixel |

Three bindings are **read-only**. They are read from the state of the keyboard while another input is already under way rather than from a key event of their own, and `QKeySequence` has no spelling for "Alt on its own" — anything spellable as `Alt` would fire on Alt+F4 too — so they are listed so the user can see them, not offered for editing:

| Key | Default | What it does |
| --- | --- | --- |
| `preserve-aspect` | `Alt` | Hold while resizing: the shape keeps its proportions |
| `coarse-step` | `Shift` | Hold while walking the cursor: ten pixels at a time |
| `select-mark` | `Shift` | Hold to pick a mark up: move it, rather than drawing with the armed tool |

**Moving the selection or the pin** is done with the **middle mouse button**, not a keyboard modifier: the drag is itself a hold-and-travel gesture, and a modifier would mean holding a key down for the whole move while the left button did the work. Hold the middle button and drag the selection's body to move it; in the pin editor the same drag slides the image under the marks. It is not in the table above because a mouse button cannot be read from the keyboard's state at all.

With the Select tool gone, **picking an existing mark up** is done by holding `Shift`: a press on a mark while it is held moves that mark, and it **does not change which tool is armed** — letting go puts the user back exactly where they were, so nudging the stroke just drawn does not mean re-arming the pen. While `Shift` is held, the mark under the pointer wears a **dashed frame**, drawn the same way as the selected mark's: the pointer's shape says a drag is possible, not *which* mark would be taken, and a mark is an area rather than a point, so something has to point at it. Without it held, a press on a mark's *body* still inks with the armed tool (a pen has to be able to start a stroke on top of an existing mark, or it stops working wherever the picture is busiest), but **a press on a mark's border always drags**, and **stretching always goes through the eight handles on that border**: they are small, deliberate targets that can only mean one thing, so they answer whatever is held and whatever is armed.

Walking the cursor with the arrow keys **moves the pointer on the screen with it**. The cursor is a step of the editor's own — the loupe and the next press both read it — but the arrow the compositor paints is a different thing, and only the CLI can move that one (the injection backends are the CLI's: the compositor's virtual-pointer protocol, the portal, `/dev/uinput`). So the editor writes where the cursor went as one request on the pipe the session arrived on, and the CLI turns it into a pointer move. The requests are **throttled but coalescing**: a held key repeats far faster than a round trip through another process and a compositor, so steps inside the interval are held back and only the newest is sent when it is up (the position is absolute, so an older one has nowhere to land) — dropping them instead would mean tapping right five times quickly moves the pointer one pixel. On its side the CLI **moves the pointer only when the compositor offers the virtual-pointer protocol**, with no fallback to the portal or `/dev/uinput`: the portal puts up a permission dialog, and tapping an arrow key should not raise anything. There the cursor still walks and the loupe still says where it is; only the arrow on screen stays put. Both the region editor and the **pin editor** go this way, and the position is always in **global logical pixels**; a pin-edit session describes only the pin's own screen, so the daemon writes the layout's bounding box (`desktop`) into it for the CLI to convert against — with no such field the pointer is simply not moved, because a position converted against the pin's rect is the wrong position.

**The walk also reaches a stroke that is in progress.** The tools whose press picks a start and whose release picks an end — the rectangle, the ellipse, the area mosaic, the arrow, the line, the wave, the freehand pen and the brush — hold the left button for the whole of the drag, so the mouse is the one thing that cannot place the far end exactly. There the keys move the **live gesture** rather than a committed mark: the anchor the press set stays where it is and the far end follows the cursor, one pixel (or ten, with the step modifier held) at a time. The pen path is the same edit with a different meaning — its last anchor has already been placed, so the keys pull that anchor's outgoing handle out instead.

**A walk's own echo does not put the magnifier out.** The warp above makes the compositor report a pointer motion, and that report arrives *after* the step that asked for it; read as the user moving the mouse it would end the two-second flash the step had just raised — so the loupe would blink on every step, and whether it survived the last one would be a race. The echo is recognised by where it lands: a warp puts the pointer exactly where the walk put its cursor, so the motion arrives on that very pixel, while a hand on the mouse covers a pixel or more. Within a short window after the request, a motion landing exactly on the warp's target is treated as the echo — it does not take the cursor back from the keyboard and does not end the flash. Any other motion still does both, so the flash cannot follow the pointer around and stay up forever.

One action can hold **several spellings**, comma-separated (`"Del, Backspace"`), any of which fires it. **Clearing** one is written as `""` — which is not the same as leaving the key out: `""` is "this action has no key", leaving it out is "follow the built-in default", so a later version that moves a default reaches anyone who never chose one. A spelling that cannot be read **falls back to the built-in default** rather than leaving the action unreachable. Only bindings that **differ from the built-in default** are written to the file.

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
| `VSHOT_PIN_SOCKET` | The socket the pin daemon listens on |
| `VSHOT_HDR_HELPER` | Path to the `vshot` that runs as `vshot --pin-hdr-server`, the helper process that draws the pin stack (otherwise inferred from where `vshot-qt-ui` lives) |
| `VSHOT_PIN_DENSITY=N` | The source density of every pinned image, same as `--density` |
| `VSHOT_PIN_DEBUG=1` | The daemon prints every pin's density decision |
| `VSHOT_PIN_FOCUS_DEBUG=1` | The daemon prints every focus change of every pin render surface |
| `VSHOT_PIN_SOURCE_FILE` | Overrides the screenshot tool's record path (default `/tmp/screenshot-path`) |
| `VSHOT_ANNOTATE_SOCKET` | The socket the annotation daemon listens on (default `$XDG_RUNTIME_DIR/vshot-annotate-<uid>.sock`) |
| `VSHOT_ANNOTATE_DEBUG=1` | Keep the annotation daemon's diagnostics on stderr |
| `VSHOT_RECORD_PIDFILE` | The pid file `vshot record stop` reads (default `$XDG_RUNTIME_DIR/vshot-record-<uid>.pid`) |
| `VSHOT_RECORD_DEBUG=1` | The recording/replay loop traces each frame's stage (grab/encode/mux) and the libavcodec version in use |
| `VSHOT_PORTAL_SHM=1` | `record --portal` asks for memory frames instead of dma-bufs (the fallback when a compositor's buffers cannot be imported) |
| `VSHOT_REPLAY_SOCKET` | The replay control socket path (default `$XDG_RUNTIME_DIR/vshot-replay-<uid>.sock`) |
| `VSHOT_REPLAY_PIDFILE` | The pid file `vshot replay stop` reads (default `$XDG_RUNTIME_DIR/vshot-replay-<uid>.pid`) |

> As soon as any `VSHOT_PIN_*_DEBUG` is set for the daemon, it stops sending its stderr to `/dev/null`, so the traces are readable. The variables must be in place when the daemon starts; for one already resident, run `vshot pin --quit` first.

## Known limitations
- **KWin's frozen overlay still depends on `zwlr_layer_shell_v1`** (which KWin provides). Authorization is not a dialog and only recognizes desktop files, so a build run straight from `target/` will always get `NoAuthorized` under Plasma;
- **Several compositors can coexist under one `XDG_RUNTIME_DIR`**, and when `WAYLAND_DISPLAY` is unset libwayland uses the default `wayland-0`, so a command may connect to "the other compositor". For every compositor-related failure vshot additionally prints which display it connected to and which displays exist;
- **Long screenshots**: the selection must lie entirely within one screen; a fixed bar that only appears at some scroll positions (a floating toolbar) is still treated as page content, so exclude it with `--ignore-top N`; a lazily loading page may duplicate or miss a few rows; the frame rate on KDE is noticeably lower than on other compositors; the time for grabbing and aligning grows linearly with the region's area, so **use a release build for long screenshots**;
- **Pixel detection**: a seamless borderless tiling layout and a completely uniform desktop have no pixel signal, and vshot reports that honestly instead of guessing; when the compositor draws no colored border around the focused window, `window active --pixel` answers with "the window under the pointer";
- **Text**: the Qt text box accepts arbitrary Unicode (including CJK submitted by an input method); a result from an old helper that carries no bitmap falls back to Rust's built-in 5x7 font, which only supports printable ASCII;
- **`monitor current`** depends on receiving pointer enter/motion on the overlay; generic Wayland has no readable global mouse position, so vshot never guesses with the first output;
- **Replay**: `replay start --portal` is not supported yet (the portal's frame loop is not wired to the ring, and it refuses with a sentence saying so); one session runs at a time;
- **Screen annotation**: the toolbar is **not** hidden automatically while `vshot replay start` runs (a replay grabs continuously, and hiding it for the whole session would make the overlay unusable), so annotate and replay are not meant to run together; like the pin overlay it needs `wlr-layer-shell`, so a compositor without layer shell cannot draw it; verified on **Hyprland** only, other desktops untested.
- Native screencopy waits up to 10 seconds for the compositor to return a frame, and times out with an error instead of blocking forever.

## Verification
```sh
cargo fmt --check
cargo test --locked
cargo clippy --locked --all-targets --all-features -- -D warnings
cargo build --release --locked
```

The Qt helper has no test framework, only **offscreen checks that need no compositor** (not built by default; add `-DVSHOT_BUILD_CHECKS=ON`), covering config reads and writes with the settings window, the text size conversion, clipboard color parsing and color card rendering, the file dialog's stylesheet and thumbnail grid, a pin's self-declared density and outline, text card padding, the color card's right-click menu, the export format of a pasted image, the annotation overlay's five tools with undo, clear and the toolbar's placement, whether the overlay leaves the frozen frame to a backdrop, where the toolbar lands, the annotation render cache being hit, the resolution that cache is built at on a high-DPI screen, and the text layer — which parses the JSON `vshot ocr --json` prints (so the wire format has one end in `src/ocr.rs` and one on the Qt side) and, building no widget, needs no `QT_QPA_PLATFORM`:

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
QT_QPA_PLATFORM=offscreen build-qt/vshot-file-dialog-check
QT_QPA_PLATFORM=offscreen build-qt/vshot-annotate-check
QT_QPA_PLATFORM=offscreen build-qt/vshot-backdrop-check
QT_QPA_PLATFORM=offscreen build-qt/vshot-toolbar-check
QT_QPA_PLATFORM=offscreen build-qt/vshot-annotation-check
QT_QPA_PLATFORM=offscreen build-qt/vshot-dpr-check
build-qt/vshot-text-layer-check
```

There are also 5 integration tests that are **not run** by default (`#[ignore]`), needing a real environment: KWin's D-Bus capture and backend selection (see the comments in `src/capture/kwin.rs`; a headless KWin suffices — virtual output named `Virtual-0`, 1024x768, no pointer capability — so it only covers the D-Bus capture layer), the active output probe (needs any real session), `/dev/uinput` scroll injection (needs write access), and **the built-in OCR engine reading drawn text** (needs those 30 MB of models on disk, which `cargo test` has nowhere to fetch them from). To run them:

```sh
cargo test -- --ignored              # all of them
cargo test --release ocr:: -- --ignored --nocapture   # just the OCR one
```

The OCR test looks for the models in the source tree's `models/` by default, and `VSHOT_OCR_MODELS=<dir>` points it elsewhere. Starting a headless KWin:

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
