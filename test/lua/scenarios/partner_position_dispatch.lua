-- test/lua/scenarios/partner_position_dispatch.lua
-- Push a synthetic MP_PKT_POSITION packet into gMpRecvRing, advance one
-- frame, and assert that the in-ROM ProcessOneRecvPacket dispatcher
-- routed it through Multiplayer_UpdateGhostPosition, leaving the partner
-- target fields populated in gMultiplayerState.
--
-- Verifies the same code path that the Tauri host exercises every time
-- a partner_position arrives over WebSocket — but without the network.

local H  = require("_harness")
local mm = require("memory_map")

H.check(H.waitForInit(mm), "init ran before scenario starts")

-- Packet payload (matches Mp_EncodePosition layout):
--   [type=0x01][mapGroup][mapNum][x][y][facing]
local mapGroup, mapNum, x, y, facing = 3, 7, 19, 23, 2  -- DIR_NORTH = 2
H.recvPushMany(mm.gMpRecvRing, {
    H.MP_PKT_POSITION, mapGroup, mapNum, x, y, facing
})

-- One frame is enough: Multiplayer_Update drains the ring.
H.runFrames(2)

H.checkEq(H.read8(mm.gMultiplayerState + H.MP_OFF_PARTNER_MAP_GROUP),
          mapGroup, "partnerMapGroup updated from packet")
H.checkEq(H.read8(mm.gMultiplayerState + H.MP_OFF_PARTNER_MAP_NUM),
          mapNum, "partnerMapNum updated from packet")
H.checkEq(H.read8(mm.gMultiplayerState + H.MP_OFF_TARGET_X),
          x, "targetX updated from packet")
H.checkEq(H.read8(mm.gMultiplayerState + H.MP_OFF_TARGET_Y),
          y, "targetY updated from packet")
H.checkEq(H.read8(mm.gMultiplayerState + H.MP_OFF_TARGET_FACING),
          facing, "targetFacing updated from packet")

-- Recv ring must have been fully drained.
H.checkEq(H.ringAvailable(mm.gMpRecvRing), 0,
          "gMpRecvRing drained after dispatch")

H.finish("partner_position_dispatch")
