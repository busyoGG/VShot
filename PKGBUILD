# Maintainer: VShot contributors
pkgname=vshot
pkgver=0.1.1
pkgrel=1
pkgdesc='Strict-freeze Wayland screenshot CLI with Qt interactive overlay and pin server'
url='https://github.com/busyoGG/VShot'
arch=('x86_64')
license=('MIT')
depends=('glibc' 'wayland' 'qt6-base' 'layer-shell-qt')
makedepends=('rust' 'cargo' 'cmake' 'gcc')
optdepends=('wl-clipboard: clipboard input and output support')
source=()
sha256sums=()

build() {
    cd "$startdir"
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
}
