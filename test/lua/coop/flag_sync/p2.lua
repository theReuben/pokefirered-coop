-- test/lua/coop/flag_sync/p2.lua
-- Two-instance flag-sync smoke test (receiver side).
--
-- Waits for the save block to exist, signals readiness to p1, then waits
-- for p1's FLAG_SET(0x4B0) to arrive via the bridge and asserts that the
-- ROM's ProcessOneRecvPacket called FlagSet and the bit is set in the
-- save block flags array.
--
-- Ordering matters: gSaveBlock1Ptr is NULL until SetSaveBlocksPointers
-- runs during the intro (intro.c), hundreds of frames after
-- Multiplayer_Init. A FLAG_SET dispatched before that is dropped by the
-- NULL-saveblock guard in event_data.c, so p1 must not inject until we
-- signal "saveblock_ready".
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

    -- Save block pointers are set mid-intro; run frames until then.
    local ok = d.waitFor(function()
        return H.read32(mm.gSaveBlock1Ptr) ~= 0
    end, 1800, "gSaveBlock1Ptr to become non-null (intro boot)")
    if not ok then return end

    local saveBlock1 = H.read32(mm.gSaveBlock1Ptr)

    -- Flag must start clear.
    local before = H.read8(saveBlock1 + FLAGS_OFF + BROCK_BYTE)
    d.assertCond((before & (1 << BROCK_BIT)) == 0,
                 "FLAG_DEFEATED_BROCK starts clear on p2")

    -- Tell p1 it is now safe to inject the FLAG_SET packet.
    d.signal("saveblock_ready")

    -- Wait for the bridge to deliver and the ROM to dispatch the flag.
    ok = d.waitFor(function()
        local v = H.read8(saveBlock1 + FLAGS_OFF + BROCK_BYTE)
        return (v & (1 << BROCK_BIT)) ~= 0
    end, 1800, "FLAG_DEFEATED_BROCK to be set via bridge")
    if not ok then return end
    -- D.run auto-calls D.pass when we return without failing.
end)
