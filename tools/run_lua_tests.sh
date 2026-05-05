#!/usr/bin/env bash
# Headless mGBA Lua scenario runner.
#
# Boots a built ROM under xvfb-run with each test/lua/scenarios/*.lua
# script attached, captures the RESULT line(s) the harness writes, and
# exits non-zero if any scenario reported FAIL.
#
# Usage:
#   tools/run_lua_tests.sh [scenario.lua ...]
#
# With no args, runs every test/lua/scenarios/*.lua.
# Requires: xvfb-run, an mGBA binary on $PATH (mgba-qt or mgba).

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ROM="${ROM:-$REPO_ROOT/pokefirered.gba}"
SCENARIO_DIR="$REPO_ROOT/test/lua/scenarios"
TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-60}"

# Pick whichever mGBA binary is available. On macOS the app bundle is
# not on PATH, so check the standard install location as a fallback.
MGBA="$(command -v mgba-qt || command -v mgba || true)"
if [[ -z "$MGBA" ]]; then
    if [[ -x "/Applications/mGBA.app/Contents/MacOS/mGBA" ]]; then
        MGBA="/Applications/mGBA.app/Contents/MacOS/mGBA"
    fi
fi
if [[ -z "$MGBA" ]]; then
    echo "error: mGBA not found (tried PATH and /Applications/mGBA.app)" >&2
    exit 2
fi

# xvfb-run is only needed on headless Linux (CI). On macOS, skip it.
XVFB_RUN=()
if command -v xvfb-run &>/dev/null; then
    XVFB_RUN=(xvfb-run -a)
fi

# GNU timeout (Linux) or gtimeout (macOS Homebrew coreutils), or no timeout.
TIMEOUT_BIN="$(command -v timeout || command -v gtimeout || true)"
TIMEOUT_CMD=()
if [[ -n "$TIMEOUT_BIN" ]]; then
    TIMEOUT_CMD=("$TIMEOUT_BIN" "$TIMEOUT_SECONDS")
fi

# Probe for Lua scripting support.  The official mGBA macOS .app is built
# without Lua; the flag simply won't appear in --help.  On CI, Ubuntu's
# mgba-qt package includes Lua support.
if ! "$MGBA" --help 2>&1 | grep -qE -- '-S|--script'; then
    echo "warning: $MGBA does not support Lua scripting (-S / --script)." >&2
    echo "         The official mGBA macOS .app omits Lua scripting." >&2
    echo "         Install a Lua-enabled build with:" >&2
    echo "           brew install mgba          # adds 'mgba' to PATH" >&2
    echo "         Then re-run: make check-lua" >&2
    exit 2
fi

if [[ ! -f "$ROM" ]]; then
    echo "error: ROM not found at $ROM (build with 'make firered' first)" >&2
    exit 2
fi

# Collect scenarios — caller can pass specific ones, otherwise glob.
SCENARIOS=()
if [[ $# -gt 0 ]]; then
    SCENARIOS=("$@")
else
    while IFS= read -r f; do SCENARIOS+=("$f"); done \
        < <(find "$SCENARIO_DIR" -maxdepth 1 -name '*.lua' -print | sort)
fi

if [[ ${#SCENARIOS[@]} -eq 0 ]]; then
    echo "error: no scenarios found in $SCENARIO_DIR" >&2
    exit 2
fi

# Lua's require() looks at LUA_PATH; point it at test/lua/ so scenarios
# can `require("_harness")` and `require("memory_map")` regardless of cwd.
export LUA_PATH="$REPO_ROOT/test/lua/?.lua;$REPO_ROOT/test/lua/?/init.lua;;"

failures=0
total=${#SCENARIOS[@]}
echo "==> Running $total Lua scenario(s) against $ROM"

for scenario in "${SCENARIOS[@]}"; do
    name="$(basename "$scenario" .lua)"
    results_file="$(mktemp -t "lua_${name}_XXXXXX.txt")"
    log_file="$(mktemp -t "lua_${name}_XXXXXX.log")"

    echo "--- $name"
    LUA_TEST_RESULTS="$results_file" \
        ${TIMEOUT_CMD[@]+"${TIMEOUT_CMD[@]}"} \
        ${XVFB_RUN[@]+"${XVFB_RUN[@]}"} \
        "$MGBA" -S "$scenario" "$ROM" \
        > "$log_file" 2>&1
    exitcode=$?

    # Pull the RESULT line from either the results file or stdout —
    # depending on the mGBA build, console:log goes to one, both, or
    # neither. Prefer the file (the harness always writes there).
    result_line="$(grep -E '^RESULT: ' "$results_file" 2>/dev/null | head -1)"
    if [[ -z "$result_line" ]]; then
        result_line="$(grep -E '^RESULT: ' "$log_file" 2>/dev/null | head -1)"
    fi

    if [[ -z "$result_line" ]]; then
        echo "    NO RESULT (timeout or crash, exit $exitcode)"
        echo "    --- last 20 lines of mgba output ---"
        tail -20 "$log_file" | sed 's/^/    /'
        failures=$((failures + 1))
    elif [[ "$result_line" == "RESULT: PASS"* ]]; then
        echo "    $result_line"
    else
        echo "    $result_line"
        failures=$((failures + 1))
    fi

    rm -f "$results_file" "$log_file"
done

echo
if [[ $failures -eq 0 ]]; then
    echo "All $total Lua scenarios passed."
    exit 0
else
    echo "$failures of $total Lua scenarios failed."
    exit 1
fi
