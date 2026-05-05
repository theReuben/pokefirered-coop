-- test/lua/coop/flag_sync/p2.lua
-- Two-instance flag-sync smoke test (receiver side).
--
-- Waits for p1's FLAG_SET(0x4B0) to arrive via the bridge, then asserts
-- that the ROM's ProcessOneRecvPacket called FlagSet and the bit is set
-- in the save block flags array.
--
-- FLAG_DEFEATED_BROCK = 0x4B0: byte=150, bit=0 in SaveBlock1.flags[].

local D  = require("_driver")
local H  = D.H
local mm = D.mm

D.run("flag_sync_p2", function(d)
    if mm.gSaveBlock1Ptr == nil then
        D.fail("flag_sync_p2", "memory_map.lua missing gSaveBlock1Ptr")
        return
    end

    local FLAGS_OFF  = 0x1270
    local BROCK_BYTE = 150
    local BROCK_BIT  = 0

    local saveBlock1 = H.read32(mm.gSaveBlock1Ptr)
    d.assertCond(saveBlock1 ~= 0, "gSaveBlock1Ptr non-null")

    -- Flag must start clear.
    local before = H.read8(saveBlock1 + FLAGS_OFF + BROCK_BYTE)
    d.assertCond((before & (1 << BROCK_BIT)) == 0,
                 "FLAG_DEFEATED_BROCK starts clear on p2")

    -- Wait up to ~3 s for the bridge to deliver and dispatch the flag.
    local ok = d.waitFor(function()
        local v = H.read8(saveBlock1 + FLAGS_OFF + BROCK_BYTE)
        return (v & (1 << BROCK_BIT)) ~= 0
    end, 180, "FLAG_DEFEATED_BROCK to be set via bridge")
    if not ok then return end
    -- D.run auto-calls D.pass when we return without failing.
end)
