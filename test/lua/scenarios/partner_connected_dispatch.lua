-- test/lua/scenarios/partner_connected_dispatch.lua
-- Push MP_PKT_PARTNER_CONNECTED into gMpRecvRing and assert that the
-- ROM's Multiplayer_Update dispatch transitions gMultiplayerState.connState
-- from MP_STATE_DISCONNECTED to MP_STATE_CONNECTED.
--
-- This is the wire-level handshake the Tauri host triggers when the relay
-- server reports "partner_connected" — the only way the ROM ever leaves
-- the disconnected state at runtime.

local H  = require("_harness")
local mm = require("memory_map")

H.check(H.waitForInit(mm), "init ran before scenario starts")

H.checkEq(H.read8(mm.gMultiplayerState + H.MP_OFF_CONN_STATE),
          H.MP_STATE_DISCONNECTED, "starts disconnected")

H.recvPush(mm.gMpRecvRing, H.MP_PKT_PARTNER_CONNECTED)
H.runFrames(2)

H.checkEq(H.read8(mm.gMultiplayerState + H.MP_OFF_CONN_STATE),
          H.MP_STATE_CONNECTED, "PARTNER_CONNECTED transitions connState")

-- Now disconnect and assert the reverse, including ghost despawn flag.
H.recvPush(mm.gMpRecvRing, H.MP_PKT_PARTNER_DISCONNECTED)
H.runFrames(2)

H.checkEq(H.read8(mm.gMultiplayerState + H.MP_OFF_CONN_STATE),
          H.MP_STATE_DISCONNECTED, "PARTNER_DISCONNECTED reverts connState")

-- ghostObjectEventId must be the GHOST_INVALID_SLOT sentinel (0xFF) since
-- no ghost was ever spawned in this scenario.
H.checkEq(H.read8(mm.gMultiplayerState + H.MP_OFF_GHOST_OBJ_ID),
          0xFF, "ghost slot remains invalid after disconnect")

H.finish("partner_connected_dispatch")
