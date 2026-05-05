-- test/lua/coop/handshake/p1.lua
-- Two-instance smoke test (sender side).
--
-- Boots, waits for Multiplayer_Init, then writes a synthetic POSITION
-- packet directly into its own gMpSendRing. The orchestrator's bridge
-- copies those bytes to the partner's inbox; the partner asserts the
-- payload arrived intact and was dispatched into gMultiplayerState.
--
-- This proves end-to-end that:
--   * Lua can write into gMpSendRing
--   * The orchestrator copies bytes between instance directories
--   * The receiver pumps inbox.bin into gMpRecvRing
--   * The receiver's ProcessOneRecvPacket dispatches the packet
-- without depending on save state, intro scripts, or input simulation.

local D  = require("_driver")
local H  = D.H
local mm = D.mm

D.run("handshake_p1", function(d)
    -- Push a recognisable POSITION packet into our own sendRing.
    -- Bytes: [type=0x01][mapGroup=5][mapNum=7][x=11][y=13][facing=DIR_NORTH(2)]
    H.recvPush(mm.gMpSendRing, H.MP_PKT_POSITION)
    H.recvPush(mm.gMpSendRing, 5)
    H.recvPush(mm.gMpSendRing, 7)
    H.recvPush(mm.gMpSendRing, 11)
    H.recvPush(mm.gMpSendRing, 13)
    H.recvPush(mm.gMpSendRing, 2)

    -- Step a generous number of frames so the orchestrator can drain
    -- our outbox, deliver to the partner, and the partner's
    -- Multiplayer_Update can consume + dispatch. The partner side
    -- asserts the actual delivery; this side just keeps the bridge
    -- ticking until both finish.
    d.step(120)
end)
