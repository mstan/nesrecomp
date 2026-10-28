#!/usr/bin/env python3
"""Read-only capacity admission for shared NES benchmark hosts; no host locks.

One sample selects a host/core for a batch. Never stops foreign jobs, deletes
foreign leases, waits for a quiet machine, or changes machine-wide settings.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import platform
import shutil
import time


def core_times():
    if os.name != 'nt':
        rows = []
        for line in Path('/proc/stat').read_text().splitlines():
            key, *values = line.split()
            if key.startswith('cpu') and key[3:].isdigit():
                v = list(map(int, values)); rows.append((v[3] + v[4], sum(v[:8])))
        return rows
    class Times(ctypes.Structure):
        _fields_ = [('idle', ctypes.c_int64), ('kernel', ctypes.c_int64), ('user', ctypes.c_int64),
                    ('reserved', ctypes.c_int64 * 2), ('reserved2', ctypes.c_uint32)]
    rows = (Times * os.cpu_count())(); used = ctypes.c_uint32()
    query = ctypes.WinDLL('ntdll').NtQuerySystemInformation
    query.argtypes = [ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32)]
    query.restype = ctypes.c_long
    status = query(8, rows, ctypes.sizeof(rows), ctypes.byref(used))
    if status < 0 or used.value != ctypes.sizeof(rows): raise RuntimeError('Unexpected processor counter shape')
    return [(r.idle, r.kernel + r.user) for r in rows]


def available_memory():
    if os.name != 'nt':
        fields = dict(line.split(':', 1) for line in Path('/proc/meminfo').read_text().splitlines())
        return int(fields['MemAvailable'].split()[0]) * 1024
    class Memory(ctypes.Structure):
        _fields_ = [('length', ctypes.c_uint32), ('load', ctypes.c_uint32)] + [
            (name, ctypes.c_uint64) for name in ('total', 'available', 'page_total', 'page_available',
                                               'virtual_total', 'virtual_available', 'extended')]
    value = Memory(); value.length = ctypes.sizeof(value)
    if not ctypes.WinDLL('kernel32').GlobalMemoryStatusEx(ctypes.byref(value)): raise ctypes.WinError()
    return value.available


def probe(workspace, seconds=2, minimum_free_bytes=1024**3, minimum_memory_bytes=768*1024**2):
    """A light single-game batch needs capacity, not an otherwise idle host.

    The default CPU allowance is half a logical core of aggregate idle time,
    with at least one core below85% busy. Memory/storage are bounded staging
    allowances; callers must raise these for larger builds or asset copies.
    """
    before = core_times(); time.sleep(seconds); after = core_times()
    busy = [max(0.0, min(1.0, 1-(b[0]-a[0])/max(1,b[1]-a[1]))) for a,b in zip(before,after)]
    if not busy: raise RuntimeError('No per-core capacity counters')
    memory = available_memory(); disk = shutil.disk_usage(workspace)
    core = min(range(len(busy)), key=busy.__getitem__)
    reasons = []
    if sum(1-b for b in busy) < .5 or busy[core] > .85: reasons.append('insufficient CPU capacity for one game')
    if memory < minimum_memory_bytes: reasons.append('insufficient available memory')
    if disk.free < minimum_free_bytes: reasons.append('insufficient workspace storage')
    return dict(host=platform.node(),system=platform.system(),machine=platform.machine(),
                logical_processors=os.cpu_count(),core_busy=busy,selected_core=core,
                available_memory_bytes=memory,workspace=str(workspace),free_bytes=disk.free,
                minimum_memory_bytes=minimum_memory_bytes,minimum_free_bytes=minimum_free_bytes,
                sample_seconds=seconds,admitted=not reasons,reasons=reasons,
                policy='capacity-only; shared host allowed; foreign jobs and leases untouched')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, default=Path.cwd())
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    row = probe(args.workspace)
    result = json.dumps(row, indent=2)
    if args.out: args.out.write_text(result+'\n')
    print(result)
    raise SystemExit(0 if row['admitted'] else 2)
