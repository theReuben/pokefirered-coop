-- tools/build_save_states.lua
-- Automates through the FRLG intro and saves mGBA state files at key
-- checkpoints used by the two-instance coop test scenarios.
--
-- Run once (or after a ROM rebuild) to regenerate the state files:
--
--   /Applications/mGBA.app/Contents/MacOS/mGBA -S tools/build_save_states.lua pokefirered.gba
--
-- Or via Make:
--   make build-states
--
-- State files are written to test/lua/states/:
--   oaks_lab.ss1          — starter ball table visible, no pick made yet
--   tall_grass_route1.ss1 — standing at Route 1 entry, can step into grass
--   pewter_gym.ss1        — inside Pewter City Gym, Brock visible (not yet fought)
--
-- Tuning notes:
--   Frame counts are conservative (~2× what the game needs) to handle
--   PAL vs NTSC timing variation.  If a step times out, increase its
--   WAIT value.  Run with VERBOSE=1 to see per-step progress prints.
--
-- mGBA Lua scripting API (0.10+):
--   emu:runFrame()             advance one frame
--   emu:setKeys(mask)          set held buttons (0 = nothing held)
--   emu:saveStateFile(path, 0) save state to file
--   emu:quit()                 exit after the script finishes

local VERBOSE = (os.getenv("VERBOSE") == "1")

-- ── Button masks (GBA key register, active-low in hardware but mGBA
--    expects active-high in setKeys) ──────────────────────────────────────
local KEY_A      = 0x001
local KEY_B      = 0x002
local KEY_SELECT = 0x004
local KEY_START  = 0x008
local KEY_RIGHT  = 0x010
local KEY_LEFT   = 0x020
local KEY_UP     = 0x040
local KEY_DOWN   = 0x080
local KEY_R      = 0x100
local KEY_L      = 0x200

-- ── Paths ─────────────────────────────────────────────────────────────────
-- Resolve repo root relative to this script (works when invoked from any cwd)
local SCRIPT_DIR = debug.getinfo(1, "S").source:match("@(.+/)") or "./"
local REPO_ROOT  = SCRIPT_DIR .. "../"
local STATES_DIR = REPO_ROOT .. "test/lua/states/"

local function log(msg)
    if VERBOSE then print("[states] " .. msg) end
end

-- ── Low-level helpers ──────────────────────────────────────────────────────

local function press(key, hold, release)
    hold    = hold    or 3
    release = release or 3
    emu:setKeys(key)
    for _ = 1, hold do emu:runFrame() end
    emu:setKeys(0)
    for _ = 1, release do emu:runFrame() end
end

local function idle(n)
    emu:setKeys(0)
    for _ = 1, n do emu:runFrame() end
end

-- Spam A through text boxes or confirmation prompts.
local function spamA(times, gap)
    gap = gap or 6
    for _ = 1, times do
        press(KEY_A, 2, gap)
    end
end

-- Walk in a direction for N tiles (hold key per-tile for ~16 frames, the
-- standard outdoor tile-step duration in FRLG at 60 fps).
local function walk(dir, tiles)
    for _ = 1, tiles do
        press(dir, 16, 2)
    end
end

-- Save an mGBA state to the states directory.
local function saveState(name)
    local path = STATES_DIR .. name
    log("saving state → " .. path)
    emu:saveStateFile(path, 0)
    print("[states] SAVED " .. name)
end

-- ── Verify mGBA scripting API is available ─────────────────────────────────
if not emu or not emu.runFrame or not emu.setKeys or not emu.saveStateFile then
    print("[states] ERROR: mGBA scripting API not available.")
    print("         Load this script via: mgba-headless --script tools/build_save_states.lua rom.gba")
    if emu and emu.quit then emu:quit() end
    return
end

-- ── PHASE 1: Title screen → New Game ──────────────────────────────────────
-- The ROM boots with the Nintendo/GF logos, then the intro animation,
-- then the title screen where pressing START leads to the main menu.
log("phase 1: title screen")

-- Skip intro logos and title animation (≈ 300 frames for logos + 200 for title)
idle(500)

-- Press START to go from title screen to main menu.
press(KEY_START, 3, 30)

-- Main menu: NEW GAME is cursor position 0.  Press A.
idle(60)
press(KEY_A, 3, 30)

-- ── PHASE 2: Professor Oak's introduction ─────────────────────────────────
-- Oak speaks for ~8 dialogue boxes.  Each press of A advances one box.
log("phase 2: oak intro speech")
spamA(12, 30)

-- ── PHASE 3: Gender selection ─────────────────────────────────────────────
-- "Are you a boy or a girl?" — BOY is the default (left option in FRLG).
-- Just press A to confirm.
log("phase 3: gender selection")
idle(30)
press(KEY_A, 3, 30)

-- ── PHASE 4: Player name entry ────────────────────────────────────────────
-- The name entry screen shows preset names.  "RED" is the first preset.
-- Press A once to select "RED" from the preset list, then A again to confirm.
log("phase 4: player name")
idle(60)
-- Navigate to preset names list (usually requires pressing RIGHT or DOWN first)
press(KEY_DOWN, 3, 10)    -- move cursor to preset list
press(KEY_A, 3, 10)       -- select first preset (RED)
press(KEY_A, 3, 30)       -- confirm name

-- ── PHASE 5: Rival name entry ─────────────────────────────────────────────
-- "And your friend's name?" — similar layout, "GARY" is first preset.
log("phase 5: rival name")
spamA(3, 30)              -- advance Oak's "your friend" dialogue
idle(60)
press(KEY_DOWN, 3, 10)    -- move to preset list
press(KEY_A, 3, 10)       -- select first preset (GARY)
press(KEY_A, 3, 30)       -- confirm name

-- ── PHASE 6: Bedroom → overworld ─────────────────────────────────────────
-- Player wakes up in their bedroom (2F of player's house in Pallet Town).
-- Walk down the stairs (2 DOWN steps + exit tile), exit the house.
log("phase 6: bedroom exit")
spamA(6, 30)              -- clear any remaining intro text

-- Walk downstairs and out of the house.  The exact tile counts depend on
-- the spawn point; these are calibrated for the FRLG player house.
walk(KEY_DOWN, 8)
press(KEY_A, 2, 10)       -- confirm any "downstairs" prompt if present
walk(KEY_DOWN, 6)

-- Outside: walk south toward Route 1 gate to trigger Oak's escort.
log("phase 6b: walk south to trigger oak escort")
walk(KEY_DOWN, 12)

-- ── PHASE 7: Oak escort cutscene ─────────────────────────────────────────
-- Oak appears, stops the player, and escorts them to the lab.  This is a
-- scripted movement sequence; press A to advance each dialogue box.
log("phase 7: oak escort to lab")
spamA(6, 30)
idle(300)                 -- wait for movement animation to complete
spamA(4, 30)              -- any remaining post-escort dialogue

-- ── PHASE 8: Oak's lab → starter table ───────────────────────────────────
-- After the escort the player is inside the lab.  Oak explains the Pokéballs.
-- Walk up to the starter table (north side of lab).
log("phase 8: approach starter table")
spamA(8, 30)              -- Oak's lab intro speech
walk(KEY_UP, 4)           -- walk north toward the table

-- ── CHECKPOINT 1: oaks_lab.ss1 ────────────────────────────────────────────
-- Player is standing in front of the three Pokéballs, no pick made yet.
log("saving oaks_lab.ss1 checkpoint")
idle(30)
saveState("oaks_lab.ss1")

-- ── PHASE 9: Choose a starter and leave the lab ──────────────────────────
-- Pick Bulbasaur (leftmost ball) to proceed.  Press A on the ball,
-- confirm the choice, clear rival interaction dialogue, and exit.
log("phase 9: pick starter and exit lab")
press(KEY_A, 3, 30)       -- interact with first ball (Bulbasaur)
spamA(10, 30)             -- Oak speech + "will you take this?" confirm
press(KEY_A, 3, 30)       -- "Yes"
spamA(10, 30)             -- nickname prompt + rival interaction + more dialogue
walk(KEY_DOWN, 12)        -- exit lab

-- ── PHASE 10: Walk to Route 1 ─────────────────────────────────────────────
-- From Pallet Town exit, walk north onto Route 1 tall grass.
log("phase 10: route 1 tall grass")
idle(60)
walk(KEY_UP, 3)           -- step onto Route 1

-- ── CHECKPOINT 2: tall_grass_route1.ss1 ───────────────────────────────────
idle(30)
saveState("tall_grass_route1.ss1")

-- ── PHASE 11: Walk through Viridian City → Pewter City ────────────────────
-- Route 1 → Viridian City (walk north ~15 tiles through Route 1).
-- Then Route 2 → Viridian Forest entrance → through forest → Pewter City.
-- This section is long; frame counts are generous.
log("phase 11: viridian city and beyond")

-- Route 1 (walk north to Viridian City)
walk(KEY_UP, 20)
idle(120)                 -- map transition
walk(KEY_UP, 15)          -- through Viridian City
idle(60)

-- Viridian City Pokémart: must deliver Oak's Parcel first in authentic FR
-- playthrough.  For test purposes we skip that and walk directly north.
-- (If the NPC blocks you, use the route through the right side of town.)
walk(KEY_UP, 10)
idle(120)                 -- Route 2 entry

-- Route 2 south section → Viridian Forest entrance
walk(KEY_UP, 25)
idle(120)                 -- forest gate / warp

-- Viridian Forest (simple straight path)
walk(KEY_UP, 60)
idle(120)                 -- exit warp

-- Route 2 north section
walk(KEY_UP, 15)
idle(120)                 -- Pewter City entry

-- Pewter City: walk to the gym (northwest area of the city)
walk(KEY_UP, 5)
walk(KEY_LEFT, 8)
walk(KEY_UP, 8)
idle(120)                 -- gym interior warp

-- ── CHECKPOINT 3: pewter_gym.ss1 ──────────────────────────────────────────
idle(60)
saveState("pewter_gym.ss1")

-- ── Done ──────────────────────────────────────────────────────────────────
print("[states] All checkpoints saved to " .. STATES_DIR)
print("[states] Checkpoints: oaks_lab.ss1  tall_grass_route1.ss1  pewter_gym.ss1")
emu:quit()
