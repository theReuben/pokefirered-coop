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

# Pick whichever mGBA binary is on PATH. Prefer mgba-qt because its
# scripting API is the most stable; fall back to mgba (SDL frontend
# in 0.10+) if qt isn't installed.
MGBA="$(command -v mgba-qt || command -v mgba || true)"
if [[ -z "$MGBA" ]]; then
    echo "error: mGBA not found on PATH (need mgba-qt or mgba)" >&2
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
        timeout "$TIMEOUT_SECONDS" \
        xvfb-run -a "$MGBA" -l "$scenario" "$ROM" \
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
