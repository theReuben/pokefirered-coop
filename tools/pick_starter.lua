-- tools/pick_starter.lua
-- Loads oaks_lab.ss1, picks Bulbasaur (confirms, says NO to nickname),
-- waits for VAR_MAP_SCENE to become 3 (rival takes Squirtle),
-- then saves oaks_lab_picked.ss1.
--
-- Run as:
--   /tmp/mgba-build/mgba-headless --script tools/pick_starter.lua pokefirered.gba

local KEY_A = 0x001
local KEY_B = 0x002

local SCRIPT_DIR = debug.getinfo(1, "S").source:match("@(.+/)") or "./"
local REPO_ROOT  = SCRIPT_DIR .. "../"
local STATES_DIR = REPO_ROOT .. "test/lua/states/"

-- Addresses loaded from memory_map.lua so this script stays in sync after rebuilds.
local mm_path = REPO_ROOT .. "test/lua/memory_map.lua"
local mm = assert(loadfile(mm_path), "[pick] Cannot load " .. mm_path)()

local ADDR_SCRIPT_STATUS = mm.sGlobalScriptContextStatus
local ADDR_FIELD_LOCK    = mm.sLockFieldControls

-- VarGet disassembly: ptr + (id - 0x3638) * 2  (constant 0xffffc9c8 = -0x3638).
-- For VAR_MAP_SCENE_PALLET_TOWN_PROFESSOR_OAKS_LAB = 0x4055:
--   offset = (0x4055 - 0x3638) * 2 = 0xA1D * 2 = 0x143A
-- Must dereference gSaveBlock1Ptr at runtime — the save system loads SaveBlock1 at a
-- different EWRAM address than the static symbol gSaveblock1.
local VAR_LAB_OFFSET = 0x143A

local function readVar()
    local ptr = emu:read32(mm.gSaveBlock1Ptr)
    if ptr == 0 then return 0 end
    return emu:read16(ptr + VAR_LAB_OFFSET)
end

local total_frames = 0
local function rf()
    emu:runFrame()
    total_frames = total_frames + 1
end

local function idle(n)
    emu:setKeys(0)
    for _ = 1, n do rf() end
end

local function press(key, hold, rel)
    hold = hold or 3; rel = rel or 5
    emu:setKeys(key)
    for _ = 1, hold do rf() end
    emu:setKeys(0)
    for _ = 1, rel do rf() end
end

-- Load save state
local ss1_path = STATES_DIR .. "oaks_lab.ss1"
print("[pick] Loading " .. ss1_path)
local f = io.open(ss1_path, "rb")
if not f then error("[pick] Cannot open " .. ss1_path) end
local data = f:read("*a"); f:close()
emu:loadStateBuffer(data, 31)

idle(120)
print(string.format("[pick] Post-load VAR_LAB=%d", readVar()))

-- Set text speed to FAST (bit field at gSaveBlock2Ptr+0x14, bits[2:0])
do
    local sb2 = emu:read32(0x03005304)
    if sb2 ~= 0 then
        local opt = emu:read16(sb2 + 0x14)
        emu:write16(sb2 + 0x14, (opt & 0xFFF8) | 2)
        print("[pick] Text speed set to FAST")
    end
end

-- Step 1: trigger the Bulbasaur ball (retry A until FIELD_LOCK=1)
print("[pick] Step 1: trigger Bulbasaur ball...")
local started = false
for try = 1, 90 do
    press(KEY_A, 3, 12)
    if emu:read8(ADDR_FIELD_LOCK) == 1 then
        print(string.format("[pick] Ball triggered at try=%d", try))
        started = true
        break
    end
end
if not started then error("[pick] Ball never triggered") end

-- Step 2: wait for "Do you want BULBASAUR?" YESNO (ctx=1)
print("[pick] Step 2: waiting for want-pokemon yesno...")
local seen = false
for i = 1, 600 do
    rf()
    if emu:read8(ADDR_SCRIPT_STATUS) == 1 then
        print(string.format("[pick] Yesno appeared at frame %d", i))
        seen = true
        break
    end
    if i % 60 == 0 then press(KEY_A, 1, 3) end
end
if not seen then error("[pick] Want-pokemon yesno never appeared") end

idle(10)            -- 5-frame guard
press(KEY_A, 2, 5)  -- YES

-- Step 3: wait for nickname YESNO (ctx=1)
print("[pick] Step 3: waiting for nickname yesno...")
seen = false
for i = 1, 500 do
    rf()
    if emu:read8(ADDR_SCRIPT_STATUS) == 1 then
        print(string.format("[pick] Nickname yesno at frame %d", i))
        seen = true
        break
    end
    if i % 20 == 0 then press(KEY_A, 1, 3) end
end
if not seen then error("[pick] Nickname yesno never appeared") end

idle(10)
press(KEY_B, 2, 5)  -- NO to nickname

-- Step 4: wait for VAR_LAB=3 (rival takes starter)
print("[pick] Step 4: waiting for VAR_LAB=3...")
local done = false
for i = 1, 700 do
    rf()
    if readVar() == 3 and emu:read8(ADDR_FIELD_LOCK) == 0 then
        print(string.format("[pick] VAR_LAB=3 at frame %d total=%d", i, total_frames))
        done = true
        break
    end
    if i % 20 == 0 then press(KEY_A, 1, 3) end
end
if not done then
    error(string.format("[pick] VAR_LAB never reached 3 (stuck at %d)", readVar()))
end

idle(30)

-- Save state
local out_path = STATES_DIR .. "oaks_lab_picked.ss1"
print("[pick] Saving " .. out_path)
local buf = emu:saveStateBuffer(31)
assert(buf and #buf > 0, "saveStateBuffer empty")
local fout = assert(io.open(out_path, "wb"))
fout:write(buf); fout:close()
print(string.format("[pick] Saved %d bytes  VAR_LAB=%d  total_frames=%d",
      #buf, readVar(), total_frames))

if emu.quit then emu:quit() else os.exit(0) end
