-- tools/mcp_gamestate/bridge.lua
-- Runs inside mgba-headless (--script flag).
-- Simulates a human player: screenshot, press buttons, wait.
--
-- IPC protocol (atomic rename, never partial reads):
--   CMD_FILE   → Python writes JSON command; Lua reads, deletes, executes.
--   RESP_FILE  ← Lua writes JSON response; Python reads, deletes.
--   READY_FILE ← Lua creates on startup; Python waits for it.
--
-- Fully callback-based (no main while loop) so emu:screenshot() is always
-- called from within the frame callback where the renderer is safe.

local CMD_FILE   = os.getenv("MGBA_BRIDGE_CMD")   or "/tmp/mgba_bridge_cmd.json"
local RESP_FILE  = os.getenv("MGBA_BRIDGE_RESP")  or "/tmp/mgba_bridge_resp.json"
local READY_FILE = os.getenv("MGBA_BRIDGE_READY") or "/tmp/mgba_bridge_ready"

-- ── Minimal JSON encode/decode ────────────────────────────────────────────

local function json_encode_val(v)
    local t = type(v)
    if t == "string"  then
        return string.format('"%s"',
            v:gsub('\\','\\\\'):gsub('"','\\"'):gsub('\n','\\n'):gsub('\r','\\r'):gsub('\t','\\t'))
    end
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

local function json_decode(s)
    local t = {}
    for k, v in s:gmatch('"([%w_]+)"%s*:%s*"([^"]*)"') do t[k] = v end
    for k, v in s:gmatch('"([%w_]+)"%s*:%s*(-?%d+)')   do t[k] = tonumber(v) end
    for k    in s:gmatch('"([%w_]+)"%s*:%s*true')       do t[k] = true end
    for k    in s:gmatch('"([%w_]+)"%s*:%s*false')      do t[k] = false end
    return t
end

-- ── Ring buffer helpers (used by internal drain/inject relay commands) ────

local ADDR = {}
local ADDR_NAMES = {
    "gMpSendRing", "gMpRecvRing",
    "gMultiplayerState", "gCoopSettings", "gSaveBlock1Ptr", "gSaveblock1", "gMpAddrTable",
    "gMpBlockExchange",
    "gStringVar4", "sFirstTextPrinter", "gDisableTextPrinters", "sGlobalScriptContextStatus",
}
for _, name in ipairs(ADDR_NAMES) do
    local val = os.getenv("ADDR_" .. name:upper())
    if val then ADDR[name] = tonumber(val) end
end

-- ── FireRed charset → ASCII (for get_text command) ───────────────────────
-- Source: charmap.txt in repo root.
local CHARSET = {}
CHARSET[0x00] = " "
for i = 0, 9  do CHARSET[0xA1 + i] = string.char(48 + i) end  -- '0'..'9'
CHARSET[0xAB] = "!"
CHARSET[0xAC] = "?"
CHARSET[0xAD] = "."
CHARSET[0xAE] = "-"
CHARSET[0xB3] = "'"
CHARSET[0xB4] = "'"
CHARSET[0xB8] = ","
CHARSET[0xBA] = "/"
for i = 0, 25 do CHARSET[0xBB + i] = string.char(65 + i) end  -- 'A'..'Z'
for i = 0, 25 do CHARSET[0xD5 + i] = string.char(97 + i) end  -- 'a'..'z'
CHARSET[0xFE] = "\n"
-- 0xFF = EOS (handled as loop terminator, not in table)
-- 0xFD = multi-byte variable escape (skip next byte, handled in decoder)

local RING_BUF_OFF  = 0
local RING_HEAD_OFF = 256
local RING_TAIL_OFF = 257

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

-- ── Atomic file write ─────────────────────────────────────────────────────

local function atomic_write(path, data)
    local tmp = path .. ".tmp"
    local f = io.open(tmp, "w")
    if not f then return false end
    f:write(data)
    f:close()
    return os.rename(tmp, path)
end

local function write_resp(result)
    atomic_write(RESP_FILE, json_encode(result) .. "\n")
end

-- ── State machine for multi-frame operations ──────────────────────────────

-- mode: "idle" | "waiting" | "pressing_hold" | "pressing_release"
local mode             = "idle"
local frames_left      = 0
local press_rel_left   = 0  -- release frames queued after hold phase
local press_hold_orig  = 0  -- original hold value (for response)
local press_rel_orig   = 0  -- original release value (for response)
local wait_frames_orig = 0  -- original frame count (for response)

-- ── Frame callback ────────────────────────────────────────────────────────

callbacks:add("frame", function()

    -- Tick state machine.
    if mode == "waiting" then
        frames_left = frames_left - 1
        if frames_left <= 0 then
            write_resp({ok=true, frames=wait_frames_orig})
            mode = "idle"
        end
        return
    elseif mode == "pressing_hold" then
        frames_left = frames_left - 1
        if frames_left <= 0 then
            emu:setKeys(0)
            if press_rel_left <= 0 then
                write_resp({ok=true, held=press_hold_orig, released=press_rel_orig})
                mode = "idle"
            else
                mode = "pressing_release"
                frames_left = press_rel_left
            end
        end
        return
    elseif mode == "pressing_release" then
        frames_left = frames_left - 1
        if frames_left <= 0 then
            write_resp({ok=true, held=press_hold_orig, released=press_rel_orig})
            mode = "idle"
        end
        return
    end

    -- Read next command (only when idle).
    local f = io.open(CMD_FILE, "r")
    if not f then return end

    local data = f:read("*a")
    f:close()
    os.remove(CMD_FILE)

    local ok, cmd = pcall(json_decode, data)
    if not ok then
        write_resp({ok=false, error="json parse error"})
        return
    end

    local c = cmd.cmd

    -- screenshot: safe here — we're inside the frame callback.
    if c == "screenshot" then
        local path = cmd.path or "/tmp/mgba_screenshot.png"
        local ok2, err = pcall(function() emu:screenshot(path) end)
        write_resp(ok2 and {ok=true, path=path} or {ok=false, error=tostring(err)})

    -- wait/run: advance N frames without pressing anything.
    elseif c == "wait" or c == "run" then
        local n = cmd.frames or 60
        wait_frames_orig = n
        if n <= 1 then
            write_resp({ok=true, frames=n})
        else
            frames_left = n - 1  -- current frame counts as 1
            mode = "waiting"
        end

    -- press: hold mask for N frames, release for M frames.
    elseif c == "press" then
        local mask    = cmd.mask or 0
        local hold    = cmd.hold or 8
        local release = cmd.release or 8
        press_hold_orig = hold
        press_rel_orig  = release
        emu:setKeys(mask)
        if hold <= 1 then
            emu:setKeys(0)
            if release <= 1 then
                write_resp({ok=true, held=hold, released=release})
            else
                mode = "pressing_release"
                frames_left = release - 1
                press_rel_left = 0
            end
        else
            mode = "pressing_hold"
            frames_left = hold - 1
            press_rel_left = release
        end

    -- keys: set raw key mask for exactly one frame.
    elseif c == "keys" then
        emu:setKeys(cmd.mask or 0)
        write_resp({ok=true})

    -- read_block_exchange: read gMpBlockExchange state for the coop battle relay.
    -- Returns {ok, send_ready, recv_ready, from_idx, data} where data is hex bytes.
    -- Used by the Python relay to shuttle SendBlock data between two instances.
    elseif c == "read_block_exchange" then
        if not ADDR.gMpBlockExchange then
            write_resp({ok=false, error="gMpBlockExchange not mapped"})
            return
        end
        local base = ADDR.gMpBlockExchange
        local send_ready = emu:read8(base + 0)
        local recv_ready = emu:read8(base + 1)
        local from_idx   = emu:read8(base + 2)
        local hex = {}
        for i = 0, 255 do hex[#hex+1] = string.format("%02X", emu:read8(base + 4 + i)) end
        write_resp({ok=true, send_ready=send_ready, recv_ready=recv_ready,
                    from_idx=from_idx, data=table.concat(hex, " ")})

    -- write_block_exchange: deposit a partner block into gMpBlockExchange for pickup.
    -- cmd.from_idx: which gBlockRecvBuffer slot (0 or 1) to fill on the other side.
    -- cmd.data: hex string of bytes to write.  Also clears send_ready.
    elseif c == "write_block_exchange" then
        if not ADDR.gMpBlockExchange then
            write_resp({ok=false, error="gMpBlockExchange not mapped"})
            return
        end
        local base = ADDR.gMpBlockExchange
        local from_idx = cmd.from_idx or 0
        local i = 0
        for hex in (cmd.data or ""):gmatch("%S+") do
            if i < 256 then emu:write8(base + 4 + i, tonumber(hex, 16) or 0) end
            i = i + 1
        end
        emu:write8(base + 2, from_idx)  -- fromPlayerIdx
        emu:write8(base + 1, 1)          -- recvReady = 1
        emu:write8(base + 0, 0)          -- clear sendReady (relay consumed it)
        write_resp({ok=true, written=i})

    -- drain: read all bytes from the send ring (internal relay use only).
    elseif c == "drain" then
        if not ADDR.gMpSendRing then
            write_resp({ok=true, bytes="", count=0})
            return
        end
        local bytes = ring_drain(ADDR.gMpSendRing)
        local hex = {}
        for _, b in ipairs(bytes) do hex[#hex+1] = string.format("%02X", b) end
        write_resp({ok=true, bytes=table.concat(hex, " "), count=#bytes})

    -- inject: push bytes into the recv ring (internal relay use only).
    elseif c == "inject" then
        if not ADDR.gMpRecvRing then
            write_resp({ok=true, pushed=0})
            return
        end
        local bytes = {}
        for hex in (cmd.bytes or ""):gmatch("%S+") do
            bytes[#bytes+1] = tonumber(hex, 16) or 0
        end
        ring_push(ADDR.gMpRecvRing, bytes)
        write_resp({ok=true, pushed=#bytes})

    -- get_text: read dialogue box state and best-effort text from gStringVar4.
    -- box_open     true if text is printing OR script is paused waiting for input
    -- printing     true only while character-by-character rendering is active
    -- waiting      true when script paused (waitmessage / msgbox waiting for A)
    -- text         UTF-8 decoded content of gStringVar4 (ShowFieldMessage target)
    elseif c == "get_text" then
        -- sFirstTextPrinter: non-NULL while a TextPrinter node is in the linked list
        local printing = false
        if ADDR.sFirstTextPrinter then
            printing = (emu:read32(ADDR.sFirstTextPrinter) ~= 0)
        end
        -- sGlobalScriptContextStatus: 0=RUNNING 1=WAITING 2=SHUTDOWN
        -- sGlobalScriptContext.mode (+4 from status): 0=STOPPED 1=BYTECODE 2=NATIVE
        --   NATIVE means script is blocked in a C wait function (waitmessage, waitmovement…)
        local waiting = false
        local in_native = false
        if ADDR.sGlobalScriptContextStatus then
            waiting   = (emu:read8(ADDR.sGlobalScriptContextStatus)     == 1)
            in_native = (emu:read8(ADDR.sGlobalScriptContextStatus + 4) == 2)
        end
        -- Decode gStringVar4 (all field messages are expanded here by ShowFieldMessage)
        local text = ""
        if ADDR.gStringVar4 then
            local chars = {}
            local i = 0
            while i < 256 do
                local b = emu:read8(ADDR.gStringVar4 + i)
                if b == 0xFF then break end          -- EOS
                if b == 0xFD then
                    i = i + 2                        -- skip variable escape + tag byte
                else
                    chars[#chars+1] = CHARSET[b] or "?"
                    i = i + 1
                end
            end
            text = table.concat(chars)
        end
        write_resp({ok=true, box_open=(printing or waiting or in_native),
                    printing=printing, waiting_for_input=waiting, in_native_script=in_native,
                    text=text})

    -- loadstate: load a save state file by path.
    elseif c == "loadstate" then
        local path = cmd.path or ""
        local f = io.open(path, "rb")
        if not f then
            write_resp({ok=false, error="cannot open: " .. path})
        else
            local data = f:read("*a"); f:close()
            local ok2, err = pcall(function() emu:loadStateBuffer(data, 31) end)
            write_resp(ok2 and {ok=true, path=path} or {ok=false, error=tostring(err)})
        end

    -- savestate: save current emulator state to a file.
    elseif c == "savestate" then
        local path = cmd.path or ""
        local buf = emu:saveStateBuffer(31)
        if buf and #buf > 0 then
            local f = io.open(path, "wb")
            if f then
                f:write(buf); f:close()
                write_resp({ok=true, path=path, size=#buf})
            else
                write_resp({ok=false, error="cannot write: " .. path})
            end
        else
            write_resp({ok=false, error="saveStateBuffer returned empty"})
        end

    -- ping: health check.
    elseif c == "ping" then
        write_resp({ok=true, msg="pong"})

    -- read_u16: read a 16-bit value from an absolute address.
    -- cmd.addr (hex string or integer) → resp.value (integer)
    elseif c == "read_u16" then
        local addr = tonumber(cmd.addr) or tonumber(tostring(cmd.addr), 16) or 0
        local v = emu:read16(addr)
        write_resp({ok=true, addr=string.format("0x%08X", addr), value=v})

    -- read_u32: read a 32-bit value from an absolute address.
    elseif c == "read_u32" then
        local addr = tonumber(cmd.addr) or tonumber(tostring(cmd.addr), 16) or 0
        local v = emu:read32(addr)
        write_resp({ok=true, addr=string.format("0x%08X", addr), value=v})

    -- write_u16: write a 16-bit value to an absolute address.
    -- cmd.addr (hex string or integer), cmd.value (integer) → resp.ok
    elseif c == "write_u16" then
        local addr = tonumber(cmd.addr) or tonumber(tostring(cmd.addr), 16) or 0
        local v    = tonumber(cmd.value) or 0
        emu:write16(addr, v)
        write_resp({ok=true, addr=string.format("0x%08X", addr), value=v})

    -- read_player_pos: read player tile X/Y from gObjectEvents[0].
    elseif c == "read_player_pos" then
        local base = 0x020015bc  -- gObjectEvents
        local x = emu:read16(base + 0x10)
        local y = emu:read16(base + 0x12)
        if x >= 0x8000 then x = x - 0x10000 end
        if y >= 0x8000 then y = y - 0x10000 end
        write_resp({ok=true, x=x, y=y})

    -- pick_starter: press A on ball, navigate yesno dialogs, press B for no-nickname.
    -- Waits until VAR_MAP_SCENE==3 (rival took a starter).  ~700 frames max.
    -- cmd.var_addr (hex): address of VAR_MAP_SCENE (gSaveBlock1Ptr + 0x143A)
    -- cmd.target_var (int): expected value after rival picks (3)
    elseif c == "pick_starter" then
        local var_addr = tonumber(cmd.var_addr) or tonumber(tostring(cmd.var_addr), 16) or 0
        local target   = tonumber(cmd.target_var) or 3
        local KEY_A    = 0x001
        local KEY_B    = 0x002
        local SCRIPT_STATUS = 0x03001809
        local FIELD_LOCK    = 0x03001808
        local err_msg  = nil

        local function rf() emu:runFrame() end
        local function rp(key, hold, rel)
            emu:setKeys(key); for _=1,hold do rf() end
            emu:setKeys(0);   for _=1,rel  do rf() end
        end

        -- Step 1: trigger ball (retry A until field lock=1)
        local started = false
        for _=1,90 do
            rp(KEY_A, 3, 12)
            if emu:read8(FIELD_LOCK) == 1 then started=true; break end
        end
        if not started then err_msg = "ball never triggered" end

        -- Step 2: wait for "Do you want X?" yesno (ctx=1)
        if not err_msg then
            local seen = false
            for i=1,600 do
                rf()
                if emu:read8(SCRIPT_STATUS) == 1 then seen=true; break end
                if i%60==0 then rp(KEY_A,1,3) end
            end
            if not seen then err_msg = "want-pokemon yesno never appeared" end
        end
        if not err_msg then
            for _=1,10 do rf() end  -- 5-frame guard
            rp(KEY_A, 2, 5)  -- YES
        end

        -- Step 3: wait for nickname yesno (ctx=1)
        if not err_msg then
            local seen = false
            for i=1,500 do
                rf()
                if emu:read8(SCRIPT_STATUS) == 1 then seen=true; break end
                if i%20==0 then rp(KEY_A,1,3) end
            end
            if not seen then err_msg = "nickname yesno never appeared" end
        end
        if not err_msg then
            for _=1,10 do rf() end  -- guard
            rp(KEY_B, 2, 5)  -- NO to nickname
        end

        -- Step 4: wait for VAR==target (rival takes starter, sets VAR_MAP_SCENE=3)
        local done_ok = false
        if not err_msg then
            for i=1,700 do
                rf()
                if emu:read16(var_addr)==target and emu:read8(FIELD_LOCK)==0 then
                    done_ok=true; break
                end
                if i%20==0 then rp(KEY_A,1,3) end
            end
            if not done_ok then err_msg = "VAR never reached " .. target end
        end

        if err_msg then
            write_resp({ok=false, error=err_msg, var=emu:read16(var_addr)})
        else
            write_resp({ok=true, var=emu:read16(var_addr)})
        end

    -- quit: exit cleanly.
    elseif c == "quit" then
        write_resp({ok=true})
        os.exit(0)

    else
        write_resp({ok=false, error="unknown command: " .. tostring(c)})
    end
end)

-- ── Startup ───────────────────────────────────────────────────────────────

local INIT_SAVESTATE = os.getenv("MGBA_BRIDGE_SAVESTATE")
if INIT_SAVESTATE and INIT_SAVESTATE ~= "" then
    local f = io.open(INIT_SAVESTATE, "rb")
    if f then
        local data = f:read("*a"); f:close()
        pcall(function() emu:loadStateBuffer(data, 31) end)
    end
end

-- Signal readiness — emulator runs automatically after this.
atomic_write(READY_FILE, "ready\n")
