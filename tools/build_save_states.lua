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
-- Map detection:
--   waitForMap() polls gSaveBlock1Ptr->location each frame so map transitions
--   are detected reliably regardless of frame-count variation.  Run with
--   VERBOSE=1 to see per-checkpoint diagnostics.

local VERBOSE = (os.getenv("VERBOSE") == "1")

-- ── Button masks ──────────────────────────────────────────────────────────────
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

-- ── Map group/num constants ───────────────────────────────────────────────────
-- Encoding: (mapNum | (mapGroup << 8)) — see include/constants/map_groups.h
local MAP = {
    HOUSE_2F   = {g=38, n=1},   -- MAP_PALLET_TOWN_PLAYERS_HOUSE_2F
    HOUSE_1F   = {g=38, n=0},   -- MAP_PALLET_TOWN_PLAYERS_HOUSE_1F
    PALLET     = {g=37, n=0},   -- MAP_PALLET_TOWN
    OAKS_LAB   = {g=38, n=3},   -- MAP_PALLET_TOWN_PROFESSOR_OAKS_LAB
    ROUTE1     = {g=37, n=19},  -- MAP_ROUTE1
    VIRIDIAN   = {g=37, n=1},   -- MAP_VIRIDIAN_CITY
    ROUTE2     = {g=37, n=20},  -- MAP_ROUTE2
    VID_FOREST = {g=35, n=0},   -- MAP_VIRIDIAN_FOREST
    PEWTER     = {g=37, n=2},   -- MAP_PEWTER_CITY
    PEWTER_GYM = {g=40, n=2},   -- MAP_PEWTER_CITY_GYM
}

-- ── Save-block pointer (IWRAM) ────────────────────────────────────────────────
-- Load from auto-generated memory map so address stays correct after ROM rebuilds.
local _mem_path = (debug.getinfo(1, "S").source:match("@(.+/)") or "./") .. "../test/lua/memory_map.lua"
local _mem = dofile(_mem_path)
local ADDR_SB1_PTR      = _mem.gSaveBlock1Ptr
local ADDR_SB2_PTR      = _mem.gSaveBlock2Ptr
local ADDR_SCRIPT_STATUS = _mem.sGlobalScriptContextStatus  -- 0=RUNNING,1=WAITING,2=SHUTDOWN
local ADDR_FIELD_LOCK   = _mem.sLockFieldControls           -- bool8: 1 while in script/dialogue
local ADDR_GTASKS        = _mem.gTasks                      -- task array base; stride 0x28
local YESNO_TASK_FUNC    = _mem.Task_HandleYesNoInput       -- ROM code addr (add 1 for THUMB ptr)
local ADDR_COOP_SETTINGS = _mem.gCoopSettings               -- struct CoopSettings (randomizeEncounters at +0)
assert(ADDR_SB1_PTR and ADDR_SB1_PTR ~= 0,
    "gSaveBlock1Ptr not found in memory_map.lua — run 'make build-states' to regenerate")
assert(ADDR_SCRIPT_STATUS and ADDR_SCRIPT_STATUS ~= 0,
    "sGlobalScriptContextStatus not found — regenerate memory_map.lua with pokefirered.elf")
assert(ADDR_FIELD_LOCK and ADDR_FIELD_LOCK ~= 0,
    "sLockFieldControls not found — regenerate memory_map.lua with pokefirered.elf")
assert(ADDR_GTASKS and YESNO_TASK_FUNC,
    "gTasks/Task_HandleYesNoInput missing — regenerate memory_map.lua with pokefirered.elf")
local SB1_LOC_GROUP = 4
local SB1_LOC_NUM   = 5

-- ── Player position (EWRAM gObjectEvents[0].currentCoords) ───────────────────
-- ObjectEvent size=0x24; currentCoords at +0x10 (x:s16, y:s16).
local ADDR_OBJ0_X = 0x020015bc + 0x10
local ADDR_OBJ0_Y = 0x020015bc + 0x12
local TASK_STRIDE = 0x28  -- sizeof(struct Task)

-- ── Paths ─────────────────────────────────────────────────────────────────────
local SCRIPT_DIR = debug.getinfo(1, "S").source:match("@(.+/)") or "./"
local REPO_ROOT  = SCRIPT_DIR .. "../"
local STATES_DIR = REPO_ROOT .. "test/lua/states/"

-- ── Helpers ───────────────────────────────────────────────────────────────────

local function log(msg)
    if VERBOSE then print("[states] " .. msg) end
end

local function readMap()
    local ptr = emu:read32(ADDR_SB1_PTR)
    if ptr == 0 then return 0, 0 end
    return emu:read8(ptr + SB1_LOC_GROUP), emu:read8(ptr + SB1_LOC_NUM)
end

-- Run idle frames until gSaveBlock1Ptr->location matches (m.g, m.n).
-- Optionally presses A every spamA_every frames to advance stuck dialogue
-- or battle menus.  Prints frame count on success; errors on timeout.
local function waitForMap(m, label, maxFrames, spamA_every)
    maxFrames   = maxFrames   or 18000
    spamA_every = spamA_every or 0
    for i = 1, maxFrames do
        emu:setKeys(0)
        emu:runFrame()
        local g, n = readMap()
        if g == m.g and n == m.n then
            print(string.format("[states] MAP %-24s (%2d,%2d) after %5d frames",
                  label, g, n, i))
            return i
        end
        if spamA_every > 0 and i % spamA_every == 0 then
            emu:setKeys(KEY_A)
            emu:runFrame()
            emu:setKeys(0)
            emu:runFrame()
        end
        if i % 3000 == 0 then
            local g2, n2 = readMap()
            local ctx2 = emu:read8(ADDR_SCRIPT_STATUS)
            local lk2  = emu:read8(ADDR_FIELD_LOCK)
            local sb1  = emu:read32(ADDR_SB1_PTR)
            local yn = false
            for slot = 0, 15 do
                local base = ADDR_GTASKS + slot * TASK_STRIDE
                if emu:read8(base + 4) ~= 0 and emu:read32(base) == (YESNO_TASK_FUNC | 1) then
                    yn = true; break
                end
            end
            print(string.format("[states] waitForMap %s: frame=%d map=(%d,%d) ctx=%d lock=%d sb1=0x%08x yesno=%s",
                  label, i, g2, n2, ctx2, lk2, sb1, tostring(yn)))
        end
    end
    local g, n = readMap()
    error(string.format("[states] TIMEOUT waiting for %s (%d,%d) – on (%d,%d) after %d frames",
          label, m.g, m.n, g, n, maxFrames))
end

local function playerPos()
    local x = emu:read16(ADDR_OBJ0_X)
    local y = emu:read16(ADDR_OBJ0_Y)
    if x >= 0x8000 then x = x - 0x10000 end
    if y >= 0x8000 then y = y - 0x10000 end
    return x, y
end

-- Walk toward a map in 16-frame tile steps, clearing blocking dialogues with A.
-- Holds dir for 16 frames per step (one tile); only presses A when a script
-- or field-lock is detected, rather than blindly alternating every 8 frames.
local function walkToMap(dir, m, label, maxFrames)
    maxFrames = maxFrames or 36000
    local frames = 0
    local lastLogFrames = 0
    while frames < maxFrames do
        -- Hold dir for one full tile step (16 frames).
        emu:setKeys(dir)
        for _ = 1, 16 do
            emu:runFrame()
            frames = frames + 1
            local g, n = readMap()
            if g == m.g and n == m.n then
                emu:setKeys(0)
                print(string.format("[states] MAP %-24s (%2d,%2d) after %5d frames",
                      label, g, n, frames))
                return frames
            end
        end
        emu:setKeys(0)
        -- Dismiss any blocking script/dialogue (up to 120 frames of A spam).
        for _ = 1, 120 do
            local ctx  = emu:read8(ADDR_SCRIPT_STATUS)
            local lock = emu:read8(ADDR_FIELD_LOCK)
            if ctx ~= 1 and lock == 0 then break end
            emu:setKeys(KEY_A); emu:runFrame(); emu:setKeys(0); emu:runFrame()
            frames = frames + 2
        end
        -- Periodic diagnostic
        if frames - lastLogFrames >= 500 then
            local x, y = playerPos()
            local g, n = readMap()
            print(string.format("[states] walkToMap %s: frame=%d map=(%d,%d) pos=(%d,%d)",
                  label, frames, g, n, x, y))
            lastLogFrames = frames
        end
    end
    local g, n = readMap()
    error(string.format("[states] TIMEOUT walking to %s (%d,%d) – on (%d,%d) after %d frames",
          label, m.g, m.n, g, n, maxFrames))
end

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

-- Kill any stale Task_HandleYesNoInput in gTasks.  The task persists across
-- map transitions and can bypass ScriptMenu_YesNo() if not cleaned up.
-- Performs a full linked-list removal (prev.next = next, next.prev = prev)
-- before clearing isActive so CreateTask() can safely reuse the slot.
local function killYesNoTask()
    local STRIDE    = 0x28   -- sizeof(struct Task)
    local NUM_TASKS = 16
    local THUMB_PTR = YESNO_TASK_FUNC | 1   -- GBA stores THUMB ptrs with bit0 set
    local killed    = 0
    for i = 0, NUM_TASKS - 1 do
        local base   = ADDR_GTASKS + i * STRIDE
        local active = emu:read8(base + 4)
        local fptr   = emu:read32(base)
        if active ~= 0 and fptr == THUMB_PTR then
            local prev_s = emu:read8(base + 5)
            local next_s = emu:read8(base + 6)
            emu:write8(ADDR_GTASKS + prev_s * STRIDE + 6, next_s)  -- prev.next = next
            emu:write8(ADDR_GTASKS + next_s * STRIDE + 5, prev_s)  -- next.prev = prev
            emu:write8(base + 4, 0)                                 -- isActive = FALSE
            print(string.format("[states] killed stale Task_HandleYesNoInput slot=%d prev=%d next=%d",
                  i, prev_s, next_s))
            killed = killed + 1
        end
    end
    if killed == 0 then
        print("[states] Task_HandleYesNoInput: no stale instance found")
    end
    return killed
end

-- Wait up to maxWait frames for a Task_HandleYesNoInput to appear in gTasks,
-- pressing A every 20 frames to advance any preceding text box.  When found:
-- press UP (ensures cursor on YES), wait for the 5-frame guard, confirm with A.
-- Returns true if found and confirmed; false on timeout (non-fatal).
local function confirmYesNo(label, maxWait)
    maxWait = maxWait or 600
    local THUMB_PTR = YESNO_TASK_FUNC | 1
    local function yesNoActive()
        for slot = 0, 15 do
            local base = ADDR_GTASKS + slot * TASK_STRIDE
            if emu:read8(base + 4) ~= 0 and emu:read32(base) == THUMB_PTR then
                return true
            end
        end
        return false
    end
    local found = 0
    for i = 1, maxWait do
        emu:setKeys(0); emu:runFrame()
        if yesNoActive() then found = i; break end
        if i % 20 == 0 then press(KEY_A, 1, 3) end
    end
    if found == 0 then
        print(string.format("[states] confirmYesNo(%s): no YES/NO in %d frames (OK)", label, maxWait))
        return false
    end
    print(string.format("[states] confirmYesNo(%s): YES/NO at frame %d", label, found))
    press(KEY_UP, 1, 5)   -- ensure cursor on YES (safe no-op if already there)
    idle(12)              -- let Task_HandleYesNoInput 5-frame guard expire
    press(KEY_A, 2, 10)   -- confirm YES
    print(string.format("[states] confirmYesNo(%s): confirmed", label))
    return true
end

local function spamA(times, gap)
    gap = gap or 6
    for _ = 1, times do press(KEY_A, 2, gap) end
end

-- Hold direction for N tiles (~16f hold + 2f release per tile).
local function walk(dir, tiles)
    for _ = 1, tiles do press(dir, 16, 2) end
end

local function logPos(label)
    local x, y = playerPos()
    local g, n = readMap()
    print(string.format("[states] POS %-24s x=%d y=%d map=(%d,%d)", label, x, y, g, n))
end

local function saveState(name)
    local path = STATES_DIR .. name
    local buf = emu:saveStateBuffer(31)
    assert(buf and #buf > 0, "saveStateBuffer returned empty result")
    local f = assert(io.open(path, "wb"), "cannot open " .. path)
    f:write(buf)
    f:close()
    local g, n = readMap()
    print(string.format("[states] SAVED %-26s (%d bytes)  map=(%d,%d)",
          name, #buf, g, n))
end

-- ── Verify mGBA scripting API ─────────────────────────────────────────────────
if not emu or not emu.runFrame or not emu.setKeys or not emu.saveStateBuffer then
    print("[states] ERROR: mGBA scripting API not available.")
    print("         Load via: mgba-headless --script tools/build_save_states.lua rom.gba")
    os.exit(1)
end

-- ── PHASE 1: Title screen → NEW GAME ─────────────────────────────────────────
-- Nintendo logo (~75f) + GF logo (~175f) + title animation (~350f) = ~600f.
log("phase 1: title → new game")
idle(700)
press(KEY_START, 5, 60)     -- title screen → main menu
idle(90)
press(KEY_A, 5, 60)         -- NEW GAME

-- ── PHASES 2-3: Oak intro speech → gender selection ──────────────────────────
-- All five Oak text boxes (WelcomeToTheWorld…TellMeALittleAboutYourself) use
-- IsTextPrinterActiveOnWindow to auto-advance — no A required.  spamA is
-- harmless (speeds up text rendering) and reaches the gender menu naturally.
log("phase 2-3: oak speech + gender (BOY)")
idle(300)
spamA(12, 50)
idle(90)
press(KEY_A, 5, 60)         -- BOY (cursor at position 0 by default)

-- ── PHASE 4: Player naming screen ────────────────────────────────────────────
-- "Your name, what is it?" auto-advances → fade to black → naming screen opens
-- automatically (DoNamingScreen sets up NAMING_SCREEN_PLAYER via GetDefaultName).
-- START jumps cursor to OK; A confirms the preset name.
log("phase 4: player name")
idle(400)                   -- naming screen initialises (~200f from gender A; buffer)
press(KEY_START, 5, 30)     -- jump cursor to OK
idle(90)
press(KEY_A, 5, 60)         -- confirm preset player name

-- ── PHASE 5: Rival name + bedroom load ───────────────────────────────────────
-- After CB2_ReturnFromNamingScreen, oak_speech resumes:
--   ConfirmName: "So your name is [X]?" (TRUE=needs A) → YES/NO (A for YES)
--   FadeInRivalPic → "What was his name?" (auto) → MoveRivalDisplayNameOptions
--   Menu: position 0=NEW NAME, 1=GREEN, 2=GARY, 3=KAZ, 4=TORU.
--   Press DOWN to move from 0→1; A selects GREEN (skips naming screen).
--   ConfirmName: "So his name is GREEN?" (TRUE) → YES/NO (A for YES)
--   "Remember, his name is GREEN!" (TRUE) → FadeOutRivalPic → ReshowPlayersPic
--   "Let's go [name]!" (TRUE) → FadeOutBGM (30f) → shrink (~100f) → fade (~36f)
--   → CB2_NewGame → bedroom 2F loads.
--
-- waitForMap spams A every 30 frames to advance all remaining dialogue boxes
-- regardless of exact timing, then confirms the bedroom map is active.
log("phase 5: rival name + wait for bedroom")

-- Step 1: Player name confirmation.
-- Oak's confirmname: "So your name is [X]? Is that right?" → YES/NO.
-- confirmYesNo() advances text with A every 20f until YES/NO task appears,
-- then moves cursor to YES and confirms.  350-frame window ends well before
-- the rival name menu appears (~700+ frames from phase 5 start).
idle(60)
confirmYesNo("player name", 350)

-- Step 2: Wait for rival name menu to slide in.
-- After player YES/NO, rival pic fades in (~150f) and preset-name menu slides
-- in (~100f).  No A presses here — pressing A while menu is at position 0
-- (NEW NAME) would open the naming screen and loop forever.
idle(900)

-- Step 3: Select GREEN (position 1 in the preset list).
do
    local ctx3 = emu:read8(ADDR_SCRIPT_STATUS)
    local lk3  = emu:read8(ADDR_FIELD_LOCK)
    local yn3  = false
    for slot = 0, 15 do
        local base = ADDR_GTASKS + slot * TASK_STRIDE
        if emu:read8(base + 4) ~= 0 and emu:read32(base) == (YESNO_TASK_FUNC | 1) then
            yn3 = true; break
        end
    end
    local px3, py3 = playerPos()
    print(string.format("[states] pre-rival-menu: ctx=%d lock=%d yesno=%s tile(%d,%d)",
          ctx3, lk3, tostring(yn3), px3-7, py3-7))
end
press(KEY_DOWN, 3, 20)      -- cursor 0(NEW NAME) → 1(GREEN)
press(KEY_A, 5, 20)         -- select GREEN (skips naming screen)

-- Step 4: Rival name YES/NO.
-- "So his name is GREEN? Is that right?" → confirm YES.
confirmYesNo("rival name", 400)

-- Step 5: "Of course! Remember, his name is GREEN!" → "Let's go [name]!" →
-- CB2_NewGame → bedroom 2F.  spamA=20 advances MSGBOX_DEFAULT boxes quickly.
waitForMap(MAP.HOUSE_2F, "bedroom 2F", 36000, 20)

-- ── PHASE 6: Bedroom → 1F → Pallet Town ──────────────────────────────────────
-- Player spawns at stored (13,13) = tile (6,6) facing north.  Staircase to 1F at (10,2).
-- (10,2) = stored (17,9).  MB_DOWN_LEFT_STAIR_WARP (0xEE) triggers via
-- CheckForPlayerAvatarCollision which checks the CURRENT tile, not the destination.
-- So: step RIGHT 5 + UP 4 to reach stored (18,9) = tile (11,2), then LEFT 2:
--   first LEFT steps onto staircase tile (17,9); second LEFT fires the warp.
-- 1F: arrive at stored (17,9) = tile (10,2); exit at (4,8) = MB_SOUTH_ARROW_WARP.
-- Walk LEFT 6 → DOWN 7: first 6 DOWN steps reach (4,8) = stored (11,15); 7th fires warp.
-- Pallet Town: exit at ~(6,8); Oak coord triggers at (12,1) and (13,1).
log("phase 6: bedroom exit → pallet town")
idle(120)                   -- wait for FieldCB_WarpExitFadeFromBlack to complete
logPos("bedroom start")

walk(KEY_RIGHT, 5)          -- stored (13,13) → (18,13)  [tile (6,6) → (11,6)]
logPos("after RIGHT 5")
walk(KEY_UP,    4)          -- stored (18,13) → (18,9)   [tile (11,6) → (11,2)]
logPos("after UP 4")
walk(KEY_LEFT,  2)          -- step 1: onto staircase (17,9); step 2: warp fires
logPos("after LEFT 2")
waitForMap(MAP.HOUSE_1F, "house 1F", 600)
idle(30)

walk(KEY_LEFT,  6)          -- stored (17,9) → (11,9)  [tile (10,2) → (4,2)]
walk(KEY_DOWN,  7)          -- step 1-6: reach (11,15) = tile (4,8); step 7: south warp fires
waitForMap(MAP.PALLET, "Pallet Town", 600)
idle(30)

-- ── PHASE 6b: Force solo mode, clear VAR_OAK, walk to Oak trigger ─────────────
-- gMultiplayerState.connState is set to MP_STATE_CONNECTED (2) by Multiplayer_Init
-- (called at game boot).  In headless mode there is no Tauri app, so connState
-- stays CONNECTED; this causes OakTrigger and WaitForPartnerStarter to take the
-- "Waiting for Partner" path and block forever.
--
-- Fix: scan IWRAM for gMpAddrTable[0] = MP_DISCOVERY_MAGIC, read gMpAddrTable[1]
-- = &gMultiplayerState, then write connState = MP_STATE_DISCONNECTED (0).
-- With connState==0, Multiplayer_IsConnected() returns 0 → all solo paths are
-- taken, and waitbossstart/waitstarterpick resolve immediately.
log("phase 6b: force solo mode + inject VAR_LAB=1, walk to lab entrance")

-- Offsets verified from GetVarPointer disassembly (0x08119ae8):
--   address = gSaveBlock1Ptr + (id + 0xFFFFC9C8) * 2  = gSaveBlock1Ptr + (id - 0x3638) * 2
-- struct comment says vars at 0x139C but compiled code puts vars[0] at ptr+0x1390 (12 bytes earlier)
local OAK_OFF = (0x4050 - 0x3638) * 2   -- 0x1430; VAR_MAP_SCENE_PALLET_TOWN_OAK
local LAB_OFF = (0x4055 - 0x3638) * 2   -- 0x143A; VAR_MAP_SCENE_PALLET_TOWN_PROFESSOR_OAKS_LAB
local function readVar(off)
    local ptr = emu:read32(ADDR_SB1_PTR)
    return ptr ~= 0 and emu:read16(ptr + off) or -1
end

-- Scan IWRAM for MP_DISCOVERY_MAGIC → get &gMultiplayerState → patch connState.
local MP_DISCOVERY_MAGIC = 0xC0DEC0DE
local mp_addr = nil
for scan = 0x03000000, 0x03008000, 4 do
    if emu:read32(scan) == MP_DISCOVERY_MAGIC then
        mp_addr = emu:read32(scan + 4)   -- gMpAddrTable[1] = &gMultiplayerState
        break
    end
end
if mp_addr then
    emu:write8(mp_addr + 1, 0)   -- connState = MP_STATE_DISCONNECTED (0); offset 1 in struct
    print(string.format("[states] gMultiplayerState at 0x%08x  connState->%d",
          mp_addr, emu:read8(mp_addr + 1)))
else
    error("[states] MP_DISCOVERY_MAGIC not found in IWRAM — Multiplayer_Init may not have run")
end

idle(270)   -- settle time for Pallet Town warp-exit walk-out animation

-- Bypass the OakTrigger escort entirely:
--   The escort (PalletTown_Frlg/scripts.inc lines 190-229) fails because the
--   player naturally steps on the warp tile at (16,13) during applymovement,
--   warping to the lab BEFORE the script reaches setvar VAR_LAB,1.  The script
--   then runs closedoor/waitdooranim on the wrong (now-unloaded) map and hangs.
-- Fix: write VAR_LAB=1 directly from Lua while still in Pallet Town so that
--   ON_TRANSITION positions Oak at (6,11) and ON_FRAME fires ChooseStarterScene
--   as soon as the lab loads.  Write VAR_OAK=1 so the OakTrigger coord event at
--   tile(12,1) does not fire while we navigate to the lab entrance.
-- All FRLG hide flags share flag 0 (FLAG_HIDE_OAK_IN_PALLET_TOWN,
--   FLAG_HIDE_OAK_IN_HIS_LAB, FLAG_HIDE_RIVAL_IN_LAB are all #defined to 0).
-- Keep flag 0 cleared every frame (inlined below) so all lab NPCs stay visible.
do
    local ptr = emu:read32(ADDR_SB1_PTR); local g, n = readMap()
    if ptr ~= 0 then
        local old_oak = emu:read16(ptr + OAK_OFF)
        local old_lab = emu:read16(ptr + LAB_OFF)
        emu:write16(ptr + OAK_OFF, 1)   -- disables OakTrigger coord event
        emu:write16(ptr + LAB_OFF, 1)   -- triggers ChooseStarterScene on entry
        print(string.format("[states] VAR_OAK %d->1  VAR_LAB %d->1  map=(%d,%d)",
              old_oak, old_lab, g, n))
    end
end

do local x, y = playerPos(); local g, n = readMap()
   print(string.format("[states] pallet spawn x=%d y=%d tile(%d,%d) map=(%d,%d)",
         x, y, x-7, y-7, g, n)) end

-- Lab NPC hide flags inlined into wait loops — callbacks:add not available headless.
-- In FRLG mode flags 0x028-0x02D are all in byte 5 of the flag array:
--   bit0=FLAG_HIDE_BULBASAUR_BALL(0x028) bit1=FLAG_HIDE_SQUIRTLE_BALL(0x029)
--   bit2=FLAG_HIDE_CHARMANDER_BALL(0x02A) bit3=FLAG_HIDE_OAK_IN_HIS_LAB(0x02B)
--   bit4=FLAG_HIDE_OAK_IN_PALLET_TOWN(0x02C) bit5=FLAG_HIDE_RIVAL_IN_LAB(0x02D)
local FLAG_BYTE_OFF = 0x1275   -- gSaveBlock1+0x1275 byte 5; bits 0-5 = lab NPC hide flags

-- Walk to the lab entrance warp at tile(16,13) via the passable corridor.
-- The escort script (PlayerWalkToLabLeft) reveals: x=11 column is open from y=3
-- to y=14; then RIGHT to (16,14); then UP onto the warp at (16,13).
-- VAR_OAK=1 prevents OakTrigger at tile(12,1) from firing during navigation.
-- Clearing bits 0-5 of FLAG_BYTE_OFF ensures Oak/Rival/Balls are all visible.
walk(KEY_RIGHT, 5)   -- tile(6,8) → tile(11,8)  [house row, known passable]
walk(KEY_DOWN,  6)   -- tile(11,8) → tile(11,14) [x=11 corridor, open to y≥14]
walk(KEY_RIGHT, 5)   -- tile(11,14) → tile(16,14)
walk(KEY_UP,    1)   -- tile(16,14) → tile(16,13) → lab warp fires
do local x,y=playerPos(); local g,n=readMap()
   print(string.format("[states] pre-warp tile(%d,%d) map=(%d,%d)",
         x-7, y-7, g, n)) end

-- ── PHASE 7: ChooseStarterScene → approach ball ───────────────────────────────
-- With VAR_LAB=1 injected, ON_TRANSITION positions Oak at (6,11) and ON_FRAME
-- fires ChooseStarterScene: Oak UP 6, Player UP 8 → 4 msgboxes → setvar VAR_LAB,2.
-- Player ends at tile(6,4) facing north.  Keep bits 0-5 cleared so all NPCs visible.
log("phase 7: enter oak's lab → approach ball")

-- Wait for the lab map to load (natural warp fires when UP 1 hits tile 16,13).
-- Clear bits 0-5 every frame so Oak/Rival/Balls are not hidden on entry.
print("[states] waiting for Oak's Lab...")
for i = 1, 36000 do
    emu:setKeys(0)
    emu:runFrame()
    local fptr = emu:read32(ADDR_SB1_PTR)
    if fptr ~= 0 then
        local b = emu:read8(fptr + FLAG_BYTE_OFF)
        if (b % 64) ~= 0 then emu:write8(fptr + FLAG_BYTE_OFF, b - (b % 64)) end
    end
    local g2, n2 = readMap()
    if g2 == MAP.OAKS_LAB.g and n2 == MAP.OAKS_LAB.n then
        print(string.format("[states] MAP %-24s (%2d,%2d) after %5d frames",
              "Oak's Lab", g2, n2, i))
        break
    end
    if i % 20 == 0 then
        emu:setKeys(KEY_A); emu:runFrame(); emu:setKeys(0)
    end
    if i == 36000 then error("[states] TIMEOUT waiting for Oak's Lab") end
end

-- ── PHASE 7: Let ChooseStarterScene complete, navigate to Bulbasaur ball ───────
-- Now that VAR_LAB=1 is written to the correct address (0x143A via GetVarPointer
-- disassembly), ON_FRAME triggers ChooseStarterScene: lockall → Oak UP 6 →
-- Player UP 8 to tile(6,4) → 4 msgboxes → setvar VAR_LAB,2 → releaseall; end.
-- After scene: navigate DOWN 1 + RIGHT 2 + UP 1(blocked by ball) to reach (8,5)N.
log("phase 7: ChooseStarterScene → approach ball")

idle(20)   -- warp-exit animation + ON_WARP settle

do local x,y=playerPos(); local g,n=readMap()
   print(string.format("[states] lab entry tile(%d,%d) map=(%d,%d)", x-7,y-7,g,n)) end

-- Set text speed to FAST so the 4 scene dialogs advance quickly
do
    local sb2 = emu:read32(ADDR_SB2_PTR)
    if sb2 ~= 0 then
        local opt = emu:read16(sb2 + 0x14)
        emu:write16(sb2 + 0x14, (opt & 0xFFF8) | 2)
        print(string.format("[states] text speed -> FAST (sb2=0x%08x opts16=0x%04x)",
              sb2, emu:read16(sb2 + 0x14)))
    end
end

-- Wait for scene to finish (ctx=SHUTDOWN, lock=0, VAR_LAB=2).
-- Press A every 30 frames to advance the 4 dialog boxes.  Scene takes ~700-900f.
for i = 1, 3600 do
    emu:setKeys(0)
    emu:runFrame()
    local fptr = emu:read32(ADDR_SB1_PTR)
    if fptr ~= 0 then
        local b = emu:read8(fptr + FLAG_BYTE_OFF)
        if (b % 64) ~= 0 then emu:write8(fptr + FLAG_BYTE_OFF, b - (b % 64)) end
    end
    local ctx  = emu:read8(ADDR_SCRIPT_STATUS)
    local lock = emu:read8(ADDR_FIELD_LOCK)
    if i > 200 and ctx == 2 and lock == 0 and readVar(LAB_OFF) == 2 then
        local x, y = playerPos()
        print(string.format("[states] scene done i=%d tile(%d,%d) VAR_LAB=%d",
              i, x-7, y-7, readVar(LAB_OFF)))
        break
    end
    if i % 30 == 0 then
        emu:setKeys(KEY_A); emu:runFrame(); emu:setKeys(0)
    end
    if i == 3600 then error("[states] TIMEOUT waiting for ChooseStarterScene") end
end

-- Final NPC hide flag clear
do
    local fptr = emu:read32(ADDR_SB1_PTR)
    if fptr ~= 0 then
        local b = emu:read8(fptr + FLAG_BYTE_OFF)
        if (b % 64) ~= 0 then emu:write8(fptr + FLAG_BYTE_OFF, b - (b % 64)) end
        print("[states] NPC hide flags cleared")
    end
end

-- From tile(6,4): DOWN 1 → (6,5); RIGHT 2 → (8,5); UP 1 blocked by ball → (8,5)N
walk(KEY_DOWN,  1)
walk(KEY_RIGHT, 2)
walk(KEY_UP,    1)   -- (8,4)=ball blocks; player stays at (8,5) facing N

do local x,y=playerPos(); local g,n=readMap()
   print(string.format("[states] at ball tile(%d,%d) VAR_LAB=%d",
         x-7,y-7,readVar(LAB_OFF))) end

-- ── CHECKPOINT 1: oaks_lab.ss1 ────────────────────────────────────────────────
-- Player at tile(8,5) facing north; Bulbasaur ball at tile(8,4).  VAR_LAB=2.
log("checkpoint 1: oaks_lab.ss1")
saveState("oaks_lab.ss1")

-- ── PHASE 8: Choose Bulbasaur → exit lab ────────────────────────────────────
-- OakChoosingBulbasaur text has a \p page break that requires A to advance.
-- The YESNO box uses `yesnobox` which sets sGlobalScriptContextStatus=WAITING(1).
-- Strategy: poll ctx each frame; press A periodically to advance page breaks.
-- ctx=1 reliably means the yesnobox (YESNO menu) is showing.
-- Task_HandleYesNoInput has a 5-frame guard; we wait 10f after ctx=1 before
-- pressing YES/NO so the guard is guaranteed to have cleared.
--
-- After picking Bulbasaur (VAR_LAB→3) we bump VAR_LAB to 4 to bypass the
-- rival-battle coord trigger at (7,8) which fires only when VAR_LAB==3.
log("phase 8: pick starter + exit lab")

-- Set text speed to FAST (1f/char) so page breaks advance quickly.
-- gSaveBlock2Ptr @ ADDR_SB2_PTR; optionsTextSpeed:3 is bits[2:0] of u16 at sb2+0x14.
do
    local sb2 = emu:read32(ADDR_SB2_PTR)
    if sb2 ~= 0 then
        local opt = emu:read16(sb2 + 0x14)
        emu:write16(sb2 + 0x14, (opt & 0xFFF8) | 2)
        print(string.format("[states] text speed -> FAST (sb2=0x%08x opts16=0x%04x)",
              sb2, emu:read16(sb2 + 0x14)))
    end
end

-- Diagnostic dump before interaction.
do
    local lock0 = emu:read8(ADDR_FIELD_LOCK)
    local ctx0  = emu:read8(ADDR_SCRIPT_STATUS)
    local px, py = playerPos()
    print(string.format("[states] PRE-P8 lock=%d ctx=%d tile(%d,%d) VAR_LAB=%d",
          lock0, ctx0, px-7, py-7, readVar(LAB_OFF)))
    if lock0 == 1 then
        print("[states] WARNING: stale lock — clearing to 0")
        emu:write8(ADDR_FIELD_LOCK, 0)
    end
end

-- Kill any stale Task_HandleYesNoInput.  If still alive from an earlier phase
-- the yesnobox command returns immediately without waiting for input, causing
-- the ball script to hit the bare `end` with VAR_RESULT=0xFF (neither YES/NO).
killYesNoTask()

-- Trigger ball interaction.  Retry A until sLockFieldControls→1.
local ball_started = false
for try = 1, 90 do
    press(KEY_A, 3, 12)
    if emu:read8(ADDR_FIELD_LOCK) == 1 then
        print(string.format("[states] ball lock at try=%d", try))
        ball_started = true
        break
    end
end
if not ball_started then
    error("[states] Bulbasaur ball never triggered after 90 tries")
end

-- ── Step A: wait for "Do you want BULBASAUR?" YESNO (ctx=1=CONTEXT_WAITING) ──
-- The text has 2 pages separated by \p (~57f each at FAST).  Press A every 60f
-- to advance the page break.  ctx=1 signals yesnobox paused the script.
do
    -- DIAGNOSTIC: trace ctx for 200 frames to understand timing
    local prev_ctx = emu:read8(ADDR_SCRIPT_STATUS)
    local seen = false
    for i = 1, 600 do
        emu:setKeys(0); emu:runFrame()
        local c = emu:read8(ADDR_SCRIPT_STATUS)
        if c ~= prev_ctx then
            print(string.format("[states] P8 ctx %d→%d at i=%d lock=%d VAR_LAB=%d",
                  prev_ctx, c, i, emu:read8(ADDR_FIELD_LOCK), readVar(LAB_OFF)))
            prev_ctx = c
        end
        if c == 1 then
            print(string.format("[states] P8: 'want Bulbasaur?' YESNO at i=%d", i))
            seen = true
            break
        end
        if i % 60 == 0 then press(KEY_A, 1, 3) end  -- advance \p page break
    end
    if not seen then error("[states] 'want Bulbasaur?' YESNO never appeared") end
end
idle(10)            -- let 5-frame guard in Task_HandleYesNoInput expire
press(KEY_A, 2, 5)  -- YES to "Do you want BULBASAUR?"
-- ctx → 0 (ChoseStarter running: hidemonpic, removeobject, energetic msgbox...)

-- ── Step B: advance energetic msgbox, then wait for nickname YESNO (ctx=1) ──
-- "This POKéMON is really quite energetic!" uses MSGBOX_DEFAULT (waitbuttonpress).
-- After that: givemon + received text (~37f) + fanfare (170f) + nickname yesnobox.
-- Press A every 20f to advance the energetic waitbuttonpress and any page breaks.
do
    local seen = false
    for i = 1, 500 do
        emu:setKeys(0); emu:runFrame()
        if emu:read8(ADDR_SCRIPT_STATUS) == 1 then
            print(string.format("[states] P8: nickname YESNO at i=%d", i))
            seen = true
            break
        end
        if i % 20 == 0 then press(KEY_A, 1, 3) end  -- advance energetic waitbuttonpress
    end
    if not seen then error("[states] nickname YESNO never appeared") end
end
idle(10)            -- 5-frame guard
press(KEY_B, 2, 5)  -- NO to "Give it a nickname?"
-- Script: WaitForPartnerStarter → RivalPicksStarter → RivalWalksToSquirtle →
--         waitmovement (~88f) → RivalTakesStarter → msgbox MSGBOX_DEFAULT →
--         received waitmessage → fanfare (170f) → setvar VAR_LAB=3 → release → end

-- ── Step C: advance rival dialog, wait for VAR_LAB=3 and lock=0 ──────────────
-- Press A every 20f to advance the rival's MSGBOX_DEFAULT waitbuttonpress.
do
    local done = false
    for i = 1, 700 do
        emu:setKeys(0); emu:runFrame()
        if readVar(LAB_OFF) == 3 and emu:read8(ADDR_FIELD_LOCK) == 0 then
            print(string.format("[states] P8: VAR_LAB=3 lock=0 at i=%d", i))
            done = true
            break
        end
        if i % 20 == 0 then press(KEY_A, 1, 3) end  -- advance rival waitbuttonpress
    end
    if not done then
        local lk = emu:read8(ADDR_FIELD_LOCK)
        local vl = readVar(LAB_OFF)
        print(string.format("[states] P8 FAIL: lock=%d ctx=%d VAR_LAB=%d",
              lk, emu:read8(ADDR_SCRIPT_STATUS), vl))
        error("[states] Phase 8 failed: VAR_LAB never reached 3")
    end
end
print(string.format("[states] P8 done: lock=%d ctx=%d VAR_LAB=%d",
      emu:read8(ADDR_FIELD_LOCK), emu:read8(ADDR_SCRIPT_STATUS), readVar(LAB_OFF)))

-- ── CHECKPOINT 1b: p1_rivals_lab.ss1 ─────────────────────────────────────────
-- P1 has Bulbasaur; VAR_LAB=3; rival battle coord trigger at tile(7,8) is live.
-- MCP relay injects PARTNER_CONNECTED on boot; test navigates both to tile(7,8).
saveState("p1_rivals_lab.ss1")

-- Bump VAR_LAB to 4 so the rival-battle coord trigger at (7,8) (fires ==3) is skipped.
do
    local fptr = emu:read32(ADDR_SB1_PTR)
    emu:write16(fptr + LAB_OFF, 4)
    print("[states] rival battle bypassed: VAR_LAB set to 4")
end

idle(10)  -- settle after script release
do local x,y=playerPos(); print(string.format("[states] EXIT pre-walk tile(%d,%d)", x-7, y-7)) end
-- Move to column 6 then approach warp with continuous DOWN hold so the
-- warp-tile step detection runs naturally (no mid-step key gaps).
walk(KEY_LEFT, 2)           -- (8,5) → (6,5)
do local x,y=playerPos(); print(string.format("[states] EXIT after LEFT 2 tile(%d,%d)", x-7, y-7)) end
idle(5)
-- Hold DOWN until the map changes (warp fires) or timeout.
-- Warps at y=12; from y=5 that is 7 tiles = ~112f. Hold for 300f.
do
    local fired = false
    for i = 1, 300 do
        emu:setKeys(KEY_DOWN)
        emu:runFrame()
        local g, n = readMap()
        if g == 37 and n == 0 then
            print(string.format("[states] EXIT warp fired at hold-frame %d", i))
            emu:setKeys(0)
            fired = true
            break
        end
        if i % 30 == 0 then
            local px, py = playerPos()
            local lk = emu:read8(ADDR_FIELD_LOCK)
            local ct = emu:read8(ADDR_SCRIPT_STATUS)
            print(string.format("[states] EXIT hold i=%d tile(%d,%d) lock=%d ctx=%d",
                  i, px-7, py-7, lk, ct))
        end
    end
    if not fired then
        emu:setKeys(0)
        local px, py = playerPos()
        print(string.format("[states] EXIT: still in lab after hold, tile(%d,%d)", px-7, py-7))
    end
end
waitForMap(MAP.PALLET, "Pallet Town (after lab)", 18000, 15)

-- ── PHASE 9: Pallet Town → Route 1 ───────────────────────────────────────────
-- Lab exit warp arrives at tile(16,14); Route 1 is north of Pallet Town (height=20).
-- OakTrigger coord events fire only for VAR_OAK==0 (our VAR_OAK=1) so north is clear.
-- Use continuous key hold (no per-tile gaps) to ensure seamless map-edge connection fires.
log("phase 9: pallet town → route 1")
idle(120)                   -- wait for lab-exit walk-out animation + map fade
do local x,y=playerPos(); local g,n=readMap()
   print(string.format("[states] pallet post-lab tile(%d,%d) map=(%d,%d)", x-7,y-7,g,n)) end

-- Path to Route 1:
--   x=13: lab building wall blocks northward from y=14 (lab west wall at x=13)
--   x=12: open from y=14 to y=3, then sign lady blocks at y=2
--   Fix: walk at x=12 to y≈3, step RIGHT to x=13, then UP through (13,2)
--        SignLadyTrigger (VAR_TEMP_2==1) shows tutorial then releaseall,
--        and (13,1) OakTrigger only fires for VAR_OAK==0 (ours=1 → clear).
-- From tile(16,14): LEFT 4 → (12,14).
walk(KEY_LEFT, 4)           -- (16,14) → (12,14)
do local x,y=playerPos(); print(string.format("[states] pallet after LEFT 4 tile(%d,%d)", x-7,y-7)) end
idle(5)
-- Phase A: hold UP from y=14 until stopped by sign lady at y=2 (~200 frames).
do
    for i = 1, 220 do
        emu:setKeys(KEY_UP)
        emu:runFrame()
        local g, n = readMap()
        if g == 37 and n == 19 then   -- reached Route 1 already?
            print("[states] Route 1 entered early (unexpected)")
            emu:setKeys(0)
            goto done_pallet
        end
    end
    emu:setKeys(0)
end
do local x,y=playerPos(); print(string.format("[states] pallet stopped at tile(%d,%d)", x-7,y-7)) end
-- Phase B: step RIGHT 1 to x=13 (clear of sign lady), then UP to trigger at (13,2).
walk(KEY_RIGHT, 1)          -- (12,≈3) → (13,≈3)
do local x,y=playerPos(); print(string.format("[states] pallet after RIGHT 1 tile(%d,%d)", x-7,y-7)) end
idle(5)
-- Phase C: hold UP with A spam to fire SignLadyTrigger at (13,2) then exit north.
do
    local reached = false
    for i = 1, 600 do
        emu:setKeys(KEY_UP)
        emu:runFrame()
        local g, n = readMap()
        if g == 37 and n == 19 then
            print(string.format("[states] Route 1 entered at phase-C frame %d", i))
            emu:setKeys(0)
            reached = true
            break
        end
        if i % 20 == 0 then
            emu:setKeys(KEY_A); emu:runFrame(); emu:setKeys(0)
        end
        if i % 60 == 0 then
            local px, py = playerPos()
            local lk = emu:read8(ADDR_FIELD_LOCK)
            local ct = emu:read8(ADDR_SCRIPT_STATUS)
            print(string.format("[states] pallet C i=%d tile(%d,%d) lock=%d ctx=%d",
                  i, px-7, py-7, lk, ct))
        end
    end
    if not reached then
        emu:setKeys(0)
        local px, py = playerPos()
        print(string.format("[states] STILL in Pallet after phase C, tile(%d,%d)", px-7,py-7))
    end
end
::done_pallet::
waitForMap(MAP.ROUTE1, "Route 1", 3600, 15)

-- ── CHECKPOINT 2: tall_grass_route1.ss1 ──────────────────────────────────────
-- Player enters Route 1 at tile(13, 39).  The route's navigable corridor runs
-- along x≈6–12 (map connection to Viridian uses offset -12, so Route 1 col 12
-- aligns with Viridian col 0).  Column x=13 is blocked by trees around y=31.
-- The player enters directly from Pallet Town at x=13 (offset 0 connection), so
-- just walk north from the entry; the checkpoint is near tall grass at y≈36.
idle(90)
do
    for i = 1, 300 do
        local lock = emu:read8(ADDR_FIELD_LOCK)
        local ctx  = emu:read8(ADDR_SCRIPT_STATUS)
        if lock == 0 and ctx ~= 1 then break end
        press(KEY_A, 2, 5)
        if i % 30 == 0 then
            local px, py = playerPos()
            print(string.format("[states] route1 dismiss i=%d lock=%d ctx=%d tile(%d,%d)", i, lock, ctx, px-7, py-7))
        end
    end
end
do local x,y = playerPos(); print(string.format("[states] route1 entry tile(%d,%d)", x-7, y-7)) end
walk(KEY_UP, 3)   -- (13,39) → (13,36): into tall grass band
do local x,y = playerPos(); print(string.format("[states] route1 pre-save tile(%d,%d)", x-7, y-7)) end
idle(30)
saveState("tall_grass_route1.ss1")
saveState("p1_route1.ss1")   -- same state, named for explicit p1 test use

-- ── PHASE 8b: reload Oak's Lab → pick Charmander → Route 1 ──────────────────
-- Reload oaks_lab.ss1 (player at tile(8,5), VAR_LAB=2, connState=0).
-- Navigate RIGHT 2 → UP 1 to face Charmander ball at tile(10,4).
-- Then follow the same pick/exit sequence as Phase 8.
log("phase 8b: reload lab → pick charmander → p2_route1.ss1")

do
    local f = assert(io.open(STATES_DIR .. "oaks_lab.ss1", "rb"),
                     "oaks_lab.ss1 not found — run phase 1-7 first")
    local buf = f:read("*all")
    f:close()
    emu:loadStateBuffer(buf)
end
idle(60)

do
    local sb2 = emu:read32(ADDR_SB2_PTR)
    if sb2 ~= 0 then
        local opt = emu:read16(sb2 + 0x14)
        emu:write16(sb2 + 0x14, (opt & 0xFFF8) | 2)
        print("[states] text speed -> FAST")
    end
end

-- Navigate to Charmander: RIGHT 2 from (8,5) → (10,5); UP 1 blocked by (10,4)
walk(KEY_RIGHT, 2)
walk(KEY_UP, 1)
do local x,y = playerPos()
   print(string.format("[states] at Charmander ball tile(%d,%d)", x-7, y-7)) end

-- ── CHECKPOINT p2_oaks_lab.ss1 ───────────────────────────────────────────────
-- P2 at Charmander ball; VAR_LAB=2; no starter picked yet.
-- Pair with oaks_lab.ss1 for P1 to test the full co-op starter selection flow.
saveState("p2_oaks_lab.ss1")

do
    local lock0 = emu:read8(ADDR_FIELD_LOCK)
    if lock0 == 1 then
        print("[states] WARNING: stale lock — clearing")
        emu:write8(ADDR_FIELD_LOCK, 0)
    end
end

killYesNoTask()

local charm_started = false
for try = 1, 90 do
    press(KEY_A, 3, 12)
    if emu:read8(ADDR_FIELD_LOCK) == 1 then
        print(string.format("[states] charmander ball lock at try=%d", try))
        charm_started = true
        break
    end
end
if not charm_started then
    error("[states] Charmander ball never triggered after 90 tries")
end

-- Wait for "Do you want CHARMANDER?" YESNO (ctx=1)
do
    local seen = false
    for i = 1, 600 do
        emu:setKeys(0); emu:runFrame()
        if emu:read8(ADDR_SCRIPT_STATUS) == 1 then
            seen = true
            print(string.format("[states] charmander: YESNO at i=%d", i))
            break
        end
        if i % 60 == 0 then press(KEY_A, 1, 3) end
    end
    if not seen then error("[states] Charmander YESNO never appeared") end
end
idle(10)
press(KEY_A, 2, 5)   -- YES to "Do you want CHARMANDER?"

-- Wait for nickname YESNO
do
    local seen = false
    for i = 1, 500 do
        emu:setKeys(0); emu:runFrame()
        if emu:read8(ADDR_SCRIPT_STATUS) == 1 then
            seen = true
            print(string.format("[states] charmander: nickname YESNO at i=%d", i))
            break
        end
        if i % 20 == 0 then press(KEY_A, 1, 3) end
    end
    if not seen then error("[states] charmander nickname YESNO never appeared") end
end
idle(10)
press(KEY_B, 2, 5)   -- NO (no nickname)

-- Wait for VAR_LAB=3 and lock=0
do
    local done = false
    for i = 1, 700 do
        emu:setKeys(0); emu:runFrame()
        if readVar(LAB_OFF) == 3 and emu:read8(ADDR_FIELD_LOCK) == 0 then
            print(string.format("[states] charmander: VAR_LAB=3 lock=0 at i=%d", i))
            done = true
            break
        end
        if i % 20 == 0 then press(KEY_A, 1, 3) end
    end
    if not done then
        error("[states] Charmander pick failed: VAR_LAB never reached 3")
    end
end

-- ── CHECKPOINT p2_rivals_lab.ss1 ─────────────────────────────────────────────
-- P2 has Charmander; VAR_LAB=3; rival battle coord trigger at tile(7,8) is live.
saveState("p2_rivals_lab.ss1")

-- Bypass rival battle coord trigger
do
    local fptr = emu:read32(ADDR_SB1_PTR)
    emu:write16(fptr + LAB_OFF, 4)
    print("[states] rival battle bypassed: VAR_LAB set to 4")
end

idle(120)  -- extra wait for rival's pick animation to fully settle
-- The rival walks to the Bulbasaur ball (tile ≈ 8-9, y=4-5) after the Charmander
-- pick. Stepping DOWN 1 tile first clears that blocking row before going LEFT.
do
    local cx, cy = playerPos()
    print(string.format("[states] charmander EXIT pre-walk tile(%d,%d)", cx-7, cy-7))
end
walk(KEY_DOWN, 1)   -- (10,5) → (10,6): below rival's ball row
-- Now align to column 6 from the new y=6 row (rival can't block here)
do
    local cx, cy = playerPos()
    local tx = cx - 7
    print(string.format("[states] charmander after DOWN 1: tile(%d,%d)", tx, cy-7))
    if tx > 6 then walk(KEY_LEFT, tx - 6)
    elseif tx < 6 then walk(KEY_RIGHT, 6 - tx) end
end
do local x,y = playerPos()
   print(string.format("[states] charmander at column 6: tile(%d,%d)", x-7, y-7)) end
-- walkToMap handles A-spam for any exit dialogue
walkToMap(KEY_DOWN, MAP.PALLET, "Pallet Town (charmander)", 18000)

-- Pallet Town → Route 1 (same path as phase 9)
idle(120)
walk(KEY_LEFT, 4)   -- (16,14) → (12,14)
idle(5)
do
    local reached = false
    -- Phase A: hold UP until stopped by sign lady at y≈3 or until Route 1
    for i = 1, 220 do
        emu:setKeys(KEY_UP); emu:runFrame()
        local g, n = readMap()
        if g == MAP.ROUTE1.g and n == MAP.ROUTE1.n then
            emu:setKeys(0); reached = true; break
        end
    end
    emu:setKeys(0)
    if not reached then
        -- Phase B: step RIGHT 1 to bypass sign lady column
        walk(KEY_RIGHT, 1)
        idle(5)
        -- Phase C: hold UP with A spam until Route 1
        for i = 1, 600 do
            emu:setKeys(KEY_UP); emu:runFrame()
            local g, n = readMap()
            if g == MAP.ROUTE1.g and n == MAP.ROUTE1.n then
                emu:setKeys(0); reached = true; break
            end
            if i % 20 == 0 then emu:setKeys(KEY_A); emu:runFrame(); emu:setKeys(0) end
        end
        emu:setKeys(0)
    end
end
waitForMap(MAP.ROUTE1, "Route 1 (charmander)", 3600, 15)

idle(90)
do
    for i = 1, 300 do
        local lock = emu:read8(ADDR_FIELD_LOCK)
        local ctx  = emu:read8(ADDR_SCRIPT_STATUS)
        if lock == 0 and ctx ~= 1 then break end
        press(KEY_A, 2, 5)
    end
end
walk(KEY_UP, 3)   -- into tall grass band
idle(30)

-- ── CHECKPOINT p2: p2_route1.ss1 ──────────────────────────────────────────────
saveState("p2_route1.ss1")

-- ── PHASE 10: Route 1 → Viridian → Forest → Pewter Gym ───────────────────────
-- Disable encounter randomizer (gCoopSettings.randomizeEncounters = 0) so the
-- starter doesn't get wiped by a high-level randomized Pokémon and black out.
-- Also inject a Super Repel (VAR_REPEL_STEP_COUNT = 9999) to suppress encounters.
log("phase 10: to pewter gym")
if ADDR_COOP_SETTINGS and ADDR_COOP_SETTINGS ~= 0 then
    emu:write8(ADDR_COOP_SETTINGS, 0)   -- randomizeEncounters = OFF
    log("encounter randomizer disabled for navigation")
end
-- Set infinite repel via VAR_REPEL_STEP_COUNT (0x4021).
-- Formula verified from GetVarPointer disassembly: address = gSaveBlock1Ptr + (id - 0x3638) * 2
local REPEL_OFF = (0x4021 - 0x3638) * 2   -- 0x13D2
do
    local sb1 = emu:read32(ADDR_SB1_PTR)
    if sb1 ~= 0 then
        emu:write16(sb1 + REPEL_OFF, 9999)   -- VAR_REPEL_STEP_COUNT = 9999 steps
        log("repel injected: 9999 steps")
    end
end

-- Route 1 zigzag path physically verified by probe through all ledge/cliff barriers.
-- Key terrain: y=20 cliff has a gap at x=9; eastern corridor is x=10-21 from y=17;
-- the only Viridian exit is x=10-13 at y=0 (metatile strip), reached via x=13,y=2.
local function reinjRepel()
    local sb1 = emu:read32(ADDR_SB1_PTR)
    if sb1 ~= 0 then emu:write16(sb1 + REPEL_OFF, 9999) end
    if ADDR_COOP_SETTINGS and ADDR_COOP_SETTINGS ~= 0 then
        emu:write8(ADDR_COOP_SETTINGS, 0)
    end
end
print(string.format("[states] phase10 start tile(%d,%d)", ({playerPos()})[1]-7, ({playerPos()})[2]-7))
walk(KEY_UP,    4);  reinjRepel()  -- tile(13,36) → tile(13,32)
walk(KEY_LEFT,  5);  reinjRepel()  -- tile(13,32) → tile(8,32)
walk(KEY_UP,    5);  reinjRepel()  -- tile(8,32)  → tile(8,27)
walk(KEY_RIGHT, 4);  reinjRepel()  -- tile(8,27)  → tile(12,27)
walk(KEY_UP,    6);  reinjRepel()  -- tile(12,27) → tile(12,21)
walk(KEY_LEFT,  3);  reinjRepel()  -- tile(12,21) → tile(9,21)
walk(KEY_UP,    1);  reinjRepel()  -- tile(9,21)  → tile(9,20)  [gap in y=20 cliff]
walk(KEY_RIGHT, 1);  reinjRepel()  -- tile(9,20)  → tile(10,20)
walk(KEY_UP,    3);  reinjRepel()  -- tile(10,20) → tile(10,17)
walk(KEY_RIGHT, 11); reinjRepel()  -- tile(10,17) → tile(21,17)
walk(KEY_UP,    15); reinjRepel()  -- tile(21,17) → tile(21,2)
walk(KEY_LEFT,  8);  reinjRepel()  -- tile(21,2)  → tile(13,2)  [Viridian exit strip x=10-13]
print(string.format("[states] phase10 at Viridian exit tile(%d,%d)", ({playerPos()})[1]-7, ({playerPos()})[2]-7))
walkToMap(KEY_UP, MAP.VIRIDIAN, "Viridian City", 7200)
-- Skip the Viridian old-man tutorial (coord trigger at tile 22,11 fires when
-- VAR_MAP_SCENE_VIRIDIAN_CITY_OLD_MAN == 0).  Set it to 2 so the old man
-- stands aside and both triggers are inactive.
do
    local sb1 = emu:read32(ADDR_SB1_PTR)
    if sb1 ~= 0 then
        local OLD_MAN_OFF = (0x4051 - 0x3638) * 2   -- VAR_MAP_SCENE_VIRIDIAN_CITY_OLD_MAN
        emu:write16(sb1 + OLD_MAN_OFF, 2)
        log("VAR_MAP_SCENE_VIRIDIAN_CITY_OLD_MAN set to 2 (tutorial done)")
    end
end
-- Player enters Viridian at tile(26,39); x=22 is the only fully unobstructed
-- north-south corridor (no walls y=0-39). Walk left 4 to x=22 then go north.
walk(KEY_LEFT, 4); reinjRepel()
walkToMap(KEY_UP, MAP.ROUTE2,   "Route 2",        7200)
-- Let any in-progress tile animation complete so playerPos() returns the settled
-- entry tile.  Without this pause the first walk() call after a map transition
-- consumes the tail of the animation and moves 1 fewer tile than expected.
idle(60)
-- Boost lead Pokémon's unencrypted level field so the Super Repel suppresses all
-- Route 2 wild encounters.  The repel only blocks wild Pokémon with level STRICTLY
-- LOWER than the lead.  The starter is level 5 and Route 2 wilds can also be level
-- 5, so without this the repel misses them.  gPlayerParty[0].level (unencrypted,
-- used by the repel check) is at struct Pokemon offset 0x54.
do
    local party_addr = (_mem.gPlayerParty or 0x02032320)
    emu:write8(party_addr + 0x54, 100)
    log("lead pokemon level set to 100 for repel coverage on Route 2")
end
reinjRepel()

-- Navigate Route 2 south section to the Viridian Forest south entrance building.
-- Entry tile: (10,79) stable after idle(60). Three collision obstacles:
--   y=70 ledge (MB_JUMP_SOUTH): only x=9-10 passable northward
--   y=63-66 east wall:          only x=2-5 passable
--   y=55 ledge (MB_JUMP_SOUTH): only x=11-13 passable northward
-- x=6-12 at y=56-62 is tall grass (MB_TALL_GRASS, col=0); with lead level=100
-- the repel suppresses all encounters in that zone.
-- After crossing y=55 at x=11, shift to x=5 and walk north to the warp tile at
-- y=51 (MB_NON_ANIMATED_DOOR, col=0). Stepping onto it fires the warp to
-- MAP_ROUTE2_VIRIDIAN_FOREST_SOUTH_ENTRANCE automatically.
do
    local rx, ry = playerPos()
    local tx, ty = rx - 7, ry - 7
    print(string.format("[states] Route2 stable: tile(%d,%d)", tx, ty))
    -- Align x to 10 (passable at y=70 ledge)
    if tx > 10 then walk(KEY_LEFT, tx - 10); reinjRepel() end
    if tx < 9  then walk(KEY_RIGHT, 9 - tx); reinjRepel() end
    walk(KEY_UP,    9); reinjRepel()  -- tile(10,79) → tile(10,70) cross y=70 ledge
    walk(KEY_UP,    3); reinjRepel()  -- tile(10,70) → tile(10,67)
    walk(KEY_LEFT,  6); reinjRepel()  -- tile(10,67) → tile(4,67) west of east wall
    walk(KEY_UP,   11); reinjRepel()  -- tile(4,67)  → tile(4,56)  past east wall (x=2-5 corridor)
    walk(KEY_RIGHT, 7); reinjRepel()  -- tile(4,56)  → tile(11,56) crosses grass; repel level=100
    walk(KEY_UP,    1); reinjRepel()  -- tile(11,56) → tile(11,55) cross y=55 ledge
    walk(KEY_UP,    1); reinjRepel()  -- tile(11,55) → tile(11,54)
    walk(KEY_LEFT,  6); reinjRepel()  -- tile(11,54) → tile(5,54)
    walk(KEY_UP,    3); reinjRepel()  -- tile(5,54)  → tile(5,51)  step onto warp
end
-- walkToMap handles the south entrance building (player lands at y=10, exit to
-- forest is north warp at y=1) and detects when Viridian Forest map loads.
walkToMap(KEY_UP, MAP.VID_FOREST, "Viridian Forest", 7200)
-- The building→forest warp triggers a full fade-in animation (~90 frames).
-- Wait 120 frames for it to complete and for the player to settle at forest
-- entry tile (29,62) before counting path steps.
idle(120)

-- Pre-set all Viridian Forest trainer defeat flags so trainer sight cones are
-- inactive during navigation.  TRAINER_FLAGS_START = 0x500 (flags.h).
-- flags[] is at SaveBlock1 offset 0x1270 (confirmed by lab NPC hide byte at 0x1275).
-- Trainers: Rick=14 (flag 0x50E), Doug=15 (0x50F), Sammy=16 (0x510),
--           Anthony=412 (0x69C), Charlie=413 (0x69D).
-- Sammy at (7,22) FACE_LEFT sight=4 sees tiles (3-6,22); the only terrain path
-- to the north exit crosses (6,22) — his defeat flag must be set or he engages.
do
    local sb1 = emu:read32(ADDR_SB1_PTR)
    if sb1 ~= 0 then
        local FLAGS_OFF           = 0x1270
        local TRAINER_FLAGS_START = 0x500
        local function setFlag(flag_id)
            local byte_idx = math.floor(flag_id / 8)
            local bit_pos  = flag_id % 8
            local addr     = sb1 + FLAGS_OFF + byte_idx
            local mask     = math.floor(2 ^ bit_pos + 0.5)
            local v        = emu:read8(addr)
            if v % (mask * 2) < mask then
                emu:write8(addr, v + mask)
            end
        end
        for _, tid in ipairs({14, 15, 16, 412, 413}) do
            setFlag(TRAINER_FLAGS_START + tid)
        end
        print("[states] forest trainer defeat flags set (Rick/Doug/Sammy/Anthony/Charlie)")
    end
end

-- NPC-aware BFS path from forest south entry (29,62) to north exit (5,9).
-- Detours around Youngster NPC at (29,58): go RIGHT before crossing y=58.
-- Crosses (6,22) which is in Sammy's sight cone — safe with defeat flag set above.
do
    local rx, ry = playerPos()
    print(string.format("[states] forest entry tile(%d,%d)", rx-7, ry-7))
    local FOREST_SEQ = {
        {KEY_UP,    3}, {KEY_RIGHT, 1}, {KEY_UP,    1}, {KEY_RIGHT, 1},
        {KEY_UP,    3}, {KEY_RIGHT, 8}, {KEY_UP,   36}, {KEY_LEFT,  7},
        {KEY_DOWN,  4}, {KEY_LEFT,  9}, {KEY_UP,   13}, {KEY_LEFT,  8},
        {KEY_DOWN, 17}, {KEY_LEFT,  8}, {KEY_UP,    4}, {KEY_LEFT,  1},
        {KEY_UP,   10}, {KEY_LEFT,  1}, {KEY_UP,    4},
    }
    for _, move in ipairs(FOREST_SEQ) do
        walk(move[1], move[2])
        local sb1 = emu:read32(ADDR_SB1_PTR)
        if sb1 ~= 0 then emu:write16(sb1 + REPEL_OFF, 9999) end
    end
    rx, ry = playerPos()
    print(string.format("[states] forest exit tile(%d,%d)", rx-7, ry-7))
end

-- Navigate from forest north exit through the entrance building and Route 2 north
-- to Pewter City.
--
-- Route 2 terrain constraint (from map.bin analysis):
--   The passable north corridor is only x=8-11 (tile).  At the building exit the
--   player lands at tile(5,13) = raw(12,20).  Pressing UP alone works until
--   tile y=2, but y=0-1 are blocked at x=5 (only x=8-11 open there).
--   Fix: once on Route 2 (37,20), walk UP one row to y<=12, then RIGHT to x>=9
--   (raw 16), then UP the rest of the way.
do
    local frames = 0
    local maxFrames = 72000
    local function atPewter()
        local g, n = readMap(); return g == MAP.PEWTER.g and n == MAP.PEWTER.n
    end
    while frames < maxFrames and not atPewter() do
        local ctx  = emu:read8(ADDR_SCRIPT_STATUS)
        local lock = emu:read8(ADDR_FIELD_LOCK)
        if ctx == 1 or lock ~= 0 then
            emu:setKeys(KEY_A); emu:runFrame(); emu:setKeys(0); emu:runFrame()
            frames = frames + 2
        else
            local x, y = playerPos()
            local gm, nm = readMap()
            local key
            if gm == MAP.ROUTE2.g and nm == MAP.ROUTE2.n then
                -- Route 2 north: must reach raw x>=16 (tile x>=9) before exiting north.
                -- At raw y>19 (tile y>12) the row at x=8-10 is blocked; go UP first.
                if y > 19 then         -- tile y>12: go UP to clear the blocked row
                    key = KEY_UP
                elseif x < 16 then     -- tile x<9: shift right into the open corridor
                    key = KEY_RIGHT
                else
                    key = KEY_UP       -- x OK, head north into Pewter
                end
            else
                key = KEY_UP           -- inside building or other map: just go north
            end
            emu:setKeys(key); emu:runFrame(); emu:setKeys(0)
            frames = frames + 1
        end
        if frames % 2000 == 0 then
            local x, y = playerPos(); local gm, nm = readMap()
            print(string.format("[states] →Pewter frame=%d map=(%d,%d) tile(%d,%d)",
                  frames, gm, nm, x-7, y-7))
        end
    end
    if not atPewter() then
        local x, y = playerPos(); local gm, nm = readMap()
        error(string.format("[states] TIMEOUT →Pewter map(%d,%d) tile(%d,%d)",
              gm, nm, x-7, y-7))
    end
    local x, y = playerPos()
    print(string.format("[states] MAP Pewter City            (%2d,%2d) after %5d frames  tile(%d,%d)",
          MAP.PEWTER.g, MAP.PEWTER.n, frames, x-7, y-7))
end

-- NPC-aware gym approach (BFS on map.bin with fat man NPC at (21,28) blocked).
-- x=22 is the clear east corridor: passable y=39..12, through y=20 wall (blocks x≤21)
-- and beside the PC building (blocks x=13-21 at y=22-25).
-- Path from any entry x (20-23): shift to x=22 → UP to y=12 → LEFT to x=8 → DOWN
-- to y=17 → RIGHT to x=15 → UP 1 step onto gym warp tile (15,16).
idle(60)   -- let warp-exit animation settle so playerPos() reads the stable tile
do
    local function pewterTilePos()
        local rx, ry = playerPos(); return rx-7, ry-7
    end
    local tx, ty = pewterTilePos()
    print(string.format("[states] Pewter settled: tile(%d,%d)", tx, ty))

    -- 1. Shift to x=22 (east corridor; fat man at (21,28) makes x=21 unsafe heading north)
    if tx < 22 then walk(KEY_RIGHT, 22-tx); reinjRepel() end
    if tx > 22 then walk(KEY_LEFT,  tx-22); reinjRepel() end

    -- 2. Walk north to y=12 (y=20 wall only blocks x≤21; x=22 passes through freely)
    tx, ty = pewterTilePos()
    walk(KEY_UP, ty-12); reinjRepel()

    -- 3. Walk west to x=8 along the open y=12 corridor
    walk(KEY_LEFT, 14); reinjRepel()

    -- 4. Walk south to y=17 (gym-approach row)
    walk(KEY_DOWN, 5); reinjRepel()

    -- 5. Walk east to x=15 (one tile south of gym entrance warp at (15,16))
    walk(KEY_RIGHT, 7); reinjRepel()

    -- 6. Step north onto gym warp → triggers transition to MAP_PEWTER_CITY_GYM
    walk(KEY_UP, 1)
    local x, y = pewterTilePos()
    print(string.format("[states] gym approach done: tile(%d,%d)", x, y))
end
waitForMap(MAP.PEWTER_GYM, "Pewter City Gym", 600, 15)
idle(60)   -- let warp-exit animation settle

-- Pre-defeat Pewter Gym trainer Liam (flag 0x534) so the path to Brock is clear.
do
    local sb1 = emu:read32(ADDR_SB1_PTR)
    if sb1 ~= 0 then
        local FLAGS_OFF = 0x1270
        local LIAM_FLAG = 0x534
        local byte_idx  = math.floor(LIAM_FLAG / 8)
        local bit_pos   = LIAM_FLAG % 8
        local addr      = sb1 + FLAGS_OFF + byte_idx
        local mask      = math.floor(2 ^ bit_pos + 0.5)
        local v         = emu:read8(addr)
        if v % (mask * 2) < mask then emu:write8(addr, v + mask) end
        print(string.format("[states] Liam trainer flag 0x534 set (byte=0x%02x)", emu:read8(addr)))
    end
end

-- Navigate north inside the gym: enter at y=12, walk to y=8 (facing south toward entrance).
-- The gym center column (x=6) is the only unobstructed path past the boulder rows.
-- Gym entrance puts player at approximately (6,12); walk UP 4 tiles to reach (6,8).
do
    local rx, ry = playerPos()
    print(string.format("[states] gym entry tile(%d,%d)", rx-7, ry-7))
end
walk(KEY_UP, 4)
do
    local rx, ry = playerPos()
    print(string.format("[states] gym pre-save tile(%d,%d)", rx-7, ry-7))
end

-- ── CHECKPOINT 3: pewter_gym.ss1 ──────────────────────────────────────────────
saveState("pewter_gym.ss1")

-- ── Done ──────────────────────────────────────────────────────────────────────
print("[states] All checkpoints saved to " .. STATES_DIR)
if emu.quit then emu:quit() else os.exit(0) end
