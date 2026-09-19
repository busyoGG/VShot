# vshot

[中文](README.md) | **English**

> **This project is pure vibe coding**: requirements come from a human, code and docs are written by AI.

A Wayland screenshot tool written in Rust, with a Qt interactive UI and a resident pin overlay. Captures are **strictly frozen**: the desktop is photographed into a still frame first, and every selection and annotation happens on that frame, so nothing on screen moves while you are choosing.

Works with Hyprland, niri, KWin/Plasma, Sway, and basic capture on any compositor providing `wlr-screencopy` (such as labwc).

> **Verification varies**: every Hyprland feature was tested live; niri's tiled and floating window capture paths were both verified on a real session; KWin/Plasma's D-Bus capture, window list, and long screenshots were tested, but `--cursor` and scroll injection were not; Sway and labwc are **untested on a live session**. See "Compositor support and verification status".

## Features

- **Region capture** — drag on the frozen desktop, or give a fixed geometry; eight resize handles, with a magnifier and size readout following the pointer
- **Annotation editing** — rectangles, ellipses, arrows, freehand, text, mosaic; undo/redo, move and resize a selection, colors and line styles, arrow head styles, font sizes, system font picker
- **monitor / all** — capture one output by name or by pointer position, or compose the whole desktop by logical position
- **window active / pick** — the focused window, or click one on the live desktop; KWin and niri hand over the window's own pixels
- **Long screenshot** — frame a scrolling region, and vshot sends the wheel, grabs frames, aligns them by content, and stitches one long image
- **Pin overlay** — pin images or clipboard content to the screen: drag, wheel to zoom, double-click to close, one-key show/hide, Space to annotate
- **Clipboard pinning** — colors, images, copied image files, plain text (rendered as a card as HTML / markdown / code / plain text)
- **Output targets** — file (with strftime paths), stdout, clipboard, or an on-screen pin; exactly one
- **Bilingual UI** — the interface and `--help` follow the system language

## Install (Arch Linux)

`PKGBUILD` builds the Rust CLI and the Qt helper into one package, so a single install provides `/usr/bin/vshot`, `/usr/bin/vshot-qt-ui`, and the `/usr/share/applications/vshot.desktop` that authorizes KWin (see "KDE authorization"):

```sh
./scripts/build-arch-package.sh
sudo pacman -U dist/vshot-0.1.0-1-x86_64.pkg.tar.zst
```

The script snapshots the current working tree (uncommitted changes included) into a temporary directory and runs `makepkg`, writing the result to `dist/`; `makepkg -si` works directly too. Runtime dependencies are `glibc`, `wayland` (uses `libwayland-client` through dlopen), `qt6-base`, and `layer-shell-qt`; file output, `--clipboard`, and `vshot pin --clipboard` need the optional `wl-clipboard` (writes via `wl-copy`, reads via `wl-paste`). For other distributions, build from source as below.

## Build

```sh
cargo build --release --locked                                # Rust CLI
cmake -S . -B build-qt -DCMAKE_BUILD_TYPE=Release             # Qt helper
cmake --build build-qt --parallel
```

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
```

The global options apply to every capture:

| Option | Meaning |
| --- | --- |
| `-o, --output PATH` | Write the PNG to PATH, expanding strftime formats like `%Y%m%d`; afterwards the file's `file://` URI is copied to the clipboard. `-` writes to stdout without copying |
| `--clipboard` | Copy the PNG data to the clipboard |
| `--pin` | Pin the image to the screen instead of writing it (the daemon deletes the temporary file once it is in memory) |
| `-c, --cursor` | Ask the compositor to draw the cursor into every output frame. **Not supported by `long`** (see "Known rough edges"); for the most common reason a capture has no cursor, see "The cursor (`--cursor`)" |
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
- Style entries: a color palette (with a custom picker: HSV gradient plus hex input), line style Solid/Dash/Dot, arrow head Open V/Filled, thickness 1-64, arrow size 1-8, font size 1-64, mosaic shape Rect/Ellip/Brush, mosaic strength 1-3, and a system font list (each entry previewed in its own glyphs)
- Arrow draws a straight arrow from press to release; Draw is freehand; the mosaic strength controls both the pixel block size and the brush radius, and selecting an existing mosaic lets you change the strength directly
- The **Select** tool picks any annotation: click to select, drag to move (text too), shapes/lines/mosaics resize by their handles, Delete/Backspace removes it; style changes apply to the selected annotation immediately, and double-clicking text reopens it for editing
- **Ctrl+Z / Ctrl+Y** (or Ctrl+Shift+Z) undo/redo
- Annotations come back to Rust in global logical coordinates and the final PNG is redrawn by the built-in software renderer, matching the preview; text is rasterized by Qt in the chosen font and composited as a bitmap, so the glyphs are identical

The UI language follows the system by default (`QLocale::system()`) and can be overridden with `VSHOT_LANG`: a value starting with `zh` selects Chinese, any other non-empty value selects English. The language is fixed when the helper starts, so switching needs a rerun. The Rust CLI's `--help` uses the same rule, so `VSHOT_LANG=zh vshot --help` is Chinese.

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

## Pin overlay

`vshot pin` pins images to the screen as overlays, held by a **resident daemon**:

- **Drag** to move (across monitors; the copy on the other screen follows)
- **Wheel** zooms around the image center (0.1x–8x), with the factor briefly shown at the image's bottom-right
- **Double-click** closes that image
- **Left click** raises the image to the front, so a clicked one is always above the rest when they overlap
- Whichever image the pointer rests on gets a solid black outline; the others are light gray (2 logical pixels thick, not covering the image itself)
- With the pointer over a pin and that screen holding the keyboard, press **Space** to enter the same annotation editor `vshot region` uses
- Right-clicking a pinned **color card** opens a format menu; clicking an entry copies that value back to the clipboard (↑/↓ to move, Enter to copy, Esc to close; a badge flashes at the bottom-right once copied)

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
- The CLI is a thin client sending one-line JSON requests over a Unix socket; the socket defaults to `$XDG_RUNTIME_DIR/vshot-pin-<uid>.sock` and can be overridden with `VSHOT_PIN_SOCKET`;
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
- **pin daemon and screencopy** — do not end the daemon with `pkill`/`kill -9` (see "pin daemon"), or some compositors leave the layer surface and its screencopy session behind, making screencopy block forever on every output.

## Environment variables

| Variable | Purpose |
| --- | --- |
| `VSHOT_LANG` | UI and `--help` language: a value starting with `zh` selects Chinese, any other non-empty value selects English, unset follows the system |
| `VSHOT_QT_HELPER` | Path to `vshot-qt-ui` |
| `VSHOT_PIXEL_DEBUG=1` | What each level of window pixel detection saw |
| `VSHOT_SESSION_DEBUG=1` | Which compositor this session was judged to be, and on what basis |
| `VSHOT_LONG_DEBUG_DIR=<dir>` | Write every long-screenshot frame and every stitch decision to disk |
| `VSHOT_PIN_SOCKET` | The socket path the pin daemon listens on |
| `VSHOT_PIN_DENSITY=N` | The source density of every pinned image, same as `--density` |
| `VSHOT_PIN_DEBUG=1` | The daemon prints every pin's density decision |
| `VSHOT_PIN_FOCUS_DEBUG=1` | The daemon prints every focus change of every pin render surface |
| `VSHOT_PIN_SOURCE_FILE` | Overrides the screenshot tool's record path (default `/tmp/screenshot-path`) |

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

The Qt helper has no test framework, only **offscreen checks that need no compositor** (not built by default; add `-DVSHOT_BUILD_CHECKS=ON`), covering clipboard color parsing and color card rendering, a pin's self-declared density, pin outlines, text card padding, and the color card's right-click menu:

```sh
cmake -S . -B build-qt -DVSHOT_BUILD_CHECKS=ON && cmake --build build-qt
QT_QPA_PLATFORM=offscreen build-qt/vshot-color-check
build-qt/vshot-pin-density-check
QT_QPA_PLATFORM=offscreen build-qt/vshot-pin-outline-check
QT_QPA_PLATFORM=offscreen build-qt/vshot-text-card-check
QT_QPA_PLATFORM=offscreen build-qt/vshot-pin-menu-check
```

There are also 4 integration tests that are **not run** by default (`#[ignore]`), needing a real environment: KWin's D-Bus capture and backend selection (needs a running KWin; a headless KWin suffices, see the comments in `src/capture/kwin.rs`, with a virtual output named `Virtual-0`, 1024x768, no pointer capability, so it only covers the D-Bus capture layer), the active output probe (needs any real session), and `/dev/uinput` scroll injection (needs write access). To run them:

```sh
cargo test -- --ignored              # all of them
```

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
