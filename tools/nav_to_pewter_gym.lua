-- tools/nav_to_pewter_gym.lua
-- Navigate from tall_grass_route1.ss1 to Pewter City Gym, saving pewter_gym.ss1.
-- Route: Route1 (via x=7 ledge corridor) → Viridian → Route2 → Forest → Pewter → Gym
-- Run as:
--   /tmp/mgba-build/mgba-headless --script tools/nav_to_pewter_gym.lua pokefirered.gba

local KEY_A     = 0x001
local KEY_B     = 0x002
local KEY_RIGHT = 0x010
local KEY_LEFT  = 0x020
local KEY_UP    = 0x040
local KEY_DOWN  = 0x080

local SCRIPT_DIR = debug.getinfo(1, "S").source:match("@(.+/)") or "./"
local REPO_ROOT  = SCRIPT_DIR .. "../"
local STATES_DIR = REPO_ROOT .. "test/lua/states/"

local _mem = dofile(REPO_ROOT .. "test/lua/memory_map.lua")
local ADDR_SB1_PTR       = _mem.gSaveBlock1Ptr
local ADDR_COOP_SETTINGS = _mem.gCoopSettings
local ADDR_SCRIPT_STATUS = _mem.sGlobalScriptContextStatus
local ADDR_FIELD_LOCK    = _mem.sLockFieldControls

local SB1_LOC_GROUP = 4
local SB1_LOC_NUM   = 5
local ADDR_OBJ0_X   = 0x020015bc + 0x10
local ADDR_OBJ0_Y   = 0x020015bc + 0x12

local function readMap()
    local ptr = emu:read32(ADDR_SB1_PTR)
    if ptr == 0 then return 0, 0 end
    return emu:read8(ptr + SB1_LOC_GROUP), emu:read8(ptr + SB1_LOC_NUM)
end

local function playerPos()
    local x = emu:read16(ADDR_OBJ0_X)
    local y = emu:read16(ADDR_OBJ0_Y)
    if x >= 0x8000 then x = x - 0x10000 end
    if y >= 0x8000 then y = y - 0x10000 end
    return x, y
end

local function isBlocked()
    local ctx  = ADDR_SCRIPT_STATUS and emu:read8(ADDR_SCRIPT_STATUS) or 0
    local lock = emu:read8(ADDR_FIELD_LOCK)
    return ctx == 1 or lock ~= 0
end

local function injectRepel()
    local sb1 = emu:read32(ADDR_SB1_PTR)
    if sb1 ~= 0 then
        emu:write16(sb1 + 0x139C + 0x21 * 2, 9999)
    end
    if ADDR_COOP_SETTINGS and ADDR_COOP_SETTINGS ~= 0 then
        emu:write8(ADDR_COOP_SETTINGS, 0)
    end
    -- Force-clear field lock so we never get permanently stuck
    emu:write8(ADDR_FIELD_LOCK, 0)
end

local function saveState(name)
    local path = STATES_DIR .. name
    local buf = emu:saveStateBuffer(31)
    assert(buf and #buf > 0, "saveStateBuffer returned empty")
    local f = assert(io.open(path, "wb"), "cannot open " .. path)
    f:write(buf); f:close()
    local g, n = readMap()
    print(string.format("[nav] SAVED %-26s (%d bytes)  map=(%d,%d)", name, #buf, g, n))
end

-- ── State machine ─────────────────────────────────────────────────────────

local frame       = 0
local phase       = "settle"
local phase_frame = 0

-- walk_to_map state
local wt_dir, wt_g, wt_n, wt_label, wt_max

-- walk_tiles state (fixed tile-count walks)
local wk_dir, wk_tiles_left, wk_tile_frame, wk_next_fn

-- forest_nav state: BFS-computed path from Forest (29,62) to exit warp (5,9)
-- Verified passable by static map analysis. Directions: UP=decrease y, DOWN=increase y.
local FOREST_SEQ = {
    {KEY_UP, 4},   {KEY_RIGHT, 2}, {KEY_UP, 3},    {KEY_RIGHT, 8},
    {KEY_UP, 36},  {KEY_LEFT, 7},  {KEY_DOWN, 4},  {KEY_LEFT, 9},
    {KEY_UP, 13},  {KEY_LEFT, 8},  {KEY_DOWN, 17}, {KEY_LEFT, 8},
    {KEY_UP, 14},  {KEY_LEFT, 2},  {KEY_UP, 4},
}
local forest_idx       = 1
local forest_tiles_left = 0
local forest_tile_frame = 0

local function enter(p)
    phase = p
    phase_frame = 0
    print(string.format("[nav] -> %-22s  frame=%d", p, frame))
end

local function enter_walk(dir, g, n, label, max_f)
    wt_dir = dir; wt_g = g; wt_n = n; wt_label = label; wt_max = max_f
    enter("walk_to_map")
end

local function enter_walk_tiles(dir, tiles, next_fn)
    wk_dir = dir; wk_tiles_left = tiles; wk_tile_frame = 0; wk_next_fn = next_fn
    enter("walk_tiles")
end

-- ── Frame callback ────────────────────────────────────────────────────────

callbacks:add("frame", function()
    frame = frame + 1
    phase_frame = phase_frame + 1

    if frame % 60 == 0 then injectRepel() end

    -- ── settle: wait 300 frames after state load ─────────────────────────
    if phase == "settle" then
        emu:setKeys(0)
        if phase_frame == 300 then
            injectRepel()
            local x, y = playerPos()
            local g, n = readMap()
            print(string.format("[nav] Settled: map=(%d,%d) pos=(%d,%d)", g, n, x, y))
            -- Walk directly north to Viridian — the eastern corridor (tile x≈13) is clear.
            enter_walk(KEY_UP, 37, 1, "Viridian City", 36000)
        end
        return
    end

    -- ── walk_tiles: walk exactly N tiles in a fixed direction ─────────────
    if phase == "walk_tiles" then
        wk_tile_frame = wk_tile_frame + 1
        if wk_tile_frame <= 16 then
            emu:setKeys(wk_dir)
        elseif wk_tile_frame <= 18 then
            emu:setKeys(0)
        else
            -- Tile complete
            wk_tile_frame = 0
            wk_tiles_left = wk_tiles_left - 1
            if wk_tiles_left <= 0 then
                wk_next_fn()
            end
        end
        return
    end

    -- ── walk_left: walk LEFT until raw x == 14 (tile x=7, ledge corridor) ─
    if phase == "walk_left" then
        local x, y = playerPos()
        if x <= 14 then
            print(string.format("[nav] walk_left done: pos=(%d,%d)", x, y))
            -- Now on the passable corridor at tile x=7, walk north to Viridian
            enter_walk(KEY_UP, 37, 1, "Viridian City", 36000)
            return
        end
        -- Always press LEFT; the every-60-frame field-lock clear in injectRepel
        -- ensures we don't stay frozen even if a script runs momentarily.
        emu:setKeys(KEY_LEFT)
        if phase_frame % 200 == 0 then
            print(string.format("[nav] walk_left  frame=%d  pos=(%d,%d)", frame, x, y))
        end
        return
    end

    -- ── walk_to_map: walk in wt_dir until map (wt_g, wt_n) reached ──────
    if phase == "walk_to_map" then
        local g, n = readMap()

        if g == wt_g and n == wt_n then
            emu:setKeys(0)
            local x, y = playerPos()
            print(string.format("[nav] Reached %-20s (%d,%d)  frame=%d  pos=(%d,%d)",
                  wt_label, g, n, frame, x, y))

            if wt_g == 37 and wt_n == 1 then        -- Viridian City
                -- Walk RIGHT 2 to x=19+0=adjust... actually walk to x<=12 (tile x=5) for
                -- Route2 forest entrance at x=5-6.
                -- Viridian → Route2: player enters Route2 at x = Viridian_x - 12.
                -- If player is at raw x=26 (tile x=19) in Viridian, in Route2 it's tile x=7.
                -- We need tile x=5 (raw x=12) to hit the south entrance building.
                -- So walk LEFT 2 tiles in Route 2. Handle after entering Route 2.
                enter_walk(KEY_UP, 37, 20, "Route 2", 10800)

            elseif wt_g == 37 and wt_n == 20 then   -- Route 2 reached
                -- Player enters Route2 at x=13 (Route1_x=13→Viridian_x=25→Route2_x=13).
                -- Forest south entrance warp is at x=5,6 (y=51). Walk LEFT 8 tiles to x=5.
                local x2, _ = playerPos()
                local left_count = math.max(1, x2 - 12)  -- raw x=12 = tile x=5
                print(string.format("[nav] Route2 entry x=%d, walking LEFT %d tiles", x2, left_count))
                enter_walk_tiles(KEY_LEFT, left_count, function()
                    enter_walk(KEY_UP, 35, 0, "Viridian Forest", 36000)
                end)

            elseif wt_g == 35 and wt_n == 0 then    -- Viridian Forest
                -- Initialize and start forest navigation
                forest_idx = 1
                forest_tiles_left = 0
                forest_tile_frame = 0
                enter("forest_nav")

            elseif wt_g == 37 and wt_n == 2 then    -- Pewter City
                enter("approach_gym")
            end
            return
        end

        if phase_frame >= wt_max then
            local x, y = playerPos()
            print(string.format("[nav] TIMEOUT %-20s target=(%d,%d) current=(%d,%d) pos=(%d,%d)",
                  wt_label, wt_g, wt_n, g, n, x, y))
            os.exit(1)
        end

        if isBlocked() then
            emu:setKeys(phase_frame % 2 == 0 and KEY_A or 0)
        else
            emu:setKeys(wt_dir)
        end

        if phase_frame % 1000 == 0 then
            local x, y = playerPos()
            print(string.format("[nav] walk %-16s  frame=%d  map=(%d,%d)  pos=(%d,%d)",
                  wt_label, frame, g, n, x, y))
        end
        return
    end

    -- ── forest_nav: follow BFS-computed path from entrance (29,62) to exit warp (5,9) ─
    if phase == "forest_nav" then
        local g, n = readMap()
        if not (g == 35 and n == 0) then
            -- Forest exited (warp triggered) — we're in north entrance or Route2
            local x, y = playerPos()
            print(string.format("[nav] Forest exited to map=(%d,%d) pos=(%d,%d)", g, n, x, y))
            -- Continue north through entrance building and Route 2 to Pewter
            enter_walk(KEY_UP, 37, 2, "Pewter City", 72000)
            return
        end

        if phase_frame > 20000 then
            local x, y = playerPos()
            local cur = forest_seq[forest_idx] or {"?", "?"}
            print(string.format("[nav] TIMEOUT forest_nav  pos=(%d,%d)  move=%d/%d",
                  x, y, forest_idx, #FOREST_SEQ))
            os.exit(1)
        end

        if isBlocked() then
            -- Press B to dismiss battle/dialogue (in forest, try to run or skip)
            emu:setKeys(phase_frame % 4 < 2 and KEY_B or KEY_A)
            return
        end

        if forest_idx > #FOREST_SEQ then
            -- Sequence complete but still on forest map — something wrong
            local x, y = playerPos()
            print(string.format("[nav] forest_nav: sequence done but still in forest  pos=(%d,%d)", x, y))
            -- Try walking north to find exit
            emu:setKeys(KEY_UP)
            return
        end

        local move = FOREST_SEQ[forest_idx]
        local dir, tiles = move[1], move[2]

        if forest_tiles_left == 0 then
            forest_tiles_left = tiles
            forest_tile_frame = 0
            local x, y = playerPos()
            local dname = ({[KEY_UP]="UP",[KEY_DOWN]="DOWN",[KEY_LEFT]="LEFT",[KEY_RIGHT]="RIGHT"})[dir] or "?"
            print(string.format("[nav] forest move %d/%d: %s x%d  pos=(%d,%d)",
                  forest_idx, #FOREST_SEQ, dname, tiles, x, y))
        end

        forest_tile_frame = forest_tile_frame + 1
        if forest_tile_frame <= 16 then
            emu:setKeys(dir)
        elseif forest_tile_frame <= 18 then
            emu:setKeys(0)
        else
            forest_tile_frame = 0
            forest_tiles_left = forest_tiles_left - 1
            if forest_tiles_left == 0 then
                forest_idx = forest_idx + 1
            end
        end
        return
    end

    -- ── approach_gym: navigate Pewter City to gym warp tile (15,16) = raw (22,23) ─
    -- Player enters Pewter from Route2 north. Connection offset gives Pewter_x = Route2_x + 12.
    -- After forest exit at Route2 x=5, entering Pewter at tile x=5+12=17 = raw x=24, tile y=39 = raw y=46.
    -- Gym warp tile: Pewter (15,16) = raw (22,23). Walk north to raw y=24, then left to raw x=22.
    if phase == "approach_gym" then
        local g, n = readMap()
        if g == 40 and n == 2 then
            emu:setKeys(0)
            local x, y = playerPos()
            print(string.format("[nav] Entered Pewter Gym  frame=%d  pos=(%d,%d)", frame, x, y))
            enter("settle_gym")
            return
        end

        if phase_frame > 7200 then
            local x, y = playerPos()
            print(string.format("[nav] TIMEOUT approach_gym  pos=(%d,%d)  map=(%d,%d)", x, y, g, n))
            os.exit(1)
        end

        local x, y = playerPos()

        if isBlocked() then
            emu:setKeys(phase_frame % 2 == 0 and KEY_A or 0)
        elseif y > 24 then
            -- Still south of gym row: walk north
            emu:setKeys(KEY_UP)
        elseif x > 22 then
            -- At gym row, need to go left to x=22 (tile x=15)
            emu:setKeys(KEY_LEFT)
        else
            -- Aligned: walk north to step onto gym warp tile
            emu:setKeys(KEY_UP)
        end

        if phase_frame % 200 == 0 then
            print(string.format("[nav] approach_gym  frame=%d  pos=(%d,%d)  map=(%d,%d)",
                  frame, x, y, g, n))
        end
        return
    end

    -- ── settle_gym: wait for gym interior to fully load ──────────────────
    if phase == "settle_gym" then
        emu:setKeys(0)
        if phase_frame >= 120 then
            enter("save_state")
        end
        return
    end

    -- ── save_state: write pewter_gym.ss1 and exit ────────────────────────
    if phase == "save_state" then
        if phase_frame == 1 then
            emu:setKeys(0)
            local g, n = readMap()
            local x, y = playerPos()
            print(string.format("[nav] Saving: map=(%d,%d) pos=(%d,%d)", g, n, x, y))
            saveState("pewter_gym.ss1")
            print("[nav] Done!")
            if emu.quit then emu:quit() else os.exit(0) end
        end
        return
    end
end)

-- ── Startup: load tall_grass_route1.ss1 ──────────────────────────────────

local ss1_path = STATES_DIR .. "tall_grass_route1.ss1"
print("[nav] Loading " .. ss1_path)
local f = io.open(ss1_path, "rb")
if not f then error("[nav] Cannot open " .. ss1_path) end
local data = f:read("*a"); f:close()
emu:loadStateBuffer(data, 31)
print("[nav] State loaded. Frame callbacks active.")
