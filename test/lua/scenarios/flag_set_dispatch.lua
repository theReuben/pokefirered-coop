-- test/lua/scenarios/flag_set_dispatch.lua
-- Push MP_PKT_FLAG_SET for FLAG_DEFEATED_BROCK (0x4B0) into gMpRecvRing
-- and assert the ROM applies it to the save block flags array.
--
-- FLAG_DEFEATED_BROCK = 0x4B0:  byte = 0x4B0/8 = 150,  bit = 0x4B0%8 = 0
-- Save block flags live at: *gSaveBlock1Ptr + 0x1270 + byteIndex
--
-- Requires gSaveBlock1Ptr in memory_map.lua (regenerate with make check-lua
-- after a ROM rebuild if missing).

local H  = require("_harness")
local mm = require("memory_map")

if mm.gSaveBlock1Ptr == nil then
    H.check(false, "memory_map.lua missing gSaveBlock1Ptr — run make check-lua to regenerate")
    H.finish("flag_set_dispatch")
    return
end

H.check(H.waitForInit(mm), "init ran before scenario starts")

local FLAGS_OFF  = 0x1270  -- flags[] offset within SaveBlock1 (global.h:1128)
local BROCK_BYTE = 150     -- 0x4B0 / 8
local BROCK_BIT  = 0       -- 0x4B0 % 8

-- Dereference the save-block pointer.
local saveBlock1 = H.read32(mm.gSaveBlock1Ptr)
H.check(saveBlock1 ~= 0, "gSaveBlock1Ptr is non-null (new game initialised save)")

-- Flag must start clear.
local before = H.read8(saveBlock1 + FLAGS_OFF + BROCK_BYTE)
H.check((before & (1 << BROCK_BIT)) == 0,
        "FLAG_DEFEATED_BROCK (0x4B0) starts clear")

-- Inject FLAG_SET packet: [type=0x02][flagId_hi=0x04][flagId_lo=0xB0]
H.recvPushMany(mm.gMpRecvRing, {H.MP_PKT_FLAG_SET, 0x04, 0xB0})
H.runFrames(2)

-- The ROM's ProcessOneRecvPacket should have called Multiplayer_HandleRemoteFlagSet
-- which calls FlagSet(0x4B0), writing bit 0 of byte 150 in the flags array.
local after = H.read8(saveBlock1 + FLAGS_OFF + BROCK_BYTE)
H.check((after & (1 << BROCK_BIT)) ~= 0,
        "FLAG_DEFEATED_BROCK set after FLAG_SET dispatch")

-- Recv ring must be drained.
H.checkEq(H.ringAvailable(mm.gMpRecvRing), 0, "gMpRecvRing fully drained")

H.finish("flag_set_dispatch")
