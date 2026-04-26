-- test/lua/test_flag_sync.lua
-- Integration test for flag sync and script mutex via ring buffers.
-- Load in mGBA Scripting console (Tools > Scripting) after booting pokefirered.gba.
-- Reload test/lua/memory_map.lua if addresses are stale after a rebuild.
--
-- The test writes packets directly to gMpRecvRing and checks that
-- gMultiplayerState fields update correctly on the next frame.

local sym = require("memory_map")

local PASS, FAIL = 0, 0
local function check(cond, msg)
    if cond then
        PASS = PASS + 1
        print("PASS: " .. msg)
    else
        FAIL = FAIL + 1
        print("FAIL: " .. msg)
    end
end

-- ---- Constants -------------------------------------------------------
local MP_RING_MAGIC        = 0xC0
local MP_PKT_FLAG_SET      = 0x02
local MP_PKT_SCRIPT_LOCK   = 0x08
local MP_PKT_SCRIPT_UNLOCK = 0x09
local MP_STATE_CONNECTED   = 2

-- MpRingBuf layout: buf[256], head(u8 @256), tail(u8 @257), magic(u8 @258)
local RING_BUF_OFF   = 0
local RING_HEAD_OFF  = 256
local RING_TAIL_OFF  = 257
local RING_MAGIC_OFF = 258

-- MultiplayerState field offsets (u8 fields, see include/multiplayer.h)
local OFF_ROLE               = 0
local OFF_CONN_STATE         = 1
local OFF_PARTNER_MAP_GROUP  = 2
local OFF_PARTNER_MAP_NUM    = 3
local OFF_TARGET_X           = 4
local OFF_TARGET_Y           = 5
local OFF_TARGET_FACING      = 6
local OFF_GHOST_OBJ_ID       = 7
local OFF_BOSS_READY         = 8
local OFF_IS_IN_SCRIPT       = 9
local OFF_PARTNER_IN_SCRIPT  = 10
local OFF_POS_FRAME_COUNTER  = 11

-- ---- Helper: push one byte into recvRing ----------------------------
local function recvPush(byte)
    local head = memory.read_u8(sym.gMpRecvRing + RING_HEAD_OFF)
    memory.write_u8(sym.gMpRecvRing + RING_BUF_OFF + head, byte)
    memory.write_u8(sym.gMpRecvRing + RING_HEAD_OFF, (head + 1) % 256)
end

-- ---- Test 1: Ring buffers initialised --------------------------------
local function testRingMagic()
    local sm = memory.read_u8(sym.gMpSendRing + RING_MAGIC_OFF)
    local rm = memory.read_u8(sym.gMpRecvRing + RING_MAGIC_OFF)
    check(sm == MP_RING_MAGIC, "gMpSendRing magic == 0xC0")
    check(rm == MP_RING_MAGIC, "gMpRecvRing magic == 0xC0")
end

-- ---- Test 2: partnerIsInScript starts FALSE --------------------------
local function testPartnerInScriptInitial()
    local v = memory.read_u8(sym.gMultiplayerState + OFF_PARTNER_IN_SCRIPT)
    check(v == 0, "partnerIsInScript starts FALSE")
end

-- ---- Test 3: SCRIPT_LOCK received → partnerIsInScript = TRUE --------
-- This writes to the recv ring; the result is visible after the next
-- Multiplayer_Update() call (i.e., one game frame).
local function queueScriptLockTest()
    recvPush(MP_PKT_SCRIPT_LOCK)
    print("  >> SCRIPT_LOCK pushed to recv ring. Wait one frame, then run checkScriptLock().")
end

local function checkScriptLock()
    local v = memory.read_u8(sym.gMultiplayerState + OFF_PARTNER_IN_SCRIPT)
    check(v ~= 0, "partnerIsInScript == TRUE after SCRIPT_LOCK recv")
end

-- ---- Test 4: SCRIPT_UNLOCK received → partnerIsInScript = FALSE -----
local function queueScriptUnlockTest()
    recvPush(MP_PKT_SCRIPT_UNLOCK)
    print("  >> SCRIPT_UNLOCK pushed to recv ring. Wait one frame, then run checkScriptUnlock().")
end

local function checkScriptUnlock()
    local v = memory.read_u8(sym.gMultiplayerState + OFF_PARTNER_IN_SCRIPT)
    check(v == 0, "partnerIsInScript == FALSE after SCRIPT_UNLOCK recv")
end

-- ---- Test 5: FLAG_SET packet queued → send ring grows ---------------
-- This test must be run while connState == MP_STATE_CONNECTED.
-- It verifies that after FlagSet() is called in the ROM, a FLAG_SET
-- packet appears in gMpSendRing.  Manual step: open a trainer battle
-- to trigger a FlagSet, then call checkFlagSetInSendRing().
local function checkFlagSetInSendRing()
    local head = memory.read_u8(sym.gMpSendRing + RING_HEAD_OFF)
    local tail = memory.read_u8(sym.gMpSendRing + RING_TAIL_OFF)
    local avail = (head - tail) % 256
    check(avail >= 3, string.format("send ring has >= 3 bytes (FLAG_SET); got %d", avail))
    if avail >= 1 then
        local typeByte = memory.read_u8(sym.gMpSendRing + RING_BUF_OFF + tail)
        check(typeByte == MP_PKT_FLAG_SET, string.format("first queued packet type == FLAG_SET (0x02); got 0x%02X", typeByte))
    end
end

-- ---- Run immediate (non-frame-dependent) tests -----------------------
testRingMagic()
testPartnerInScriptInitial()

-- ---- Queue multi-frame tests (call checkX after next frame) ----------
print("")
print("Step-by-step tests (require game frame between queue and check):")
print("  1. queueScriptLockTest()  → wait one frame → checkScriptLock()")
print("  2. queueScriptUnlockTest() → wait one frame → checkScriptUnlock()")
print("  3. (trigger trainer defeat) → checkFlagSetInSendRing()")
print("")
print(string.format("Immediate results: %d passed, %d failed", PASS, FAIL))
