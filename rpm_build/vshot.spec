# SPDX-License-Identifier: GPL-3.0-or-later
%global forgeurl https://github.com/busyoGG/VShot
%bcond check 1

Version:    0.1.10
%forgemeta

Name:       vshot
Release:    1%{?dist}
Summary:    A shortcut tool
# GPL-3.0-or-later: the recording shim loads Fedora's GPL-3.0 build of FFmpeg components
# at run time, and the Qt / LayerShellQt UI is used under those libraries' GPL options.
# See NOTICE for the third-party notices; LICENSE carries the full GPLv3 text.
License:    GPL-3.0-or-later
URL:        %{forgeurl}
Source0:    %{forgesource}

# PaddleOCR's PP-OCRv6 models, converted to ONNX by RapidOCR, plus the
# character dictionary oar-ocr reads them with.  They are downloaded rather
# than committed: 30 MB of weights do not belong in the source tree, and the
# package installs them under /usr/share/vshot/models where the binary looks.
%global modelbase https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.9.2/onnx/PP-OCRv6
Source1:    %{modelbase}/det/PP-OCRv6_det_small.onnx
Source2:    %{modelbase}/rec/PP-OCRv6_rec_small.onnx
Source3:    https://github.com/GreatV/oar-ocr/releases/download/v0.7.0/ppocrv6_dict.txt

BuildRequires:  cmake gcc gcc-c++
BuildRequires:  cargo-rpm-macros >= 24
# `libpipewire` is the same shape for `record --portal`: the portal's screen
# cast arrives on a PipeWire stream, and the client for it is compiled from
# libpipewire's headers while the library itself is dlopen'ed, so the build
# needs the package and the run time does not.
BuildRequires:  pipewire-devel
# Similar to the Arch build, `onnxruntime` on Fedora has a generic variant and
# a ROCm variant. Using or here allows for the generic one to be preferred while
# retaining the possibility of using the specialized version.
# What is actually used is only the shared library and its `.pc` file, which
# both variant ships: `ort-sys` finds it through pkg-config and links it
# dynamically.  The provider it offers is not selected by anything here, so a
# GPU build runs the OCR on the CPU exactly like the CPU one does.
BuildRequires:  (onnxruntime-devel or onnxruntime-rocm-devel)
BuildRequires:  qt6-qtbase-devel layer-shell-qt-devel
# `ffmpeg` provides the libavcodec/libavformat headers the recording shim is
# compiled against; at run time both libraries are dlopen'ed, so ffmpeg stays
# an optdepend and a machine without it simply has no `vshot record`.
# ffmpeg-devel is only packaged in RPM Fusion.
# Fedora's main repo packages the free variant of each component separately.
BuildRequires:  (ffmpeg-devel or (libavcodec-free-devel and libavfilter-free-devel))
# Same with the -devel packages.
Requires:   (onnxruntime or onnxruntime-rocm)
# Acquired by `ldd`-ing the built binaries. Direct Qt libs are omitted since
# layer-shell-qt requires them as well.
Requires:   layer-shell-qt

%description
Strict-freeze Wayland screenshot CLI with Qt interactive overlay and pin server

%prep
%forgeautosetup -p1
%cargo_prep

%generate_buildrequires
%cargo_generate_buildrequires -t

%build
%cargo_build
%{cargo_license_summary}
%{cargo_license} > LICENSE.dependencies
cmake -S . -B build-qt \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build-qt --parallel

%check
%cargo_test

%install
%{__install} -Dpm755 target/rpm/vshot -t %{buildroot}%{_bindir}
%{__install} -Dpm755 build-qt/vshot-qt-ui -t %{buildroot}%{_bindir}
# The OCR models, under the names `src/ocr.rs` looks for.
%{__install} -Dpm644 %{S:1} %{buildroot}%{_datadir}/%{name}/models/det.onnx
%{__install} -Dpm644 %{S:2} %{buildroot}%{_datadir}/%{name}/models/rec.onnx
%{__install} -Dpm644 %{S:3} %{buildroot}%{_datadir}/%{name}/models/dict.txt
# Authorizes the CLI for KWin's restricted ScreenShot2 D-Bus interface.
%{__install} -Dpm644 vshot.desktop %{buildroot}%{_datadir}/applications/%{name}.desktop
# The launcher entry the application menu shows: it opens the settings
# window, which is the one thing vshot can do from a menu (a capture needs
# a destination, so there is nothing sensible to launch bare).
%{__install} -Dpm644 vshot-settings.desktop %{buildroot}%{_datadir}/applications/%{name}-settings.desktop
# One scalable icon rather than a size ladder: the shells that read it
# (KDE, GNOME, wlroots launchers) render SVG themselves and ask for the
# size they need, so a ladder would only add files to keep in step.
%{__install} -Dpm644 icons/vshot.svg %{buildroot}%{_datadir}/icons/hicolor/scalable/apps/%{name}.svg

%files
%{_bindir}/%{name}
%{_bindir}/%{name}-qt-ui
%{_datadir}/%{name}/models/det.onnx
%{_datadir}/%{name}/models/rec.onnx
%{_datadir}/%{name}/models/dict.txt
%{_datadir}/applications/%{name}.desktop
%{_datadir}/applications/%{name}-settings.desktop
%{_datadir}/icons/hicolor/scalable/apps/%{name}.svg
%license LICENSE
%license LICENSE.dependencies
# The third-party notices: Qt / LayerShellQt under their GPL options,
# FFmpeg, and the model licenses.  GPLv3 section 6 wants the license texts
# to travel with the binaries, and this is the file that says which
# third-party works they cover.
%license NOTICE

%changelog
%autochangelog
