-- mesen_mcp.lua: File-based IPC for MCP tool access
-- Uses WRITE CALLBACKS for reliable RAM capture (emu.read broken in this build).
-- Tracks all OAM tile writes to find the projectile slot.
-- Load in Mesen2 Script Window with "Allow I/O access" checked.

local frame_trace = {}
local MAX_FRAME   = 500
local wv = {}          -- last-write capture: wv[addr] = val
local current_frame = 0

-- ── Write callbacks ──────────────────────────────────────────────────────────

-- General single-address write capture
local function on_write(addr, val)
    wv[addr] = val
end

local function watch(addr)
    pcall(function()
        emu.addMemoryCallback(on_write, emu.callbackType.write, addr, addr)
    end)
end

for _, addr in ipairs({
    0x000B, 0x0013, 0x001A, 0x001F, 0x0020,
    0x0026, 0x005A, 0x005B, 0x00B5, 0x00B6, 0x0120,
    -- OAM slots 5 and 37 (our recompiler's known projectile slots)
    0x0714, 0x0715, 0x0716, 0x0717,
    0x0794, 0x0795, 0x0796, 0x0797,
}) do watch(addr) end

-- Watch ALL OAM tile bytes ($0701,$0705,...,$07FD) for tile=0x39
-- Also capture their matching attr bytes. Track first hit per reload.
local proj39_seen = false
pcall(function()
    emu.addMemoryCallback(function(addr, val)
        local off  = addr - 0x0700
        local slot = math.floor(off / 4)
        local typ  = off % 4
        if typ == 1 then   -- tile byte
            wv['oam_t' .. slot] = val
            if val == 0x39 and not proj39_seen then
                proj39_seen      = true
                wv.proj39_slot   = slot
                wv.proj39_frame  = current_frame
                -- attr is next write; cache slot for attr lookup
                wv.proj39_attr   = wv['oam_a' .. slot] or 0
            end
        elseif typ == 2 then  -- attr byte
            wv['oam_a' .. slot] = val
            -- If we just saw this slot's tile=0x39, update attr
            if wv.proj39_slot == slot then
                wv.proj39_attr = val
            end
        end
    end, emu.callbackType.write, 0x0700, 0x07FF)
end)

-- ── Minimal JSON encoder ─────────────────────────────────────────────────────
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

-- ── Input injection ───────────────────────────────────────────────────────────
local pending_input = nil   -- {buttons=N, frames=N}

-- Try multiple event types for input injection (API varies between Mesen2 builds)
local inject_call_count = 0
local function inject_input(buttons)
    inject_call_count = inject_call_count + 1
    -- Get current input state object from Mesen and modify it
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

-- Inject on inputPolled (fires when game reads controller) with startFrame fallback
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

-- ── Per-frame snapshot ────────────────────────────────────────────────────────
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
        -- Write-captured RAM values
        r0B      = wv[0x000B] or 0,
        r13      = wv[0x0013] or 0,
        r1A      = wv[0x001A] or 0,
        r1F      = wv[0x001F] or 0,
        r20      = wv[0x0020] or 0,
        r26      = wv[0x0026] or 0,
        r5A      = wv[0x005A] or 0,
        r5B      = wv[0x005B] or 0,
        rB5      = wv[0x00B5] or 0,
        rB6      = wv[0x00B6] or 0,
        r120     = wv[0x0120] or 0,
        oam5_y   = wv[0x0714] or 0,
        oam5_t   = wv[0x0715] or 0,
        oam5_a   = wv[0x0716] or 0,
        oam5_x   = wv[0x0717] or 0,
        oam37_y  = wv[0x0794] or 0,
        oam37_t  = wv[0x0795] or 0,
        oam37_a  = wv[0x0796] or 0,
        oam37_x  = wv[0x0797] or 0,
        -- Projectile tile=0x39 tracker (any OAM slot)
        proj39_slot  = wv.proj39_slot  or -1,
        proj39_frame = wv.proj39_frame or -1,
        proj39_attr  = wv.proj39_attr  or -1,
        -- Input injection diagnostics
        inject_calls = wv.last_inject_calls   or 0,
        inject_btns  = wv.last_inject_buttons or 0,
        inject_ok0t  = wv.last_inject_ok0t    or 0,
        inject_err   = wv.last_inject_err     or "none",
    }

    frame_trace[#frame_trace+1] = snap
    if #frame_trace > MAX_FRAME then table.remove(frame_trace, 1) end

    tick = tick + 1
    write_file(STATE_FILE, jval(snap))
    if tick % 30 == 0 then
        write_file(TRACE_FILE, jval(frame_trace))
        write_file(EXEC_FILE,  jval(exec_hits))
    end

    -- ── Command handler ───────────────────────────────────────────────────────
    local cmd_txt = read_file(CMD_FILE)
    if cmd_txt then
        os.remove(CMD_FILE)

        if cmd_txt:find('"clear"') then
            frame_trace = {}; exec_hits = {}
            proj39_seen = false; wv.proj39_slot = nil
            write_file(TRACE_FILE, "[]"); write_file(EXEC_FILE, "{}")
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
            -- Also inject immediately for this frame
            inject_input(buttons)
            write_file(RESP_FILE, '{"ok":true,"cmd":"input","buttons":' .. buttons .. ',"frames":' .. frames .. '}')

        elseif cmd_txt:find('"snap"') then
            -- Reset proj39 tracker so next projectile fire is captured fresh
            proj39_seen = false
            write_file(RESP_FILE, jval(snap))
        end
    end
end, emu.eventType.endFrame)

emu.log("[mesen_mcp] v2 ready. Write callbacks active. OAM full-range watcher active.")

-- Dump available emu API to file for inspection
local api_keys = {}
for k, v in pairs(emu) do api_keys[#api_keys+1] = k .. "=" .. type(v) end
table.sort(api_keys)
write_file("C:/temp/mesen_emu_api.txt", table.concat(api_keys, "\n"))
