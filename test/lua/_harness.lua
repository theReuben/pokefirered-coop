-- test/lua/_harness.lua
-- Shared helpers for headless mGBA scenarios run by tools/run_lua_tests.sh.
--
-- Each scenario `require`s this module, drives the ROM via memory writes
-- and frame ticks, then calls H.finish(). The harness writes a single
-- "RESULT: PASS" or "RESULT: FAIL <msg>" line to the results file and
-- exits the emulator. tools/run_lua_tests.sh greps for the result.
--
-- Compatible with both the legacy `memory` API and the modern `emu:`
-- scripting API. Detects which is available at load time.

local H = {}

-- ------------------------------------------------------------------ --
-- Memory access shim — uses whichever API this mGBA build provides.
-- ------------------------------------------------------------------ --

local function pick_reader()
    if emu and emu.read8 then
        return
            function(a) return emu:read8(a)  end,
            function(a) return emu:read16(a) end,
            function(a) return emu:read32(a) end,
            function(a, b) return emu:write8(a, b)  end,
            function(a, b) return emu:write16(a, b) end,
            function(a, b) return emu:write32(a, b) end
    end
    if memory and memory.read_u8 then
        return
            function(a) return memory.read_u8(a)  end,
            function(a) return memory.read_u16(a) end,
            function(a) return memory.read_u32(a) end,
            function(a, b) return memory.write_u8(a, b)  end,
            function(a, b) return memory.write_u16(a, b) end,
            function(a, b) return memory.write_u32(a, b) end
    end
    error("no compatible mGBA memory API found (need emu:read8 or memory.read_u8)")
end

H.read8, H.read16, H.read32, H.write8, H.write16, H.write32 = pick_reader()

-- ------------------------------------------------------------------ --
-- Frame stepping. mGBA exposes emu:runFrame(); fall back to a no-op
-- yield if running under a build without it (CI checks luac syntax only).
-- ------------------------------------------------------------------ --

function H.runFrames(n)
    if not emu or not emu.runFrame then return end
    for _ = 1, n do emu:runFrame() end
end

-- ------------------------------------------------------------------ --
-- Constants from include/constants/multiplayer.h
-- ------------------------------------------------------------------ --

H.MP_RING_SIZE         = 256
H.MP_RING_MAGIC        = 0xC0
H.MP_DISCOVERY_MAGIC   = 0xC0DEC0DE

H.MP_PKT_POSITION      = 0x01
H.MP_PKT_FLAG_SET      = 0x02
H.MP_PKT_VAR_SET       = 0x03
H.MP_PKT_BOSS_READY    = 0x04
H.MP_PKT_BOSS_CANCEL   = 0x05
H.MP_PKT_SEED_SYNC     = 0x06
H.MP_PKT_FULL_SYNC     = 0x07
H.MP_PKT_SCRIPT_LOCK   = 0x08
H.MP_PKT_SCRIPT_UNLOCK = 0x09
H.MP_PKT_BOSS_START            = 0x0A
H.MP_PKT_PARTNER_CONNECTED     = 0x0B
H.MP_PKT_PARTNER_DISCONNECTED  = 0x0C
H.MP_PKT_ITEM_GIVE             = 0x0D
H.MP_PKT_FLAG_CLEAR            = 0x0E
H.MP_PKT_STARTER_PICK          = 0x0F

H.MP_STATE_DISCONNECTED = 0
H.MP_STATE_CONNECTING   = 1
H.MP_STATE_CONNECTED    = 2

-- MpRingBuf field offsets (struct layout from include/multiplayer.h)
H.RING_BUF_OFF   = 0    -- u8 buf[256]
H.RING_HEAD_OFF  = 256
H.RING_TAIL_OFF  = 257
H.RING_MAGIC_OFF = 258

-- MultiplayerState field offsets (matches struct order in include/multiplayer.h)
H.MP_OFF_ROLE                = 0
H.MP_OFF_CONN_STATE          = 1
H.MP_OFF_PARTNER_MAP_GROUP   = 2
H.MP_OFF_PARTNER_MAP_NUM     = 3
H.MP_OFF_TARGET_X            = 4
H.MP_OFF_TARGET_Y            = 5
H.MP_OFF_TARGET_FACING       = 6
H.MP_OFF_GHOST_OBJ_ID        = 7
H.MP_OFF_BOSS_READY          = 8
H.MP_OFF_PARTNER_BOSS_ID     = 9
H.MP_OFF_IS_IN_SCRIPT        = 10
H.MP_OFF_PARTNER_IN_SCRIPT   = 11
H.MP_OFF_POS_FRAME_COUNTER   = 12
H.MP_OFF_PARTNER_STARTER     = 14   -- u16, 2-byte aligned (offset is approximate)

-- ------------------------------------------------------------------ --
-- Ring buffer helpers — push bytes into gMpRecvRing, drain gMpSendRing.
-- ------------------------------------------------------------------ --

function H.recvPush(ringAddr, byte)
    local head = H.read8(ringAddr + H.RING_HEAD_OFF)
    H.write8(ringAddr + H.RING_BUF_OFF + head, byte & 0xFF)
    H.write8(ringAddr + H.RING_HEAD_OFF, (head + 1) & 0xFF)
end

function H.recvPushMany(ringAddr, bytes)
    for _, b in ipairs(bytes) do H.recvPush(ringAddr, b) end
end

function H.ringAvailable(ringAddr)
    local head = H.read8(ringAddr + H.RING_HEAD_OFF)
    local tail = H.read8(ringAddr + H.RING_TAIL_OFF)
    return (head - tail) & 0xFF
end

function H.sendDrain(ringAddr)
    local out = {}
    while true do
        local head = H.read8(ringAddr + H.RING_HEAD_OFF)
        local tail = H.read8(ringAddr + H.RING_TAIL_OFF)
        if head == tail then break end
        table.insert(out, H.read8(ringAddr + H.RING_BUF_OFF + tail))
        H.write8(ringAddr + H.RING_TAIL_OFF, (tail + 1) & 0xFF)
    end
    return out
end

-- ------------------------------------------------------------------ --
-- Init wait — block until Multiplayer_Init has populated the magic
-- bytes in the rings + discovery table. Returns true on success.
-- ------------------------------------------------------------------ --

function H.waitForInit(memMap, maxFrames)
    maxFrames = maxFrames or 600   -- 10 seconds at 60fps
    local sym = memMap or require("memory_map")
    local frames = 0
    while frames < maxFrames do
        local sm = H.read8(sym.gMpSendRing + H.RING_MAGIC_OFF)
        local rm = H.read8(sym.gMpRecvRing + H.RING_MAGIC_OFF)
        if sm == H.MP_RING_MAGIC and rm == H.MP_RING_MAGIC then
            return true
        end
        H.runFrames(1)
        frames = frames + 1
    end
    return false
end

-- ------------------------------------------------------------------ --
-- Test result accumulation. Each scenario calls H.check(cond, msg)
-- per assertion and H.finish(name) at the end.
-- ------------------------------------------------------------------ --

local sPass, sFail = 0, 0
local sFailMsgs = {}

function H.check(cond, msg)
    if cond then
        sPass = sPass + 1
    else
        sFail = sFail + 1
        table.insert(sFailMsgs, msg or "(unnamed)")
    end
end

function H.checkEq(actual, expected, msg)
    H.check(actual == expected,
        string.format("%s: expected %s, got %s",
            msg or "checkEq", tostring(expected), tostring(actual)))
end

local function writeResult(name, ok, fails)
    -- Results file path comes from env so the wrapper can prove it ran.
    local path = os.getenv("LUA_TEST_RESULTS") or "lua_test_results.txt"
    local f = io.open(path, "a")
    if not f then return end
    if ok then
        f:write(string.format("RESULT: PASS %s (%d assertions)\n", name, sPass))
    else
        local joined = table.concat(fails, " | ")
        f:write(string.format("RESULT: FAIL %s (%d/%d failed) %s\n",
            name, sFail, sPass + sFail, joined))
    end
    f:close()
    -- Also echo to stdout so the wrapper can grep without reading the file.
    if ok then
        print(string.format("RESULT: PASS %s (%d assertions)", name, sPass))
    else
        print(string.format("RESULT: FAIL %s (%d/%d failed) %s",
            name, sFail, sPass + sFail, table.concat(fails, " | ")))
    end
end

function H.finish(name)
    local ok = (sFail == 0)
    writeResult(name, ok, sFailMsgs)
    -- Quit the emulator if the API allows; the wrapper has a hard timeout
    -- as a fallback.
    if emu and emu.quit then
        emu:quit()
    end
end

return H
