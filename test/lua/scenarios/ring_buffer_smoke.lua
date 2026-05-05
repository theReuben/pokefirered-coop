-- test/lua/scenarios/ring_buffer_smoke.lua
-- Smoke test: after the ROM boots and Multiplayer_Init runs, both ring
-- buffers carry MP_RING_MAGIC (0xC0) at their `magic` field. This proves
-- (1) Multiplayer_Init was reached during boot, (2) the symbol map
-- addresses are correct, and (3) IWRAM is reachable from Lua.

local H  = require("_harness")
local mm = require("memory_map")

H.check(H.waitForInit(mm), "ring magic bytes appeared within init timeout")

H.checkEq(H.read8(mm.gMpSendRing + H.RING_MAGIC_OFF),
          H.MP_RING_MAGIC, "gMpSendRing.magic == 0xC0")
H.checkEq(H.read8(mm.gMpRecvRing + H.RING_MAGIC_OFF),
          H.MP_RING_MAGIC, "gMpRecvRing.magic == 0xC0")

-- Both rings start empty.
H.checkEq(H.ringAvailable(mm.gMpSendRing), 0, "gMpSendRing empty at boot")
H.checkEq(H.ringAvailable(mm.gMpRecvRing), 0, "gMpRecvRing empty at boot")

H.finish("ring_buffer_smoke")
