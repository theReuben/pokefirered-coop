-- test/lua/test_rival_lab.lua
--
-- Deterministic scenario: both players walk to the rival-battle trigger tiles
-- in Oak's lab, advance dialogue, select their party (Bulbasaur / Charmander),
-- and verify the co-op double battle starts in sync.
--
-- Run via MCP harness:
--   start_emulator p1 savestate=p1_rivals_lab.ss1
--   start_emulator p2 savestate=p2_rivals_lab.ss1
--   (then drive with press_button / drive_to_tile per the steps below)
--
-- Or run as a standalone Lua scenario once the harness supports it.
--
-- Memory layout (from memory_map.lua):
--   gMultiplayerState + 1  = connState   (0=DISC, 1=CONN-ING, 2=CONNECTED)
--   gBattlersCount         = 0x020000B0
--   gBattleTypeFlags       = 0x020000AC

local H    = require("test/lua/_harness")
local M    = require("test/lua/memory_map")

-- ── Helpers ──────────────────────────────────────────────────────────────────

local CONNECTED = 2  -- MP_STATE_CONNECTED

local function connState(inst)
    -- gMultiplayerState.connState is at offset +1 from the struct base.
    -- H.read8 reads directly by address; we add 1 for the struct offset.
    return H.read8(M.gMultiplayerState + 1)
end

local function battlersCount()
    return H.read8(M.gBattlersCount)
end

local function battleTypeFlags()
    return H.read32(M.gBattleTypeFlags)
end

-- Wait up to `frames` frames for `cond()` to return true; fail if not met.
local function waitFor(cond, frames, msg)
    for i = 1, frames do
        emu:runFrame()
        if cond() then return end
    end
    H.fail(msg or "timeout waiting for condition")
end

-- ── Step 1: Verify connection established ────────────────────────────────────
-- The relay auto-injects PARTNER_CONNECTED; wait up to 10s.
waitFor(function() return connState() == CONNECTED end, 600,
    "connState never reached CONNECTED — relay may not be running")

H.log("Both instances CONNECTED")

-- ── Step 2: Navigation ───────────────────────────────────────────────────────
-- P1 target tile: (7,8) — rightmost trigger tile
-- P2 target tile: (5,8) — leftmost trigger tile (avoids ghost collision)
--
-- drive_to_tile is an MCP tool; in a pure Lua run we'd need button presses.
-- This script documents the expected tile sequence; the actual driving is done
-- by the MCP harness calling drive_to_tile for each instance.
--
-- Expected path for P1 from save state start (8,5): UP UP UP LEFT (4 steps)
-- Expected path for P2 from save state start (10,5): LEFT UP…LEFT (8 steps)
H.log("Navigation: P1→(7,8), P2→(5,8) — use drive_to_tile from MCP harness")

-- ── Step 3: Wait for rival dialogue ─────────────────────────────────────────
-- After stepping onto trigger tiles the rival dialogue fires.
-- Advance with A presses (3 boxes).
waitFor(function()
    -- CONTEXT_RUNNING=0, CONTEXT_WAITING=1, CONTEXT_SHUTDOWN=2
    -- WAITING (1) = native script active (e.g. waitbossstart holding in native mode)
    return H.read8(M.sGlobalScriptContextStatus) == 1
end, 300, "rival battle trigger script never started")

for _ = 1, 5 do
    H.pressButton("A", 12, 30)
    emu:runFrame()
end

H.log("Rival dialogue advanced")

-- ── Step 4: Party selection ──────────────────────────────────────────────────
-- Both instances open waitcoopparty.  Each needs: party select → START → A.
-- The harness drives this; here we just wait for the party menu to close.
waitFor(function()
    -- Once waitcoopparty returns, CB2_ReturnToFieldContinueScript runs and
    -- the overworld fade kicks off.  sLockFieldControls goes back to 0 once
    -- the field is resumed.
    return H.read8(M.sLockFieldControls) == 0
        and H.read8(M.sGlobalScriptContextStatus) ~= 0
end, 1800, -- 30s — party select is slow
    "party selection never completed")

H.log("Party selection done")

-- ── Step 5: Sync check ───────────────────────────────────────────────────────
-- Wait for the battle to start (gBattlersCount == 4).
waitFor(function() return battlersCount() == 4 end, 600,
    "co-op battle never started (gBattlersCount != 4 after 10s)")

local flags = battleTypeFlags()
local DOUBLE = 0x00000001
local MULTI  = 0x00000040
if (flags & DOUBLE) == 0 then H.fail("BATTLE_TYPE_DOUBLE not set: " .. string.format("0x%08X", flags)) end
if (flags & MULTI)  == 0 then H.fail("BATTLE_TYPE_MULTI not set: "  .. string.format("0x%08X", flags)) end

H.log(string.format("Battle started: gBattleTypeFlags=0x%08X gBattlersCount=%d", flags, battlersCount()))
H.pass("rival lab co-op double battle started correctly")
