#!/usr/bin/env python3
"""
mesen_mcp_server.py — MCP server for Mesen2 bidirectional control.

Mesen2 must be running with mesen_mcp.lua loaded (Allow I/O access checked).
Lua writes state to C:/temp/mesen_*.json each frame and reads commands from
C:/temp/mesen_cmd.json, writing responses to C:/temp/mesen_resp.json.

Add to .mcp.json:
  "mesen": { "type": "stdio", "command": "python", "args": ["F:/Projects/nesrecomp/tools/mesen_mcp_server.py"] }

Requires: pip install mcp
"""

import sys, json, os, time
from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp import types

STATE_FILE = "C:/temp/mesen_state.json"
TRACE_FILE = "C:/temp/mesen_trace.json"
EXEC_FILE  = "C:/temp/mesen_exec.json"
CMD_FILE   = "C:/temp/mesen_cmd.json"
RESP_FILE  = "C:/temp/mesen_resp.json"

def read_json(path):
    try:
        with open(path, "r") as f:
            return json.loads(f.read())
    except FileNotFoundError:
        return {"error": f"File not found: {path}. Is Mesen2 running with mesen_mcp.lua loaded?"}
    except Exception as e:
        return {"error": str(e)}

def send_command(cmd_dict, timeout=3.0):
    """Write a command to CMD_FILE and wait for response in RESP_FILE."""
    # Remove stale response
    try: os.remove(RESP_FILE)
    except FileNotFoundError: pass

    with open(CMD_FILE, "w") as f:
        f.write(json.dumps(cmd_dict))

    # Poll for response (Lua processes once per frame ~16ms)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(RESP_FILE):
            try:
                with open(RESP_FILE, "r") as f:
                    return json.loads(f.read())
            except Exception:
                pass
        time.sleep(0.02)
    return {"error": "Timeout waiting for Mesen2 response. Is the game running (not paused)?"}

app = Server("mesen2")

@app.list_tools()
async def list_tools():
    return [
        types.Tool(
            name="mesen_cpu",
            description="Get NES CPU registers (PC, SP, A, X, Y, PS) and key RAM bytes from Mesen2.",
            inputSchema={"type": "object", "properties": {}}
        ),
        types.Tool(
            name="mesen_trace",
            description="Get the last 500 per-frame snapshots (PC, SP, A, X, Y, key RAM) from Mesen2.",
            inputSchema={"type": "object", "properties": {}}
        ),
        types.Tool(
            name="mesen_exec",
            description="Get execution hit log for a specific address (or all watched addresses if addr omitted). addr is decimal.",
            inputSchema={
                "type": "object",
                "properties": {
                    "addr": {"type": "integer", "description": "Address to query (decimal). Omit for all."}
                }
            }
        ),
        types.Tool(
            name="mesen_clear",
            description="Clear all trace and exec-hit buffers in Mesen2.",
            inputSchema={"type": "object", "properties": {}}
        ),
        types.Tool(
            name="mesen_ram",
            description="Read bytes from the last Mesen2 RAM snapshot. addr and len are decimal. Covers $0000-$01FF.",
            inputSchema={
                "type": "object",
                "properties": {
                    "addr": {"type": "integer", "description": "Start address (decimal)"},
                    "len":  {"type": "integer", "description": "Number of bytes"}
                },
                "required": ["addr"]
            }
        ),
        types.Tool(
            name="mesen_state_keys",
            description="List keys available in the current Mesen2 CPU state snapshot.",
            inputSchema={"type": "object", "properties": {}}
        ),
        types.Tool(
            name="mesen_send_cmd",
            description=(
                "Send a command to Mesen2 and wait for response. "
                "Commands: "
                "'load_state' (slot: int 0-9) — load a Mesen2 save state slot; "
                "'input' (buttons: int bitmask A=1 B=2 Sel=4 Start=8 Up=16 Down=32 Left=64 Right=128, frames: int) — inject controller input; "
                "'snap' — return current write-captured RAM snapshot immediately; "
                "'clear' — clear trace buffers. "
                "Returns response JSON from Lua."
            ),
            inputSchema={
                "type": "object",
                "properties": {
                    "cmd":     {"type": "string",  "description": "Command name: load_state | input | snap | clear"},
                    "slot":    {"type": "integer", "description": "Save state slot (0-9) for load_state"},
                    "buttons": {"type": "integer", "description": "Button bitmask for input command"},
                    "frames":  {"type": "integer", "description": "Number of frames to hold input"},
                    "timeout": {"type": "number",  "description": "Seconds to wait for response (default 3)"}
                },
                "required": ["cmd"]
            }
        ),
    ]

@app.call_tool()
async def call_tool(name: str, arguments: dict):
    if name == "mesen_cpu":
        result = read_json(STATE_FILE)

    elif name == "mesen_trace":
        result = read_json(TRACE_FILE)

    elif name == "mesen_exec":
        data = read_json(EXEC_FILE)
        addr = arguments.get("addr")
        if addr is not None and isinstance(data, dict):
            key = str(addr)
            result = data.get(key, {"error": f"No hits for addr {addr}."})
        else:
            result = data

    elif name == "mesen_clear":
        result = send_command({"cmd": "clear"})

    elif name == "mesen_ram":
        state = read_json(STATE_FILE)
        if "error" in state:
            result = state
        else:
            result = {"note": "Arbitrary RAM reads use write-capture: request a 'snap' via mesen_send_cmd to get current write-captured values."}

    elif name == "mesen_state_keys":
        state = read_json(STATE_FILE)
        if "error" in state:
            result = state
        else:
            result = {"keys": sorted(state.keys())}

    elif name == "mesen_send_cmd":
        cmd = arguments.get("cmd", "")
        timeout = arguments.get("timeout", 3.0)
        payload = {"cmd": cmd}
        if "slot"    in arguments: payload["slot"]    = arguments["slot"]
        if "buttons" in arguments: payload["buttons"] = arguments["buttons"]
        if "frames"  in arguments: payload["frames"]  = arguments["frames"]
        result = send_command(payload, timeout=timeout)

    else:
        result = {"error": f"Unknown tool: {name}"}

    return [types.TextContent(type="text", text=json.dumps(result, indent=2))]

async def main():
    async with stdio_server() as streams:
        await app.run(streams[0], streams[1], app.create_initialization_options())

if __name__ == "__main__":
    import asyncio
    asyncio.run(main())
