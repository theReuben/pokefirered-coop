# Running Lua Scenario Tests Locally

The Lua test layer (Layer 2) drives a real built ROM under mGBA and asserts
on emulator memory state via the `-S` Lua scripting flag. This flag is only
available in mGBA builds that were compiled with Lua support enabled.

## Platform support

| Environment | Works out of the box? |
|---|---|
| CI (Ubuntu, mgba-qt from apt) | Yes |
| macOS official .app (mGBA.app) | No — compiled without Lua |
| macOS Homebrew (`brew install mgba`) | No — Qt build, no Lua |
| macOS source build (SDL + Lua) | Yes — see below |
| Linux source build | Yes — see below |

## Quick check

```bash
make check-lua
```

If you see `warning: ... does not support Lua scripting (-S flag)`, follow
the steps below.

## macOS: build mGBA from source with SDL + Lua

All prerequisites are available via Homebrew:

```bash
brew install cmake sdl2 lua
```

Clone and build (takes 3–5 minutes on Apple Silicon). The SDL frontend has a
cmake 4.x compatibility issue on macOS, so we build the headless binary
instead — it uses `--script` instead of `-S` and the test runner detects this
automatically.

```bash
# Make the keg-only Homebrew lua visible to cmake
brew link --force lua

git clone --depth=1 https://github.com/mgba-emu/mgba /tmp/mgba-src

cmake --fresh -B /tmp/mgba-build \
  -DBUILD_QT=OFF \
  -DBUILD_SDL=OFF \
  -DBUILD_HEADLESS=ON \
  -DUSE_LUA=ON \
  -DCMAKE_BUILD_TYPE=Release \
  /tmp/mgba-src

cmake --build /tmp/mgba-build -j$(sysctl -n hw.ncpu)
```

The resulting binary is at `/tmp/mgba-build/mgba-headless`. The test runner
finds it automatically if placed there. To run:

```bash
make check-lua
```

Or point explicitly at it:

```bash
MGBA=/tmp/mgba-build/mgba-headless make check-lua
```

## Linux (Ubuntu/Debian): install from apt

Ubuntu's `mgba-qt` package includes Lua scripting support since Noble (24.04):

```bash
sudo apt-get install mgba-qt xvfb lua5.4
make check-lua
```

If you're on an older release and mgba-qt lacks `-S`, build from source:

```bash
sudo apt-get install cmake libsdl2-dev liblua5.4-dev
git clone --depth=1 https://github.com/mgba-emu/mgba /tmp/mgba-src
cmake -B /tmp/mgba-build -DBUILD_QT=OFF -DBUILD_SDL=ON -DUSE_LUA=ON \
  -DCMAKE_BUILD_TYPE=Release /tmp/mgba-src
cmake --build /tmp/mgba-build -j$(nproc)
MGBA=/tmp/mgba-build/mgba make check-lua
```

## What the tests cover

Each `.lua` file in `test/lua/scenarios/` is a self-contained scenario:

| Scenario | What it tests |
|---|---|
| `flag_set_dispatch.lua` | FLAG_SET packet → FlagSet() → save block bit flipped |
| `script_mutex_dispatch.lua` | SCRIPT_LOCK/UNLOCK packets → partnerIsInScript transitions |

The test runner (`tools/run_lua_tests.sh`) boots the ROM under mGBA with
each script attached, captures `RESULT: PASS` / `RESULT: FAIL` from the
harness, and exits non-zero if any scenario fails.

## Skipping Lua tests

If you don't need Lua tests locally (CI catches them on every push), you
can skip the step entirely — `make check-lua` exits 2 (not 1) on the
capability check so Make reports it as an error rather than a test failure.
There's no `--skip` flag; simply don't run `make check-lua` if your local
mGBA lacks scripting support.
