-- test/lua/coop/_driver.lua
-- Common driver loop for two-instance scenarios. Each scenario's p1.lua /
-- p2.lua loads this module, configures itself, then calls D.run(scenarioFn).
--
-- Responsibilities:
--   * Bridge tick: drain gMpSendRing → outbox.bin; consume inbox.bin →
--     gMpRecvRing. The Python orchestrator copies bytes between the two
--     instances' files at ~20Hz.
--   * Scenario tick: invoke the scenario coroutine each frame so it can
--     drive inputs, wait for state, and assert.
--   * Result reporting: scenario calls D.pass()/D.fail(reason) which
--     writes the RESULT line to $LUA_TEST_RESULTS and quits.
--
-- Path conventions:
--   $COOP_INSTANCE_DIR/inbox.bin   bytes pushed by orchestrator (consume + truncate)
--   $COOP_INSTANCE_DIR/outbox.bin  bytes drained from gMpSendRing (append)
--   $COOP_INSTANCE_DIR/result.txt  written via H.finish()

local H  = require("_harness")
local mm = require("memory_map")

local D = {}

D.H  = H
D.mm = mm

local function instDir()
    return os.getenv("COOP_INSTANCE_DIR") or "/tmp/coop_inst"
end

local function instId()
    return os.getenv("COOP_INSTANCE_ID") or "p?"
end

D.inboxPath  = instDir() .. "/inbox.bin"
D.outboxPath = instDir() .. "/outbox.bin"

-- ------------------------------------------------------------------ --
-- Bridge tick: file-IPC between the two ROM instances.
-- ------------------------------------------------------------------ --

local function readBinary(path)
    local f = io.open(path, "rb")
    if not f then return nil end
    local data = f:read("*all")
    f:close()
    return data
end

local function appendBinary(path, data)
    local f = io.open(path, "ab")
    if not f then return end
    f:write(data)
    f:close()
end

local function truncate(path)
    local f = io.open(path, "wb")
    if f then f:close() end
end

-- Push every byte from inbox.bin into gMpRecvRing, then truncate the file.
-- The orchestrator atomically writes-then-appends so we drop nothing.
function D.pumpInbox()
    local data = readBinary(D.inboxPath)
    if not data or #data == 0 then return end
    truncate(D.inboxPath)
    for i = 1, #data do
        local byte = string.byte(data, i)
        H.recvPush(mm.gMpRecvRing, byte)
    end
end

-- Drain everything in gMpSendRing into outbox.bin (append).
function D.flushOutbox()
    local bytes = H.sendDrain(mm.gMpSendRing)
    if #bytes == 0 then return end
    local buf = {}
    for i, b in ipairs(bytes) do buf[i] = string.char(b & 0xFF) end
    appendBinary(D.outboxPath, table.concat(buf))
end

-- ------------------------------------------------------------------ --
-- Result reporting.
-- ------------------------------------------------------------------ --

function D.pass(name)
    H.check(true, "scenario completed")
    H.finish(name or instId())
end

function D.fail(name, reason)
    H.check(false, reason or "(no reason given)")
    H.finish(name or instId())
end

-- ------------------------------------------------------------------ --
-- Main loop.
--
-- scenarioFn receives D and runs as a coroutine. It can call:
--   D.step(n)          run n frames, bridging each one
--   D.waitFor(pred, t) run frames until pred() returns true or timeout
--   D.assert(cond, m)  H.check + immediate D.fail on failure
--   D.pass(name)       declare success and exit
--
-- The driver guarantees one bridge tick per emulated frame so packets
-- never queue up faster than the dispatcher can consume them.
-- ------------------------------------------------------------------ --

local sCo = nil
local sName = "scenario"
local sFinished = false

function D.step(n)
    n = n or 1
    for _ = 1, n do
        if sFinished then return end
        D.pumpInbox()
        H.runFrames(1)
        D.flushOutbox()
    end
end

function D.waitFor(pred, maxFrames, label)
    maxFrames = maxFrames or 600
    for _ = 1, maxFrames do
        if pred() then return true end
        D.step(1)
    end
    D.fail(sName, "timeout waiting for " .. (label or "predicate"))
    sFinished = true
    return false
end

function D.assertCond(cond, msg)
    if not cond then
        D.fail(sName, msg or "assertion failed")
        sFinished = true
    end
end

function D.run(name, scenarioFn)
    sName = name or "scenario"
    -- Block until Multiplayer_Init populates the ring magic bytes.
    if not H.waitForInit(mm) then
        D.fail(sName, "Multiplayer_Init never ran")
        return
    end

    sCo = coroutine.create(function() scenarioFn(D) end)
    local ok, err = coroutine.resume(sCo)
    if not ok then
        D.fail(sName, "scenario crashed: " .. tostring(err))
        return
    end

    if not sFinished then
        -- Scenario returned without calling D.pass — treat as success.
        D.pass(sName)
    end
end

return D
