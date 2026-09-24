// The recording C shims: `shim.c` (the libavcodec encoder and the MP4 muxer)
// and, where libpipewire's headers are installed, `pipewire_client.c` (the
// portal's screen-cast stream).  Both libraries are loaded with dlopen inside
// the C code, so the link here is against the small `vshot_av` archive alone —
// no libavcodec or libpipewire symbols in the binary's dynamic table.

fn main() {
    println!("cargo:rerun-if-changed=src/record/shim.c");
    println!("cargo:rerun-if-changed=src/record/pipewire_client.c");
    // `cfg` names a build script chooses have to be declared, or rustc reports
    // every use of them as a typo.
    println!("cargo:rustc-check-cfg=cfg(vshot_pipewire)");
    let mut build = cc::Build::new();
    build.file("src/record/shim.c");
    // The headers come from the system ffmpeg; a build machine without them
    // cannot compile the shim, which is the same class of dependency as the
    // system ONNX Runtime the OCR side already needs.  libavfilter and
    // libavformat are only header dependencies: the shim loads the libraries
    // itself with dlopen, and their absence is reported at runtime by the
    // paths that need them (the zero-copy path and the MP4 muxer
    // respectively).
    for flag in pkg_config_cflags(&["libavcodec", "libavutil", "libavfilter"]) {
        build.flag(flag);
    }
    // The portal's PipeWire client is the one C file that is optional: it is
    // compiled only where libpipewire's headers are installed, and its absence
    // costs nothing else — the compositor's own protocols still record, and
    // `record --portal` reports the missing package instead of the build
    // failing for everybody.
    if pkg_config_present("libpipewire-0.3") {
        println!("cargo:rustc-cfg=vshot_pipewire");
        build.file("src/record/pipewire_client.c");
        for flag in pkg_config_cflags(&["libpipewire-0.3"]) {
            build.flag(flag);
        }
    } else {
        println!(
            "cargo:warning=libpipewire-0.3 has no pkg-config entry; this build leaves out \
             `record --portal`"
        );
    }
    build.warnings(true);
    build.compile("vshot_av");

    // dlopen lives in libc on musl and libdl on glibc; linking `dl` covers
    // both without pulling in libavcodec itself.
    println!("cargo:rustc-link-lib=dl");
}

/// The compiler flags pkg-config reports for `packages`, verbatim: the include
/// directories mostly, but `-D_REENTRANT` and its neighbours matter to the
/// headers that check them.
fn pkg_config_cflags(packages: &[&str]) -> Vec<String> {
    let output = std::process::Command::new("pkg-config")
        .arg("--cflags")
        .args(packages)
        .output();
    match output {
        Ok(output) if output.status.success() => String::from_utf8_lossy(&output.stdout)
            .split_whitespace()
            .map(str::to_string)
            .collect(),
        _ => Vec::new(),
    }
}

/// Whether pkg-config knows `package` at all.
fn pkg_config_present(package: &str) -> bool {
    std::process::Command::new("pkg-config")
        .arg("--exists")
        .arg(package)
        .status()
        .map(|status| status.success())
        .unwrap_or(false)
}
