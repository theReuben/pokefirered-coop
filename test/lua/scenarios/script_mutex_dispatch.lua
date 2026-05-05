-- test/lua/scenarios/script_mutex_dispatch.lua
-- Push MP_PKT_SCRIPT_LOCK into gMpRecvRing and assert that the ROM's
-- Multiplayer_Update transitions gMultiplayerState.partnerIsInScript to TRUE.
-- Then push SCRIPT_UNLOCK and verify the reverse.
--
-- Tests the advisory mutex that prevents two players from simultaneously
-- triggering the same NPC interaction.

local H  = require("_harness")
local mm = require("memory_map")

H.check(H.waitForInit(mm), "init ran before scenario starts")

-- Initial state: partnerIsInScript starts FALSE.
H.checkEq(H.read8(mm.gMultiplayerState + H.MP_OFF_PARTNER_IN_SCRIPT),
          0, "partnerIsInScript starts FALSE")

-- SCRIPT_LOCK received → partnerIsInScript = TRUE.
H.recvPush(mm.gMpRecvRing, H.MP_PKT_SCRIPT_LOCK)
H.runFrames(2)

H.checkEq(H.read8(mm.gMultiplayerState + H.MP_OFF_PARTNER_IN_SCRIPT),
          1, "SCRIPT_LOCK sets partnerIsInScript = TRUE")

-- SCRIPT_UNLOCK received → partnerIsInScript = FALSE.
H.recvPush(mm.gMpRecvRing, H.MP_PKT_SCRIPT_UNLOCK)
H.runFrames(2)

H.checkEq(H.read8(mm.gMultiplayerState + H.MP_OFF_PARTNER_IN_SCRIPT),
          0, "SCRIPT_UNLOCK clears partnerIsInScript = FALSE")

-- Recv ring must be fully drained.
H.checkEq(H.ringAvailable(mm.gMpRecvRing), 0, "gMpRecvRing fully drained")

H.finish("script_mutex_dispatch")
