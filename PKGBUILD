# Maintainer: VShot contributors
# SPDX-License-Identifier: GPL-3.0-or-later
pkgname=vshot
pkgver=0.1.3
pkgrel=4
pkgdesc='Strict-freeze Wayland screenshot CLI with Qt interactive overlay and pin server'
url='https://github.com/busyoGG/VShot'
arch=('x86_64')
# GPL-3.0-or-later: the recording shim loads Arch's GPL-3.0 FFmpeg build at run
# time, and the Qt / LayerShellQt UI is used under those libraries' GPL options.
# See NOTICE for the third-party notices; LICENSE carries the full GPLv3 text.
license=('GPL-3.0-or-later')
# `onnxruntime` is a virtual provide: all six Arch variants (cpu, cuda,
# opt-cuda, rocm, opt-rocm) declare `Provides: onnxruntime` and conflict with
# each other, so a system has exactly one.  Naming the virtual package rather
# than `onnxruntime-cpu` means a user who already has a GPU build installed
# satisfies this without pacman tearing that build -- and the rccl, migraphx
# and rocm-hip-sdk packages behind it -- back out.  A fresh install gets to
# pick; the CPU build is the one the README recommends, because it is about
# 46 MB against well over a gigabyte.
#
# What is actually used is only the shared library and its `.pc` file, which
# every variant ships: `ort-sys` finds it through pkg-config and links it
# dynamically.  The provider it offers is not selected by anything here, so a
# GPU build runs the OCR on the CPU exactly like the CPU one does.
depends=('glibc' 'wayland' 'qt6-base' 'layer-shell-qt' 'onnxruntime')
# `ffmpeg` provides the libavcodec/libavformat headers the recording shim is
# compiled against; at run time both libraries are dlopen'ed, so ffmpeg stays
# an optdepend and a machine without it simply has no `vshot record`.
#
# `libpipewire` is the same shape for `record --portal`: the portal's screen
# cast arrives on a PipeWire stream, and the client for it is compiled from
# libpipewire's headers while the library itself is dlopen'ed, so the build
# needs the package and the run time does not.
makedepends=('rust' 'cargo' 'cmake' 'gcc' 'pkgconf' 'ffmpeg' 'libpipewire')
optdepends=('wl-clipboard: clipboard input and output support'
            'ffmpeg: screen recording (vshot record)'
            'libpipewire: screen recording through the desktop portal, and microphone audio (vshot record --portal / --mic)')
# PaddleOCR's PP-OCRv6 models, converted to ONNX by RapidOCR, plus the
# character dictionary oar-ocr reads them with.  They are downloaded rather
# than committed: 30 MB of weights do not belong in the source tree, and the
# package installs them under /usr/share/vshot/models where the binary looks.
_model_base='https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.9.2/onnx/PP-OCRv6'
source=(
    "vshot-det.onnx::$_model_base/det/PP-OCRv6_det_small.onnx"
    "vshot-rec.onnx::$_model_base/rec/PP-OCRv6_rec_small.onnx"
    "vshot-dict.txt::https://github.com/GreatV/oar-ocr/releases/download/v0.7.0/ppocrv6_dict.txt"
)
sha256sums=('090f04abcd9d9a7498bc4ebf677e4cb9bdce1fe4197ddb7e529f1ef44e1ff94f'
            '6f327246b50388f3c176ae304bd95767ea6dc0c9ae92153ef8cbe210b3c14884'
            'b5f2bfe2bdd9448429e3e82b51c789775d9b42f2403d082b00662eb77e401c5d')

build() {
    cd "$startdir"
    # `pkg-config` is how ort-sys finds the system ONNX Runtime; without it the
    # crate tries to download one of its own.
    cargo build --release --locked
    cmake -S . -B build-qt \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr
    cmake --build build-qt --parallel
}

package() {
    cd "$startdir"
    install -Dm755 target/release/vshot "$pkgdir/usr/bin/vshot"
    install -Dm755 build-qt/vshot-qt-ui "$pkgdir/usr/bin/vshot-qt-ui"
    # The OCR models, under the names `src/ocr.rs` looks for.
    install -Dm644 "$srcdir/vshot-det.onnx" "$pkgdir/usr/share/vshot/models/det.onnx"
    install -Dm644 "$srcdir/vshot-rec.onnx" "$pkgdir/usr/share/vshot/models/rec.onnx"
    install -Dm644 "$srcdir/vshot-dict.txt" "$pkgdir/usr/share/vshot/models/dict.txt"
    # Authorizes the CLI for KWin's restricted ScreenShot2 D-Bus interface.
    install -Dm644 "$startdir/vshot.desktop" "$pkgdir/usr/share/applications/vshot.desktop"
    # The launcher entry the application menu shows: it opens the settings
    # window, which is the one thing vshot can do from a menu (a capture needs
    # a destination, so there is nothing sensible to launch bare).
    install -Dm644 "$startdir/vshot-settings.desktop" \
        "$pkgdir/usr/share/applications/vshot-settings.desktop"
    # One scalable icon rather than a size ladder: the shells that read it
    # (KDE, GNOME, wlroots launchers) render SVG themselves and ask for the
    # size they need, so a ladder would only add files to keep in step.
    install -Dm644 "$startdir/icons/vshot.svg" \
        "$pkgdir/usr/share/icons/hicolor/scalable/apps/vshot.svg"
    install -Dm644 "$startdir/LICENSE" "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
    # The third-party notices: Qt / LayerShellQt under their GPL options,
    # FFmpeg, and the model licenses.  GPLv3 section 6 wants the license texts
    # to travel with the binaries, and this is the file that says which
    # third-party works they cover.
    install -Dm644 "$startdir/NOTICE" "$pkgdir/usr/share/licenses/$pkgname/NOTICE"
}
