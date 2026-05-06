-- tools/mcp_gamestate/bridge.lua
-- Runs inside mgba-headless (--script flag).  Provides a file-based IPC
-- channel so the MCP server can read memory, inject packets, advance
-- frames, and take screenshots without scripting against the emulator
-- process directly.
--
-- IPC protocol (atomic file rename, so reads never see partial writes):
--   CMD_FILE  → Python writes JSON command; Lua reads, deletes, executes.
--   RESP_FILE ← Lua writes JSON response; Python reads, deletes.
--   READY_FILE← Lua creates on startup; Python waits for it.
--
-- Command format: {"cmd":"<name>", ...args}
-- Response format: {"ok":true/false, ...fields}  (always one JSON line)

local CMD_FILE   = os.getenv("MGBA_BRIDGE_CMD")   or "/tmp/mgba_bridge_cmd.json"
local RESP_FILE  = os.getenv("MGBA_BRIDGE_RESP")  or "/tmp/mgba_bridge_resp.json"
local READY_FILE = os.getenv("MGBA_BRIDGE_READY") or "/tmp/mgba_bridge_ready"

-- ── Minimal JSON encode/decode ────────────────────────────────────────────

local function json_encode_val(v)
    local t = type(v)
    if t == "string"  then return string.format('"%s"', (v:gsub('\\','\\\\'):gsub('"','\\"'))) end
    if t == "number"  then return string.format("%d", v) end
    if t == "boolean" then return tostring(v) end
    if t == "table"   then
        local parts = {}
        for k, val in pairs(v) do
            parts[#parts+1] = string.format('"%s":%s', k, json_encode_val(val))
        end
        return "{" .. table.concat(parts, ",") .. "}"
    end
    return "null"
end

local function json_encode(t) return json_encode_val(t) end

-- Minimal flat-object parser: handles string and integer values only.
local function json_decode(s)
    local t = {}
    for k, v in s:gmatch('"([%w_]+)"%s*:%s*"([^"]*)"') do t[k] = v end
    for k, v in s:gmatch('"([%w_]+)"%s*:%s*(-?%d+)')   do t[k] = tonumber(v) end
    for k    in s:gmatch('"([%w_]+)"%s*:%s*true')       do t[k] = true end
    for k    in s:gmatch('"([%w_]+)"%s*:%s*false')      do t[k] = false end
    return t
end

-- ── Address map (injected by MCP server as env vars) ─────────────────────

local ADDR = {}
local ADDR_NAMES = {
    "gMultiplayerState", "gMpSendRing", "gMpRecvRing",
    "gCoopSettings", "gSaveBlock1Ptr", "gSaveblock1", "gMpAddrTable",
}
for _, name in ipairs(ADDR_NAMES) do
    local env_key = "ADDR_" .. name:upper()
    local val = os.getenv(env_key)
    if val then ADDR[name] = tonumber(val) end
end

-- ── Ring buffer helpers ───────────────────────────────────────────────────

local RING_BUF_OFF  = 0
local RING_HEAD_OFF = 256
local RING_TAIL_OFF = 257
local RING_MAGIC_OFF = 258

local function ring_push(base, bytes)
    for _, b in ipairs(bytes) do
        local head = emu:read8(base + RING_HEAD_OFF)
        emu:write8(base + RING_BUF_OFF + head, b & 0xFF)
        emu:write8(base + RING_HEAD_OFF, (head + 1) & 0xFF)
    end
end

local function ring_drain(base)
    local out = {}
    while true do
        local head = emu:read8(base + RING_HEAD_OFF)
        local tail = emu:read8(base + RING_TAIL_OFF)
        if head == tail then break end
        out[#out+1] = emu:read8(base + RING_BUF_OFF + tail)
        emu:write8(base + RING_TAIL_OFF, (tail + 1) & 0xFF)
    end
    return out
end

local function ring_available(base)
    local h = emu:read8(base + RING_HEAD_OFF)
    local t = emu:read8(base + RING_TAIL_OFF)
    return (h - t) & 0xFF
end

-- ── Save-block flag access ────────────────────────────────────────────────

local FLAGS_OFFSET = 0x1270  -- flags[] within SaveBlock1

local function resolve_save_block()
    if not ADDR.gSaveBlock1Ptr then return nil end
    local ptr = emu:read32(ADDR.gSaveBlock1Ptr)
    if ptr == 0 then
        -- headless boot: wire pointer to static gSaveblock1 struct
        if ADDR.gSaveblock1 then
            emu:write32(ADDR.gSaveBlock1Ptr, ADDR.gSaveblock1)
            return ADDR.gSaveblock1
        end
        return nil
    end
    return ptr
end

local function check_flag(flag_id)
    local save = resolve_save_block()
    if not save then return nil end
    local byte_idx = math.floor(flag_id / 8)
    local bit_idx  = flag_id % 8
    local byte_val = emu:read8(save + FLAGS_OFFSET + byte_idx)
    return (byte_val & (1 << bit_idx)) ~= 0
end

-- ── MultiplayerState field offsets ───────────────────────────────────────

local MP = {
    role=0, connState=1, partnerMapGroup=2, partnerMapNum=3,
    targetX=4, targetY=5, targetFacing=6, ghostObjId=7,
    bossReady=8, partnerBossId=9, isInScript=10, partnerIsInScript=11,
    posFrameCounter=12,
}

local function get_mp_state()
    if not ADDR.gMultiplayerState then return {error="no gMultiplayerState address"} end
    local b = ADDR.gMultiplayerState
    local s = {
        role              = emu:read8(b + MP.role),
        connState         = emu:read8(b + MP.connState),
        partnerMapGroup   = emu:read8(b + MP.partnerMapGroup),
        partnerMapNum     = emu:read8(b + MP.partnerMapNum),
        targetX           = emu:read8(b + MP.targetX),
        targetY           = emu:read8(b + MP.targetY),
        targetFacing      = emu:read8(b + MP.targetFacing),
        ghostObjId        = emu:read8(b + MP.ghostObjId),
        isInScript        = emu:read8(b + MP.isInScript),
        partnerIsInScript = emu:read8(b + MP.partnerIsInScript),
    }
    if ADDR.gMpSendRing then s.sendAvail = ring_available(ADDR.gMpSendRing) end
    if ADDR.gMpRecvRing then s.recvAvail = ring_available(ADDR.gMpRecvRing) end
    if ADDR.gMpSendRing then
        s.ringsMagicOk = (emu:read8(ADDR.gMpSendRing + RING_MAGIC_OFF) == 0xC0) and
                         (emu:read8(ADDR.gMpRecvRing + RING_MAGIC_OFF) == 0xC0)
    end
    return s
end

-- ── Command dispatch ─────────────────────────────────────────────────────

local running = true

local function execute(cmd)
    local c = cmd.cmd
    if not c then return {ok=false, error="missing cmd field"} end

    if c == "ping" then
        return {ok=true, msg="pong"}

    elseif c == "quit" then
        running = false
        return {ok=true}

    elseif c == "run" then
        local n = cmd.frames or 1
        for _ = 1, n do emu:runFrame() end
        return {ok=true, frames=n}

    elseif c == "r8" then
        if not cmd.addr then return {ok=false, error="missing addr"} end
        return {ok=true, val=emu:read8(cmd.addr)}

    elseif c == "r16" then
        if not cmd.addr then return {ok=false, error="missing addr"} end
        return {ok=true, val=emu:read16(cmd.addr)}

    elseif c == "r32" then
        if not cmd.addr then return {ok=false, error="missing addr"} end
        return {ok=true, val=emu:read32(cmd.addr)}

    elseif c == "w8" then
        emu:write8(cmd.addr, cmd.val)
        return {ok=true}

    elseif c == "w16" then
        emu:write16(cmd.addr, cmd.val)
        return {ok=true}

    elseif c == "w32" then
        emu:write32(cmd.addr, cmd.val)
        return {ok=true}

    elseif c == "state" then
        local s = get_mp_state()
        s.ok = (s.error == nil)
        return s

    elseif c == "inject" then
        -- bytes: space-separated hex, e.g. "0B" for PARTNER_CONNECTED
        if not ADDR.gMpRecvRing then return {ok=false, error="no gMpRecvRing address"} end
        local bytes = {}
        for hex in (cmd.bytes or ""):gmatch("%S+") do
            bytes[#bytes+1] = tonumber(hex, 16) or 0
        end
        ring_push(ADDR.gMpRecvRing, bytes)
        return {ok=true, pushed=#bytes}

    elseif c == "drain" then
        if not ADDR.gMpSendRing then return {ok=false, error="no gMpSendRing address"} end
        local bytes = ring_drain(ADDR.gMpSendRing)
        local hex = {}
        for _, b in ipairs(bytes) do hex[#hex+1] = string.format("%02X", b) end
        return {ok=true, bytes=table.concat(hex, " "), count=#bytes}

    elseif c == "flag" then
        if cmd.id == nil then return {ok=false, error="missing id"} end
        local result = check_flag(cmd.id)
        if result == nil then return {ok=false, error="save block not available"} end
        return {ok=true, val=result and 1 or 0, set=result}

    elseif c == "keys" then
        emu:setKeys(cmd.mask or 0)
        return {ok=true, mask=cmd.mask or 0}

    elseif c == "press" then
        local mask    = cmd.mask or 0
        local hold    = cmd.hold or 3
        local release = cmd.release or 3
        emu:setKeys(mask)
        for _ = 1, hold do emu:runFrame() end
        emu:setKeys(0)
        for _ = 1, release do emu:runFrame() end
        return {ok=true, held=hold, released=release}

    elseif c == "loadstate" then
        if not cmd.path then return {ok=false, error="missing path"} end
        local f = io.open(cmd.path, "rb")
        if not f then return {ok=false, error="cannot open: " .. cmd.path} end
        local data = f:read("*a"); f:close()
        local ok, err = pcall(function() emu:loadStateBuffer(data, 31) end)
        if ok then return {ok=true, path=cmd.path}
        else return {ok=false, error=tostring(err)} end

    elseif c == "savestate" then
        local path = cmd.path or "/tmp/mgba_savestate.ss0"
        local buf, err = nil, nil
        local ok = pcall(function() buf = emu:saveStateBuffer(31) end)
        if not ok or not buf then return {ok=false, error="saveStateBuffer failed"} end
        local f = io.open(path, "wb")
        if not f then return {ok=false, error="cannot write: " .. path} end
        f:write(buf); f:close()
        return {ok=true, path=path}

    elseif c == "screenshot" then
        local path = cmd.path or "/tmp/mgba_screenshot.png"
        local ok, err = pcall(function() emu:screenshot(path) end)
        if ok then return {ok=true, path=path}
        else return {ok=false, error=tostring(err)} end

    elseif c == "addrs" then
        local out = {ok=true}
        for k, v in pairs(ADDR) do out[k] = v end
        return out

    else
        return {ok=false, error="unknown command: " .. tostring(c)}
    end
end

-- ── Atomic file write ─────────────────────────────────────────────────────

local function atomic_write(path, data)
    local tmp = path .. ".tmp"
    local f = io.open(tmp, "w")
    if not f then return false end
    f:write(data)
    f:close()
    return os.rename(tmp, path)
end

-- ── Main loop ─────────────────────────────────────────────────────────────

-- Load initial save state if requested (set via MGBA_BRIDGE_SAVESTATE env var).
local INIT_SAVESTATE = os.getenv("MGBA_BRIDGE_SAVESTATE")
if INIT_SAVESTATE and INIT_SAVESTATE ~= "" then
    local f = io.open(INIT_SAVESTATE, "rb")
    if f then
        local data = f:read("*a"); f:close()
        pcall(function() emu:loadStateBuffer(data, 31) end)
    end
end

-- Signal readiness to the MCP server.
atomic_write(READY_FILE, "ready\n")

while running do
    emu:runFrame()

    local f = io.open(CMD_FILE, "r")
    if f then
        local data = f:read("*a")
        f:close()
        os.remove(CMD_FILE)

        local ok, result = pcall(function()
            return execute(json_decode(data))
        end)
        if not ok then result = {ok=false, error=tostring(result)} end

        atomic_write(RESP_FILE, json_encode(result) .. "\n")
    end
end

if emu and emu.quit then emu:quit() end
