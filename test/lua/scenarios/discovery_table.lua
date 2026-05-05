-- test/lua/scenarios/discovery_table.lua
-- Verifies that Multiplayer_Init populates the IWRAM discovery table
-- so the Tauri host can find every key co-op global by scanning for
-- MP_DISCOVERY_MAGIC. This is the contract between ROM and host.

local H  = require("_harness")
local mm = require("memory_map")

if not H.waitForInit(mm) then
    H.check(false, "Multiplayer_Init never ran (ring magic bytes missing)")
    H.finish("discovery_table")
    return
end

if mm.gMpAddrTable == nil then
    H.check(false, "memory_map.lua missing gMpAddrTable — regenerate via tools/extract_symbols.py")
    H.finish("discovery_table")
    return
end

local base = mm.gMpAddrTable

-- gMpAddrTable[0] must be the discovery magic so the scan succeeds.
H.checkEq(H.read32(base + 0), H.MP_DISCOVERY_MAGIC, "gMpAddrTable[0] == MP_DISCOVERY_MAGIC")

-- gMpAddrTable[1..4] must point at the four globals exposed to the host.
H.checkEq(H.read32(base + 4),  mm.gMultiplayerState, "gMpAddrTable[1] = &gMultiplayerState")
H.checkEq(H.read32(base + 8),  mm.gMpSendRing,       "gMpAddrTable[2] = &gMpSendRing")
H.checkEq(H.read32(base + 12), mm.gMpRecvRing,       "gMpAddrTable[3] = &gMpRecvRing")
H.checkEq(H.read32(base + 16), mm.gCoopSettings,     "gMpAddrTable[4] = &gCoopSettings")

H.finish("discovery_table")
