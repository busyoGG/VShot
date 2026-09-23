// The recording encoder's C shim: builds `shim.c` against the system
// ffmpeg headers and links the Rust side to it.  libavcodec itself is
// loaded with dlopen inside the shim, so the link here is against the
// small `vshot_av` archive alone — no libavcodec symbols in the binary's
// dynamic table.

fn main() {
    println!("cargo:rerun-if-changed=src/record/shim.c");
    let mut build = cc::Build::new();
    build.file("src/record/shim.c");
    // The headers come from the system ffmpeg; a build machine without
    // them cannot compile the shim, which is the same class of dependency
    // as the system ONNX Runtime the OCR side already needs.  libavfilter
    // and libavformat are only header dependencies: the shim loads the
    // libraries itself with dlopen, and their absence is reported at
    // runtime by the paths that need them (the zero-copy path and the MP4
    // muxer respectively).
    let probe = pkg_config_include();
    for path in probe {
        build.include(path);
    }
    build.warnings(true);
    build.compile("vshot_av");

    // dlopen lives in libc on musl and libdl on glibc; linking `dl` covers
    // both without pulling in libavcodec itself.
    println!("cargo:rustc-link-lib=dl");
}

/// The include directories pkg-config reports for libavcodec, read from a
/// pkg-config invocation.  A missing pkg-config or missing libavcodec is
/// not fatal here: the compile step then fails with the compiler's own
/// message about the missing headers, which names the package to install.
fn pkg_config_include() -> Vec<String> {
    let mut flags = Vec::new();
    let output = std::process::Command::new("pkg-config")
        .args(["--cflags-only-I", "libavcodec", "libavutil", "libavfilter"])
        .output();
    if let Ok(output) = output {
        if output.status.success() {
            for token in String::from_utf8_lossy(&output.stdout).split_whitespace() {
                if let Some(path) = token.strip_prefix("-I") {
                    flags.push(path.to_string());
                }
            }
        }
    }
    flags
}
