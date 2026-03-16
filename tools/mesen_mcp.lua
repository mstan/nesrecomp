-- mesen_mcp.lua: Game-agnostic Mesen2 ↔ MCP bridge via file-based IPC.
-- Uses WRITE CALLBACKS for reliable RAM capture.
-- Load in Mesen2 Script Window with "Allow I/O access" checked.
--
-- Pairs with mesen_mcp_server.py (the MCP server reads/writes the same files).

local frame_trace = {}
local MAX_FRAME   = 500
local wv = {}          -- last-write capture: wv[addr] = val
local current_frame = 0

-- Boot trace: captures the FIRST 600 frames after script load, then freezes.
local boot_trace  = {}
local BOOT_MAX    = 600
local BOOT_FILE   = "C:/temp/mesen_boot.json"
local boot_frozen = false

-- ── Write callbacks ────────────────────────────────────────────────────────

local function on_write(addr, val)
    wv[addr] = val
end

local function watch(addr)
    pcall(function()
        emu.addMemoryCallback(on_write, emu.callbackType.write, addr, addr)
    end)
end

-- Universal NES addresses worth watching in any game
for _, addr in ipairs({
    0x0774,   -- common: nodisplay counter
    0x0779,   -- common: PPUMASK shadow
}) do watch(addr) end

-- ── Minimal JSON encoder ───────────────────────────────────────────────────
local function jstr(s)
    return '"' .. tostring(s):gsub('\\','\\\\'):gsub('"','\\"'):gsub('\n','\\n') .. '"'
end
local function jval(v)
    local t = type(v)
    if t == "number"  then return tostring(v)
    elseif t == "boolean" then return v and "true" or "false"
    elseif t == "string"  then return jstr(v)
    elseif t == "table" then
        if #v > 0 then
            local p = {}
            for _, i in ipairs(v) do p[#p+1] = jval(i) end
            return "[" .. table.concat(p, ",") .. "]"
        else
            local p = {}
            for k, i in pairs(v) do p[#p+1] = jstr(tostring(k)) .. ":" .. jval(i) end
            return "{" .. table.concat(p, ",") .. "}"
        end
    end
    return "null"
end

local function write_file(path, content)
    local f = io.open(path, "w")
    if f then f:write(content) f:close() end
end
local function read_file(path)
    local f = io.open(path, "r")
    if not f then return nil end
    local s = f:read("*a")
    f:close()
    return s
end

local STATE_FILE = "C:/temp/mesen_state.json"
local TRACE_FILE = "C:/temp/mesen_trace.json"
local EXEC_FILE  = "C:/temp/mesen_exec.json"
local CMD_FILE   = "C:/temp/mesen_cmd.json"
local RESP_FILE  = "C:/temp/mesen_resp.json"

-- ── Input injection ─────────────────────────────────────────────────────────
local pending_input = nil   -- {buttons=N, frames=N}

local inject_call_count = 0
local function inject_input(buttons)
    inject_call_count = inject_call_count + 1
    local ok, err = pcall(function()
        local state = emu.getInput(0)
        state.a      = (buttons & 0x01) ~= 0
        state.b      = (buttons & 0x02) ~= 0
        state.select = (buttons & 0x04) ~= 0
        state.start  = (buttons & 0x08) ~= 0
        state.up     = (buttons & 0x10) ~= 0
        state.down   = (buttons & 0x20) ~= 0
        state.left   = (buttons & 0x40) ~= 0
        state.right  = (buttons & 0x80) ~= 0
        emu.setInput(state)
    end)
    wv.last_inject_buttons = buttons
    wv.last_inject_calls   = inject_call_count
    wv.last_inject_ok0t    = ok and 1 or 0
    wv.last_inject_err     = err and tostring(err) or "ok"
end

local function do_inject_frame()
    if pending_input and pending_input.frames > 0 then
        inject_input(pending_input.buttons)
        pending_input.frames = pending_input.frames - 1
        if pending_input.frames <= 0 then pending_input = nil end
    end
end

local input_hooked = false
for _, evtype in ipairs({"inputPolled", "startFrame", "endFrame"}) do
    if not input_hooked then
        pcall(function()
            local ev = emu.eventType[evtype]
            if ev then
                emu.addEventCallback(do_inject_frame, ev)
                input_hooked = true
                emu.log("[mesen_mcp] input hooked on " .. evtype)
            end
        end)
    end
end

-- ── Dynamic watch command ───────────────────────────────────────────────────
-- "watch" command: add write callbacks for a list of addresses at runtime.
-- This lets game-specific MCP tools register addresses without editing Lua.

local function handle_watch(cmd_txt)
    local addrs_str = cmd_txt:match('"addrs"%s*:%s*%[(.-)%]')
    if not addrs_str then return '{"ok":false,"error":"missing addrs array"}' end
    local count = 0
    for a in addrs_str:gmatch("(%d+)") do
        local addr = tonumber(a)
        if addr then watch(addr); count = count + 1 end
    end
    return '{"ok":true,"cmd":"watch","count":' .. count .. '}'
end

-- ── RAM read command ────────────────────────────────────────────────────────
-- "read_ram" command: read a range of RAM bytes directly via emu.read().
-- Falls back to write-capture if emu.read is unavailable.

local function handle_read_ram(cmd_txt)
    local addr  = tonumber(cmd_txt:match('"addr"%s*:%s*(%d+)')) or 0
    local len   = tonumber(cmd_txt:match('"len"%s*:%s*(%d+)'))  or 1
    local bytes = {}
    local ok, _ = pcall(function()
        for i = 0, len - 1 do
            bytes[#bytes+1] = emu.read(addr + i, emu.memType.nesMemory)
        end
    end)
    if ok then
        return '{"ok":true,"cmd":"read_ram","addr":' .. addr .. ',"data":[' .. table.concat(bytes, ",") .. ']}'
    else
        -- Fallback: return write-captured values
        local vals = {}
        for i = 0, len - 1 do
            vals[#vals+1] = wv[addr + i] or -1
        end
        return '{"ok":true,"cmd":"read_ram","addr":' .. addr .. ',"source":"write_capture","data":[' .. table.concat(vals, ",") .. ']}'
    end
end

-- ── Per-frame snapshot ──────────────────────────────────────────────────────
local tick = 0
local exec_hits = {}

emu.addEventCallback(function()
    local s = emu.getState()
    current_frame = s["frameCount"] or s["ppu.frameCount"] or 0

    local snap = {
        pc    = s["cpu.pc"]  or 0,
        sp    = s["cpu.sp"]  or 0,
        a     = s["cpu.a"]   or 0,
        x     = s["cpu.x"]   or 0,
        y     = s["cpu.y"]   or 0,
        ps    = s["cpu.ps"]  or 0,
        frame = current_frame,
        -- Include all write-captured values
        wv    = wv,
        -- Input injection diagnostics
        inject_calls = wv.last_inject_calls   or 0,
        inject_btns  = wv.last_inject_buttons or 0,
        inject_ok0t  = wv.last_inject_ok0t    or 0,
        inject_err   = wv.last_inject_err     or "none",
    }

    frame_trace[#frame_trace+1] = snap
    if #frame_trace > MAX_FRAME then table.remove(frame_trace, 1) end

    -- Boot trace: freeze after BOOT_MAX frames
    if not boot_frozen then
        boot_trace[#boot_trace+1] = snap
        if #boot_trace >= BOOT_MAX then
            boot_frozen = true
            write_file(BOOT_FILE, jval(boot_trace))
            emu.log("[mesen_mcp] boot trace frozen at frame " .. tostring(current_frame))
        end
    end

    tick = tick + 1
    write_file(STATE_FILE, jval(snap))
    if tick % 30 == 0 then
        write_file(TRACE_FILE, jval(frame_trace))
        write_file(EXEC_FILE,  jval(exec_hits))
    end

    -- ── Command handler ─────────────────────────────────────────────────────
    local cmd_txt = read_file(CMD_FILE)
    if cmd_txt then
        os.remove(CMD_FILE)

        if cmd_txt:find('"clear"') then
            frame_trace = {}; exec_hits = {}
            boot_trace = {}; boot_frozen = false
            write_file(TRACE_FILE, "[]"); write_file(EXEC_FILE, "{}")
            write_file(BOOT_FILE,  "[]")
            write_file(RESP_FILE, '{"ok":true,"cmd":"clear"}')

        elseif cmd_txt:find('"load_state"') then
            local slot = tonumber(cmd_txt:match('"slot"%s*:%s*(%d+)')) or 0
            local ok, err = pcall(function() emu.loadSaveState(slot) end)
            if ok then
                write_file(RESP_FILE, '{"ok":true,"cmd":"load_state","slot":' .. slot .. '}')
            else
                write_file(RESP_FILE, '{"ok":false,"error":' .. jstr(tostring(err)) .. '}')
            end

        elseif cmd_txt:find('"input"') then
            local buttons = tonumber(cmd_txt:match('"buttons"%s*:%s*(%d+)')) or 0
            local frames  = tonumber(cmd_txt:match('"frames"%s*:%s*(%d+)'))  or 1
            pending_input = { buttons = buttons, frames = frames }
            inject_input(buttons)
            write_file(RESP_FILE, '{"ok":true,"cmd":"input","buttons":' .. buttons .. ',"frames":' .. frames .. '}')

        elseif cmd_txt:find('"watch"') then
            write_file(RESP_FILE, handle_watch(cmd_txt))

        elseif cmd_txt:find('"read_ram"') then
            write_file(RESP_FILE, handle_read_ram(cmd_txt))

        elseif cmd_txt:find('"snap"') then
            write_file(RESP_FILE, jval(snap))
        end
    end
end, emu.eventType.endFrame)

emu.log("[mesen_mcp] ready. Generic NES bridge — use 'watch' command to add game-specific addresses.")
