-- test/lua/coop/handshake/p2.lua
-- Two-instance smoke test (receiver side).
--
-- Waits for the orchestrator's bridge to deliver p1's POSITION packet,
-- then asserts that the in-ROM dispatcher routed it into the partner-
-- target fields of gMultiplayerState. Demonstrates that the entire
-- send→bridge→recv→dispatch pipeline works in a real two-ROM setup.

local D  = require("_driver")
local H  = D.H
local mm = D.mm

D.run("handshake_p2", function(d)
    -- Initial state: partnerMapGroup is sentinel 0xFF set by Multiplayer_Init.
    d.assertCond(
        H.read8(mm.gMultiplayerState + H.MP_OFF_PARTNER_MAP_GROUP) == 0xFF,
        "partnerMapGroup starts at sentinel 0xFF"
    )

    -- Wait up to ~3 seconds for the bytes to arrive and dispatch.
    -- Predicate: targetX is now 11 (the value p1 encoded).
    local ok = d.waitFor(function()
        return H.read8(mm.gMultiplayerState + H.MP_OFF_TARGET_X) == 11
    end, 180, "POSITION packet to dispatch (targetX==11)")
    if not ok then return end -- waitFor already failed the test

    -- Verify every field landed correctly.
    d.assertCond(
        H.read8(mm.gMultiplayerState + H.MP_OFF_PARTNER_MAP_GROUP) == 5,
        "partnerMapGroup == 5"
    )
    d.assertCond(
        H.read8(mm.gMultiplayerState + H.MP_OFF_PARTNER_MAP_NUM) == 7,
        "partnerMapNum == 7"
    )
    d.assertCond(
        H.read8(mm.gMultiplayerState + H.MP_OFF_TARGET_Y) == 13,
        "targetY == 13"
    )
    d.assertCond(
        H.read8(mm.gMultiplayerState + H.MP_OFF_TARGET_FACING) == 2,
        "targetFacing == 2"
    )
end)
