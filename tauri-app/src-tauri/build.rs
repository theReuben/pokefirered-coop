use std::path::PathBuf;
use std::process::Command;

fn main() {
    tauri_build::build();

    if std::env::var("CARGO_FEATURE_MGBA").is_ok() {
        build_mgba();
    }
}

fn build_mgba() {
    let manifest = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap());
    let mgba_src = manifest.join("mgba");
    let mgba_build = out.join("mgba-cmake");

    // ── cmake configure ───────────────────────────────────────────────────────
    std::fs::create_dir_all(&mgba_build).unwrap();
    let ok = Command::new("cmake")
        .current_dir(&mgba_build)
        .args([
            mgba_src.to_str().unwrap(),
            "-DBUILD_SHARED=OFF",
            "-DBUILD_STATIC=ON",
            "-DBUILD_SDL=OFF",
            "-DBUILD_QT=OFF",
            "-DBUILD_LIBRETRO=OFF",
            "-DBUILD_PERF=OFF",
            "-DBUILD_TEST=OFF",
            "-DBUILD_EXAMPLE=OFF",
            "-DCMAKE_BUILD_TYPE=Release",
            // Disable optional features that pull in extra deps / bloat structs.
            "-DBUILD_LTO=OFF",
            "-DUSE_EDITLINE=OFF",
            "-DUSE_FREETYPE=OFF",
            "-DUSE_SQLITE3=OFF",
            "-DUSE_DISCORD_RPC=OFF",
            "-DUSE_FFMPEG=OFF",
            "-DUSE_LIBZIP=OFF",
            "-DUSE_LUA=OFF",
            "-DENABLE_SCRIPTING=OFF",
            "-DENABLE_DEBUGGERS=OFF",
            "-DENABLE_GDB_STUB=OFF",
        ])
        .status()
        .expect("cmake not found — install with: brew install cmake")
        .success();
    assert!(ok, "cmake configure failed");

    // ── cmake build ───────────────────────────────────────────────────────────
    let ok = Command::new("cmake")
        .args(["--build", mgba_build.to_str().unwrap(), "--target", "mgba", "-j4"])
        .status()
        .expect("cmake --build failed")
        .success();
    assert!(ok, "libmgba.a build failed");

    // ── compile C wrapper with the same flags cmake used for libmgba.a ────────
    // This ensures struct mCore layout matches between the wrapper and the lib.
    let (cmake_defines, cmake_includes, cmake_extra_flags) =
        parse_flags_make(&mgba_build.join("CMakeFiles/mgba.dir/flags.make"));

    let mut build = cc::Build::new();
    build.file(manifest.join("src/mgba_wrapper.c"));
    for (name, val) in &cmake_defines {
        build.define(name.as_str(), val.as_deref());
    }
    for inc in &cmake_includes {
        build.include(inc);
    }
    for flag in &cmake_extra_flags {
        build.flag(flag);
    }
    build.opt_level(2);
    build.compile("mgba_wrapper");

    // ── linker directives ─────────────────────────────────────────────────────
    println!("cargo:rustc-link-search=native={}", mgba_build.display());
    println!("cargo:rustc-link-lib=static=mgba");
    // macOS system frameworks used by mGBA internals
    println!("cargo:rustc-link-lib=framework=Foundation");
    println!("cargo:rustc-link-lib=framework=OpenGL");
    // mGBA uses libpng and zlib
    println!("cargo:rustc-link-search=native=/opt/homebrew/lib");
    println!("cargo:rustc-link-lib=static=png16");
    println!("cargo:rustc-link-lib=dylib=z");

    // ── bindgen — run on the simple wrapper header (not mGBA internals) ───────
    let wrapper_h = manifest.join("src/mgba_wrapper.h");
    let bindings = bindgen::Builder::default()
        .header(wrapper_h.to_str().unwrap())
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .generate()
        .expect("bindgen failed on mgba_wrapper.h");

    bindings
        .write_to_file(out.join("mgba_bindings.rs"))
        .unwrap();

    // ── rebuild triggers ──────────────────────────────────────────────────────
    println!("cargo:rerun-if-changed=src/mgba_wrapper.c");
    println!("cargo:rerun-if-changed=src/mgba_wrapper.h");
}

/// Parse C_DEFINES and C_INCLUDES from a cmake-generated flags.make file.
/// Returns (defines, include_dirs, extra_flags) where:
///   defines = Vec<(name, Option<value>)>
///   include_dirs = Vec<PathBuf-style strings> for .include()
///   extra_flags = Vec<raw flags> for -isystem, -F, etc.
fn parse_flags_make(
    path: &PathBuf,
) -> (Vec<(String, Option<String>)>, Vec<String>, Vec<String>) {
    let content = std::fs::read_to_string(path).unwrap_or_default();
    let mut defines: Vec<(String, Option<String>)> = Vec::new();
    let mut includes: Vec<String> = Vec::new();
    let mut extra_flags: Vec<String> = Vec::new();

    for line in content.lines() {
        if let Some(rest) = line.strip_prefix("C_DEFINES = ") {
            for token in rest.split_whitespace() {
                if let Some(d) = token.strip_prefix("-D") {
                    if let Some((name, val)) = d.split_once('=') {
                        defines.push((name.to_string(), Some(val.to_string())));
                    } else {
                        defines.push((d.to_string(), None));
                    }
                }
            }
        } else if let Some(rest) = line.strip_prefix("C_INCLUDES = ") {
            let tokens: Vec<&str> = rest.split_whitespace().collect();
            let mut i = 0;
            while i < tokens.len() {
                let t = tokens[i];
                if let Some(path) = t.strip_prefix("-I") {
                    if path.is_empty() && i + 1 < tokens.len() {
                        includes.push(tokens[i + 1].to_string());
                        i += 2;
                    } else {
                        includes.push(path.to_string());
                        i += 1;
                    }
                } else if t == "-isystem" && i + 1 < tokens.len() {
                    // cc::Build doesn't have a native isystem method; use flag()
                    extra_flags.push(format!("-isystem{}", tokens[i + 1]));
                    i += 2;
                } else if t.starts_with("-F") || t.starts_with("--sysroot") {
                    extra_flags.push(t.to_string());
                    i += 1;
                } else {
                    i += 1;
                }
            }
        }
    }

    (defines, includes, extra_flags)
}
