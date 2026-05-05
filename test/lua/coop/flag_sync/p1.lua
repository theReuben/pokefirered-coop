-- test/lua/coop/flag_sync/p1.lua
-- Two-instance flag-sync smoke test (sender side).
--
-- p1 simulates a connected ROM by:
--   1. Receiving PARTNER_CONNECTED so connState == CONNECTED
--   2. Injecting a FLAG_SET packet directly into its own gMpSendRing
--      (simulating what the ROM would emit when FlagSet(0x4B0) is called
--      while connected)
--
-- The orchestrator bridges p1's sendRing → p2's recvRing.
-- p2 asserts the in-ROM ProcessOneRecvPacket applied the flag.
--
-- FLAG_DEFEATED_BROCK = 0x4B0

local D  = require("_driver")
local H  = D.H
local mm = D.mm

D.run("flag_sync_p1", function(d)
    -- Become "connected" so the ROM won't suppress flag sends.
    H.recvPush(mm.gMpRecvRing, H.MP_PKT_PARTNER_CONNECTED)
    d.step(3)

    d.assertCond(
        H.read8(mm.gMultiplayerState + H.MP_OFF_CONN_STATE) == H.MP_STATE_CONNECTED,
        "p1 connState == CONNECTED"
    )

    -- Inject FLAG_SET(0x4B0) into our own sendRing to be bridged to p2.
    -- Layout: [MP_PKT_FLAG_SET][flagId_hi=0x04][flagId_lo=0xB0]
    H.recvPush(mm.gMpSendRing, H.MP_PKT_FLAG_SET)
    H.recvPush(mm.gMpSendRing, 0x04)
    H.recvPush(mm.gMpSendRing, 0xB0)

    -- Step enough frames so the orchestrator can bridge the packet and
    -- p2's ROM can dispatch it. 120 frames ≈ 2 s of emulated time.
    d.step(120)
end)
