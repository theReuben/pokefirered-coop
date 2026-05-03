use std::path::PathBuf;
use std::process::Command;

fn main() {
    tauri_build::build();

    if std::env::var("CARGO_FEATURE_MGBA").is_ok() {
        build_mgba();
    }
}

fn build_mgba() {
    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    let manifest = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap());
    let mgba_src = manifest.join("mgba");
    let mgba_build = out.join("mgba-cmake");

    // ── cmake configure ───────────────────────────────────────────────────────
    std::fs::create_dir_all(&mgba_build).unwrap();

    let mut cmake_args = vec![
        mgba_src.to_str().unwrap().to_string(),
        "-DBUILD_SHARED=OFF".to_string(),
        "-DBUILD_STATIC=ON".to_string(),
        "-DBUILD_SDL=OFF".to_string(),
        "-DBUILD_QT=OFF".to_string(),
        "-DBUILD_LIBRETRO=OFF".to_string(),
        "-DBUILD_PERF=OFF".to_string(),
        "-DBUILD_TEST=OFF".to_string(),
        "-DBUILD_EXAMPLE=OFF".to_string(),
        "-DCMAKE_BUILD_TYPE=Release".to_string(),
        "-DBUILD_LTO=OFF".to_string(),
        "-DUSE_EDITLINE=OFF".to_string(),
        "-DUSE_FREETYPE=OFF".to_string(),
        "-DUSE_SQLITE3=OFF".to_string(),
        "-DUSE_DISCORD_RPC=OFF".to_string(),
        "-DUSE_FFMPEG=OFF".to_string(),
        "-DUSE_LIBZIP=OFF".to_string(),
        "-DUSE_LUA=OFF".to_string(),
        "-DENABLE_SCRIPTING=OFF".to_string(),
        "-DENABLE_DEBUGGERS=OFF".to_string(),
        "-DENABLE_GDB_STUB=OFF".to_string(),
        // Build only the library target, skipping all GUI/application code.
        // This avoids the Windows "epoxy required" check and other UI deps.
        "-DLIBMGBA_ONLY=ON".to_string(),
        // Remove libpng dependency — we don't need screenshots or PNG saves.
        "-DUSE_PNG=OFF".to_string(),
        // Required on Linux: Tauri builds a cdylib (.so), which requires all
        // statically-linked objects to be compiled as position-independent code.
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON".to_string(),
        // Export compile commands so we can match the wrapper's compile flags
        // to libmgba.a's flags (prevents struct layout mismatches).
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON".to_string(),
    ];

    // macOS: build a fat library so both aarch64 and x86_64 slices are covered
    // by the single cmake invocation that each cargo arch-build triggers.
    if target_os == "macos" {
        cmake_args.push("-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64".to_string());
    }

    // Windows: use Ninja so cmake generates compile_commands.json
    // (the Visual Studio generator does not support EXPORT_COMPILE_COMMANDS).
    if target_os == "windows" {
        cmake_args.push("-G".to_string());
        cmake_args.push("Ninja".to_string());
    }

    let ok = Command::new("cmake")
        .current_dir(&mgba_build)
        .args(&cmake_args)
        .status()
        .expect("cmake not found — install with: brew install cmake  (macOS)  or  apt install cmake  (Linux)")
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
    let (cmake_defines, cmake_includes, cmake_extra_flags) = {
        let cc_json  = mgba_build.join("compile_commands.json");
        let flags_mk = mgba_build.join("CMakeFiles/mgba.dir/flags.make");
        if cc_json.exists() {
            parse_compile_commands(&cc_json)
        } else {
            parse_flags_make(&flags_mk)
        }
    };

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

    match target_os.as_str() {
        "macos" => {
            println!("cargo:rustc-link-lib=framework=Foundation");
            println!("cargo:rustc-link-lib=framework=OpenGL");
            println!("cargo:rustc-link-lib=dylib=z");
        }
        "linux" => {
            println!("cargo:rustc-link-lib=dylib=z");
        }
        "windows" => {
            // Ninja/MSVC puts release artifacts in a Release subdirectory.
            println!("cargo:rustc-link-search=native={}/Release", mgba_build.display());

            // cmake falls back to its bundled zlib when no system zlib is found.
            // Link whichever location it ended up in.
            let zlib_dir = mgba_build.join("zlib");
            if zlib_dir.exists() {
                println!("cargo:rustc-link-search=native={}", zlib_dir.display());
                println!("cargo:rustc-link-search=native={}/Release", zlib_dir.display());
                println!("cargo:rustc-link-lib=static=zlibstatic");
            }
        }
        _ => {}
    }

    // ── rebuild triggers ──────────────────────────────────────────────────────
    println!("cargo:rerun-if-changed=src/mgba_wrapper.c");
    println!("cargo:rerun-if-changed=src/mgba_wrapper.h");
}

/// Parse compile flags from cmake's compile_commands.json.
/// Works on all generators (Makefile, Ninja) and all platforms.
fn parse_compile_commands(
    path: &PathBuf,
) -> (Vec<(String, Option<String>)>, Vec<String>, Vec<String>) {
    let content = std::fs::read_to_string(path).unwrap_or_default();
    let command = extract_json_string_value(&content, "command").unwrap_or_default();

    let mut defines: Vec<(String, Option<String>)> = Vec::new();
    let mut includes: Vec<String> = Vec::new();
    let mut extra_flags: Vec<String> = Vec::new();

    let tokens: Vec<&str> = command.split_whitespace().collect();
    let mut i = 0;
    while i < tokens.len() {
        let t = tokens[i];

        // -DFOO / /DFOO (MSVC uses /D on Windows)
        if let Some(d) = t.strip_prefix("-D").or_else(|| t.strip_prefix("/D")) {
            if d.is_empty() && i + 1 < tokens.len() {
                let next = tokens[i + 1];
                if let Some((name, val)) = next.split_once('=') {
                    defines.push((name.to_string(), Some(val.to_string())));
                } else {
                    defines.push((next.to_string(), None));
                }
                i += 2;
            } else if let Some((name, val)) = d.split_once('=') {
                defines.push((name.to_string(), Some(val.to_string())));
                i += 1;
            } else if !d.is_empty() {
                defines.push((d.to_string(), None));
                i += 1;
            } else {
                i += 1;
            }
        }
        // -IFOO / /IFOO
        else if let Some(p) = t.strip_prefix("-I").or_else(|| t.strip_prefix("/I")) {
            if p.is_empty() && i + 1 < tokens.len() {
                includes.push(tokens[i + 1].to_string());
                i += 2;
            } else if !p.is_empty() {
                includes.push(p.to_string());
                i += 1;
            } else {
                i += 1;
            }
        }
        // -isystem (GCC/Clang system include)
        else if t == "-isystem" && i + 1 < tokens.len() {
            extra_flags.push(format!("-isystem{}", tokens[i + 1]));
            i += 2;
        }
        // -F (macOS framework search path)
        else if t.starts_with("-F") && t.len() > 2 {
            extra_flags.push(t.to_string());
            i += 1;
        }
        else {
            i += 1;
        }
    }

    (defines, includes, extra_flags)
}

/// Extract the string value of a JSON key (minimal parser, no external crates).
fn extract_json_string_value(json: &str, key: &str) -> Option<String> {
    let needle = format!("\"{}\"", key);
    let pos = json.find(&needle)?;
    let after = json[pos + needle.len()..].trim_start_matches(|c: char| c.is_whitespace() || c == ':');
    if !after.starts_with('"') { return None; }

    let mut result = String::new();
    let mut chars = after[1..].chars();
    loop {
        match chars.next()? {
            '"' => break,
            '\\' => match chars.next()? {
                'n'  => result.push('\n'),
                't'  => result.push('\t'),
                'r'  => result.push('\r'),
                '"'  => result.push('"'),
                '\\' => result.push('\\'),
                c    => { result.push('\\'); result.push(c); }
            },
            c => result.push(c),
        }
    }
    Some(result)
}

/// Parse C_DEFINES and C_INCLUDES from a cmake Makefile-generator flags.make.
/// Fallback for platforms where compile_commands.json is unavailable.
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
