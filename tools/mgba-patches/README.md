# mGBA headless build for co-op testing (Windows)

The MCP test harness (`tools/mcp_gamestate/server.py`) drives a headless build of
mGBA with Lua scripting. On Windows this build must be produced carefully — the
notes below capture the working recipe and the patches required.

## Toolchain

Use the **MSYS2 CLANG64** toolchain. The MINGW64 and UCRT64 toolchains both
produce a binary that crashes at startup with a
`32 bit pseudo relocation ... out of range` error (Windows ASLR places system
DLLs >2 GB from the image base, overflowing the 32-bit pseudo-reloc). CLANG64
does not use pseudo-relocations and works.

```powershell
pacman -S --noconfirm `
  mingw-w64-clang-x86_64-toolchain `
  mingw-w64-clang-x86_64-cmake `
  mingw-w64-clang-x86_64-ninja `
  mingw-w64-clang-x86_64-lua `
  mingw-w64-clang-x86_64-libepoxy
```

## Patches

`0001-headless-video-buffer.patch` — the headless frontend never calls
`core->setVideoBuffer()`, so `getPixels()` returns NULL and **any screenshot
segfaults the process** (which also breaks the recording feature). The patch
allocates a static framebuffer and registers it right after `core->init()`.
Apply it to the mGBA source tree before building:

```bash
cd <mgba-src>
git apply <repo>/tools/mgba-patches/0001-headless-video-buffer.patch
```

## Build

```powershell
$env:PATH = "C:\msys64\clang64\bin;C:\msys64\usr\bin;" + $env:PATH
cmake -S <mgba-src> -B <mgba-build> `
  -DBUILD_HEADLESS=ON -DUSE_LUA=ON -DBUILD_SDL=OFF -DBUILD_QT=OFF `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=C:/msys64/clang64/bin/clang.exe `
  -DCMAKE_CXX_COMPILER=C:/msys64/clang64/bin/clang++.exe `
  -DCMAKE_PREFIX_PATH=C:/msys64/clang64 -G Ninja
cmake --build <mgba-build> --target mgba-headless -j8
```

Then copy every DLL from `C:\msys64\clang64\bin` that the binary needs next to
`mgba-headless.exe` (simplest: copy all `*.dll`), and point the MCP server at it
via the `MGBA` env var (see `.mcp.json`).

## Known headless limitations / harness notes

- **Only `press_button` and `wait` are safe nav primitives.** `move_steps` and
  `advance_text` run synchronous `emu:runFrame()` loops inside the Lua command
  handler and time out (>30 s) on this build, corrupting instance state. Drive
  movement one tile at a time with `press_button`.
- **Heartbeat timeout is wall-clock.** The relay's silence-based disconnect
  detector uses real time, which is unrelated to emulated time when an agent
  steps the emulators slowly. Menus that pause `Multiplayer_Update` (party
  select) send no packets, so a low timeout fires a false
  `PARTNER_DISCONNECTED`. Default is now 1 hour; override with
  `COOP_HEARTBEAT_TIMEOUT`. Real process death is still caught immediately via
  `process.poll()`.
- **Rival lab trigger spans 3 tiles** — (5,8), (6,8), (7,8). In co-op the two
  players must stand on *different* trigger tiles; the first player's ghost is
  collidable and occupies its tile on the partner's screen. Send P1 to (7,8)
  and P2 to (6,8) (or (5,8)).
