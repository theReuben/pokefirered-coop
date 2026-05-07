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
-- gSaveBlock1Ptr is at 0x03005300; location.mapGroup is at SB1+4, mapNum at SB1+5.
local ADDR_SB1_PTR  = 0x03005300
local SB1_LOC_GROUP = 4
local SB1_LOC_NUM   = 5

-- ── Player position (EWRAM gObjectEvents[0].currentCoords) ───────────────────
-- ObjectEvent size=0x24; currentCoords at +0x10 (x:s16, y:s16).
local ADDR_OBJ0_X = 0x020015bc + 0x10
local ADDR_OBJ0_Y = 0x020015bc + 0x12

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
    end
    local g, n = readMap()
    error(string.format("[states] TIMEOUT waiting for %s (%d,%d) – on (%d,%d) after %d frames",
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

local function spamA(times, gap)
    gap = gap or 6
    for _ = 1, times do press(KEY_A, 2, gap) end
end

-- Hold direction for N tiles (~16f hold + 2f release per tile).
local function walk(dir, tiles)
    for _ = 1, tiles do press(dir, 16, 2) end
end

local function playerPos()
    local x = emu:read16(ADDR_OBJ0_X)
    local y = emu:read16(ADDR_OBJ0_Y)
    if x >= 0x8000 then x = x - 0x10000 end
    if y >= 0x8000 then y = y - 0x10000 end
    return x, y
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

idle(200)                   -- CB2_ReturnFromNamingScreen completes + ConfirmName appears
press(KEY_A, 5, 30)         -- "So your name is [X]?"
idle(30)                    -- YES/NO appears
press(KEY_A, 5, 30)         -- YES

idle(250)                   -- rival pic fades in, text auto-prints, menu slides in
press(KEY_DOWN, 3, 20)      -- cursor 0(NEW NAME) → 1(GREEN)
press(KEY_A, 5, 30)         -- select GREEN → ConfirmName (no naming screen required)

-- From here all remaining inputs are A presses; waitForMap handles timing.
waitForMap(MAP.HOUSE_2F, "bedroom 2F", 36000, 30)

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

local OAK_OFF = 0x139C + (0x4050 - 0x4000) * 2   -- SB1 offset for VAR_MAP_SCENE_PALLET_TOWN_OAK
local LAB_OFF = 0x139C + (0x4055 - 0x4000) * 2   -- SB1 offset for VAR_MAP_SCENE_PALLET_TOWN_PROFESSOR_OAKS_LAB
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

-- Flag 0 fix is inlined into the wait loops below — callbacks:add is not
-- available in mGBA headless context.
local FLAG_BYTE_OFF = 0x1270   -- gSaveBlock1+0x1270 byte 0; bit 0 = flag 0

-- Walk to the lab entrance warp at tile(16,13) via the passable corridor.
-- The escort script (PlayerWalkToLabLeft) reveals: x=11 column is open from y=3
-- to y=14; then RIGHT to (16,14); then UP onto the warp at (16,13).
-- VAR_OAK=1 prevents OakTrigger at tile(12,1) from firing during navigation.
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
-- Player ends at tile(6,4) facing north.  Keep flag 0 cleared so all NPCs visible.
log("phase 7: choosestarter scene → approach ball")

-- Wait for the lab map to load (natural warp fires when UP 1 hits tile 16,13).
-- Clear flag 0 every frame so Oak/Rival/Balls are not hidden on entry.
print("[states] waiting for Oak's Lab...")
for i = 1, 36000 do
    emu:setKeys(0)
    emu:runFrame()
    local fptr = emu:read32(ADDR_SB1_PTR)
    if fptr ~= 0 then
        local b = emu:read8(fptr + FLAG_BYTE_OFF)
        if b % 2 ~= 0 then emu:write8(fptr + FLAG_BYTE_OFF, b - 1) end
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

-- Wait for ChooseStarterScene to complete (VAR_LAB==2).
-- ON_FRAME fires it immediately since VAR_LAB=1 was injected before entry.
-- Spam A every 30f to advance the 4 scene msgboxes.
-- Keep clearing flag 0 so Rival/Oak/Balls remain visible for waitmovement 0.
print("[states] waiting for ChooseStarterScene (VAR_LAB->2)...")
local scene_done = false
for i = 1, 18000 do
    emu:setKeys(0)
    emu:runFrame()
    local fptr = emu:read32(ADDR_SB1_PTR)
    if fptr ~= 0 then
        local b = emu:read8(fptr + FLAG_BYTE_OFF)
        if b % 2 ~= 0 then emu:write8(fptr + FLAG_BYTE_OFF, b - 1) end
        if emu:read16(fptr + LAB_OFF) == 2 then
            print(string.format("[states] ChooseStarterScene complete frame=%d", i))
            scene_done = true
            break
        end
    end
    if i % 30 == 0 then
        emu:setKeys(KEY_A); emu:runFrame(); emu:setKeys(0)
    end
    if i % 3000 == 0 then
        local fptr2 = emu:read32(ADDR_SB1_PTR)
        local vlab = fptr2 ~= 0 and emu:read16(fptr2 + LAB_OFF) or -1
        local fb   = fptr2 ~= 0 and emu:read8(fptr2 + FLAG_BYTE_OFF) or -1
        local px, py = playerPos(); local gm, nm = readMap()
        print(string.format("[states] heartbeat frame=%5d VAR_LAB=%d flag0_byte=0x%02x tile(%d,%d) map=(%d,%d)",
              i, vlab, fb, px-7, py-7, gm, nm))
    end
end
if not scene_done then
    local fptr2 = emu:read32(ADDR_SB1_PTR)
    local vlab = fptr2 ~= 0 and emu:read16(fptr2 + LAB_OFF) or -1
    error(string.format("[states] TIMEOUT: ChooseStarterScene never set VAR_LAB=2 (last=%d)", vlab))
end

idle(60)    -- settle after releaseall

do local x,y=playerPos(); local g,n=readMap()
   print(string.format("[states] post-scene tile(%d,%d) map=(%d,%d) VAR_LAB=%d",
         x-7,y-7,g,n,readVar(LAB_OFF))) end

-- Navigate from (6,4) to (8,5) facing north.
-- DOWN 1 clears y=4 (rival+ball row); RIGHT 2 to x=8; UP 1 → blocked by ball
-- at (8,4) leaving player at (8,5) facing north.
walk(KEY_DOWN,  1)          -- (6,4) → (6,5)
walk(KEY_RIGHT, 2)          -- (6,5) → (8,5)
walk(KEY_UP,    1)          -- (8,5) tries (8,4), ball blocks → stays at (8,5) facing N

do local x,y=playerPos(); local g,n=readMap()
   print(string.format("[states] at ball tile(%d,%d) VAR_LAB=%d",
         x-7,y-7,readVar(LAB_OFF))) end

-- ── CHECKPOINT 1: oaks_lab.ss1 ────────────────────────────────────────────────
-- Player at tile(8,5) facing north; Bulbasaur ball at tile(8,4).  VAR_LAB=2.
log("checkpoint 1: oaks_lab.ss1")
saveState("oaks_lab.ss1")

-- ── PHASE 8: Choose Bulbasaur → rival battle → exit lab ──────────────────────
-- Player at (8,5) facing NORTH.  Ball interaction fires BulbasaurBall script:
--   setvar VAR_STARTER_NUM,0  → ConfirmStarterChoice  → MSGBOX_YESNO
-- Starter pick timing:
--   ~60f  MSGBOX_YESNO "Do you want BULBASAUR?" → A=YES
--   ~30f  "This BULBASAUR is quite energetic!" msgbox → A to advance
--   ~200f received msg + waitfanfare → MSGBOX_YESNO "Give a nickname?" → B=NO
--   waitstarterpick resolves immediately (connState=0) → RivalPicksStarter
--   ~100f rival walks to Squirtle ball (4 tiles right from x=5)
--   ~30f  "I'll take THIS ONE then!" msgbox → A to advance
--   ~200f rival received + waitfanfare → setvar VAR_LAB,3 + release
-- Walk DOWN 3 + LEFT 1: (8,5)→(8,8)→(7,8) → RivalBattleTrigger (VAR_LAB==3).
-- Walk DOWN 4: (7,8)→(7,12) → exit warp → MAP_PALLET_TOWN.
log("phase 8: pick starter + rival battle + exit lab")
press(KEY_A, 3, 30)         -- interact with Bulbasaur ball
idle(60)                    -- MSGBOX_YESNO for starter appears
press(KEY_A, 3, 30)         -- YES to "Do you want BULBASAUR?"
idle(30)
press(KEY_A, 3, 30)         -- advance "This BULBASAUR is quite energetic!" msgbox
idle(200)                   -- received msg + waitfanfare (~170f)
press(KEY_B, 3, 30)         -- NO to "Give it a nickname?"
idle(100)                   -- rival walks to Squirtle ball (~80f)
press(KEY_A, 3, 30)         -- advance "I'll take THIS ONE then!" msgbox
idle(200)                   -- rival received msg + waitfanfare
idle(60)                    -- release settle; VAR_LAB=3
do local x,y=playerPos(); local g,n=readMap()
   print(string.format("[states] DBG8a post-starter tile(%d,%d) map=(%d,%d) VAR_LAB=%d",
         x-7,y-7,g,n,readVar(LAB_OFF))) end

walk(KEY_DOWN, 3)           -- (8,5) → (8,8) [x=8: no LeaveStarter trigger here]
walk(KEY_LEFT, 1)           -- (8,8) → (7,8) → RivalBattleTrigger fires (VAR_LAB==3)
spamA(100, 15)              -- rival intro + battle turns + exp + post-battle (100×17=1700f)
idle(300)                   -- releaseall settle
do local x,y=playerPos(); local g,n=readMap()
   print(string.format("[states] DBG8b post-battle tile(%d,%d) map=(%d,%d)",x-7,y-7,g,n)) end

walk(KEY_DOWN, 4)           -- (7,8) → (7,12) → exit warp fires
waitForMap(MAP.PALLET, "Pallet Town (after lab)", 18000, 15)

-- ── PHASE 9: Pallet Town → Route 1 ───────────────────────────────────────────
-- Lab exit warp arrives at tile(16,14) = stored(23,21); Route 1 is north of Pallet Town y=0.
-- Walk LEFT 4 to x=12 (lab wall blocks UP at x=16-17); UP 15 exits Pallet Town north edge.
log("phase 9: pallet town → route 1")
idle(120)                   -- wait for lab-exit walk-out animation (~30f) + map fade (~60f)
do local x,y=playerPos(); local g,n=readMap()
   print(string.format("[states] DBG9 pallet post-lab x=%d y=%d tile(%d,%d) map=(%d,%d)",x,y,x-7,y-7,g,n)) end
walk(KEY_LEFT, 4)           -- tile(16,14) → tile(12,14)
walk(KEY_UP,   15)          -- tile(12,14) → exits north edge → Route 1
waitForMap(MAP.ROUTE1, "Route 1", 600)

-- ── CHECKPOINT 2: tall_grass_route1.ss1 ──────────────────────────────────────
-- A few tiles into Route 1; tall grass is reachable immediately.
walk(KEY_UP, 5)
idle(30)
saveState("tall_grass_route1.ss1")

-- ── PHASE 10: Route 1 → Viridian → Forest → Pewter Gym ───────────────────────
-- Each waitForMap uses A-spam to handle wild encounters and trainer battles
-- that may interrupt the preceding walk.  Frame budget is generous (~10 min).
log("phase 10: to pewter gym")

walk(KEY_UP, 40)
waitForMap(MAP.VIRIDIAN, "Viridian City", 7200, 30)

walk(KEY_UP, 30)
waitForMap(MAP.ROUTE2, "Route 2", 3600, 30)

walk(KEY_UP, 30)
waitForMap(MAP.VID_FOREST, "Viridian Forest", 3600, 30)

walk(KEY_UP, 70)            -- straight north through the forest
waitForMap(MAP.PEWTER, "Pewter City", 36000, 30)

-- Walk to Pewter Gym (northwest of city centre)
walk(KEY_UP,   5)
walk(KEY_LEFT, 8)
walk(KEY_UP,   10)
waitForMap(MAP.PEWTER_GYM, "Pewter City Gym", 3600, 30)

-- ── CHECKPOINT 3: pewter_gym.ss1 ──────────────────────────────────────────────
idle(60)
saveState("pewter_gym.ss1")

-- ── Done ──────────────────────────────────────────────────────────────────────
print("[states] All checkpoints saved to " .. STATES_DIR)
if emu.quit then emu:quit() else os.exit(0) end
