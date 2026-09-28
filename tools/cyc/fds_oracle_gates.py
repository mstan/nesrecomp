#!/usr/bin/env python3
"""FDS gates against nesref (Mesen 0.9.9): a local check on owner images, not CI.

Runs the cycle runtime (cyc_interp or a compiled FDS program) and nesref on the
same BIOS and disk and compares, frame by frame, what both machines hold:

  cpu    the 2 KiB of CPU RAM          (nesref: per-frame RAM trace)
  prg    the 32 KiB of PRG RAM         (nesref: savestate, BaseMapper work RAM)
  nt     the 2 KiB of nametable RAM    (nesref: savestate, BaseMapper nametables)
  chr    the 8 KiB of CHR RAM          (nesref: savestate, BaseMapper CHR RAM)
  pic    the picture: cyc color indices through Mesen's libretro palette
         (Libretro/libretro.cpp:84 defaultPalette) against nesref's PNG

nesref exposes only CPU RAM through libretro, so every other memory comes from
one NESREF_STATEDUMP per frame, one process per frame (a 751-frame boot is 751
short runs; --jobs runs them in parallel). The Mesen 0.9.9 state blob is
parsed by content, not by a fixed offset: BaseMapper::StreamState
(BaseMapper.cpp:495-512) writes CHR RAM [u32 8192][...] then work RAM
[u32 32768][...], save RAM [u32 0], nametables [u32 2048][...]; the FDS
fields (FDS.cpp:489-494) are the next block of 314 bytes.

Frame pairing: nesref frame f is the state after f retro_runs; cyc's frame log
record k is the state after k + 1 frames. Mesen ends a frame at the first
instruction boundary after scanline 240 dot 0, cyc at the one after VBlank
(scanline 241 dot 1), one scanline (~114 CPU cycles) later; by default the
cyc snapshot is taken at Mesen's point (--frame-log-at mesen), so a frame that
ends mid-loop compares like with like. --offset shifts the pairing if the two
machines number frames differently (measured and printed).

  python tools/cyc/fds_oracle_gates.py --gate nodisk  --cyc build/.../cyc_interp.exe \\
      --nesref F:/Projects/nesref_wt-fds/nesref.exe --image smb2j.fds --bios disksys.rom --out out/
  python tools/cyc/fds_oracle_gates.py --gate boot ...     # SMB2J, disk in at power-on
  python tools/cyc/fds_oracle_gates.py --gate otocky --image otocky.fds ...
  python tools/cyc/fds_oracle_gates.py --gate input --input route.txt --frames 3200:3950 ...
      # any cyc --input route (buttons and DISK_ lines), disk in at power-on
  python tools/cyc/fds_oracle_gates.py --gate insert ...  # no disk at power-on: cyc's auto
      # insert (the default) against nesref's no-disk boot and a scripted DISK_INSERT at the same frame

The nodisk gate turns cyc's auto insert off (--fds-hle no-auto-insert), so the
drive stays empty in both machines.
"""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import zlib

# Mesen 0.9.9 Libretro/libretro.cpp:84, the "Default" palette.
MESEN_PALETTE = [
    0x666666, 0x002A88, 0x1412A7, 0x3B00A4, 0x5C007E, 0x6E0040, 0x6C0600, 0x561D00, 0x333500, 0x0B4800,
    0x005200, 0x004F08, 0x00404D, 0x000000, 0x000000, 0x000000, 0xADADAD, 0x155FD9, 0x4240FF, 0x7527FE,
    0xA01ACC, 0xB71E7B, 0xB53120, 0x994E00, 0x6B6D00, 0x388700, 0x0C9300, 0x008F32, 0x007C8D, 0x000000,
    0x000000, 0x000000, 0xFFFEFF, 0x64B0FF, 0x9290FF, 0xC676FF, 0xF36AFF, 0xFE6ECC, 0xFE8170, 0xEA9E22,
    0xBCBE00, 0x88D800, 0x5CE430, 0x45E082, 0x48CDDE, 0x4F4F4F, 0x000000, 0x000000, 0xFFFEFF, 0xC0DFFF,
    0xD3D2FF, 0xE8C8FF, 0xFBC2FF, 0xFEC4EA, 0xFECCC5, 0xF7D8A5, 0xE4E594, 0xCFEF96, 0xBDF4AB, 0xB3F3CC,
    0xB5EBF2, 0xB8B8B8, 0x000000, 0x000000,
]

NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


# ---------------------------------------------------------------- PNG reading
def read_png_rgb(path):
    try:
        from PIL import Image
        with Image.open(path) as im:
            raw = im.convert('RGB').tobytes()
        return [(raw[i] << 16) | (raw[i + 1] << 8) | raw[i + 2] for i in range(0, len(raw), 3)]
    except ImportError:
        pass
    data = Path(path).read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', path
    pos, idat, width = 8, b'', 0
    while pos < len(data):
        n, kind = struct.unpack_from('>I4s', data, pos)
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            width, height, depth, ctype = struct.unpack_from('>IIBB', body)
            assert depth == 8 and ctype in (2, 6), (path, depth, ctype)
            bpp = 3 if ctype == 2 else 4
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = width * bpp
    rows, prev = [], bytearray(stride)
    for y in range(height):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return [(r[x * bpp] << 16) | (r[x * bpp + 1] << 8) | r[x * bpp + 2] for r in rows for x in range(width)]


# ---------------------------------------------------------------- nesref side
def nesref_script(route, frames):
    """A cyc --input file as nesref script lines that run `frames` frames.

    cyc applies a line `F ...` before frame F; nesref applies a command at the
    frame boundary before its next retro_run, and WAIT n advances n - 1 frames,
    so a first WAIT F + 1 and then WAIT (F - previous F) + 1 land every
    command at nesref f = F (the gates pair nesref f with cyc record f - 1)."""
    steps = []
    for line in Path(route).read_text().splitlines():
        line = line.split('#')[0].strip()
        if not line:
            continue
        f, *rest = line.split(None, 1)
        steps.append((int(f), rest[0].strip() if rest else '-'))
    out, held, last = [], set(), None
    for f, what in sorted(steps, key=lambda st: st[0]):
        if f != last:
            out.append(f'WAIT {f + 1 if last is None else f - last + 1}')
        last = f
        if what.startswith('DISK_'):
            out.append(what)
            continue
        if what.startswith(('1:', '2:')):
            raise SystemExit('nesref scripts drive controller 1 only')
        new = set() if what in ('-', '') else {b.upper() for b in what.replace(' ', '+').split('+') if b}
        out += [f'RELEASE {b}' for b in sorted(held - new)] + [f'HOLD {b}' for b in sorted(new - held)]
        held = new
    out.append(f'WAIT {frames - (last or 0) + 10}')
    return out


def nesref_env(args, sysdir, savedir, extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith('NESREF_')}
    env.update({'NESREF_SYSTEM_DIR': str(sysdir), 'NESREF_SAVE_DIR': str(savedir)})
    env.update(extra)
    return env


def run_nesref(args, work, extra, log):
    savedir = work / 'save'
    savedir.mkdir(parents=True, exist_ok=True)
    p = subprocess.run([str(args.nesref), str(args.core), str(args.image)], cwd=work,
                       env=nesref_env(args, args.sysdir, savedir, extra), capture_output=True, text=True,
                       timeout=600, creationflags=NO_WINDOW)
    Path(log).write_text(p.stdout + p.stderr)
    if p.returncode:
        raise RuntimeError(f'nesref exit {p.returncode}; see {log}')
    return p.stdout


def ram_trace(path):
    """Per-frame CPU RAM from nesref's delta JSONL: {frame: bytes}."""
    ram, frames, cur = bytearray(0x800), {}, None
    for line in Path(path).read_text().splitlines():
        r = json.loads(line)
        f = r['f']
        if cur is not None and f != cur:
            frames[cur] = bytes(ram)
        cur = f
        ram[int(r['adr'], 16)] = int(r['val'], 16)
    if cur is not None:
        frames[cur] = bytes(ram)
    # A frame with no line changed nothing: it holds the previous frame's RAM.
    if frames:
        held = None
        for f in range(min(frames), max(frames) + 1):
            held = frames.setdefault(f, held)
    return frames


def parse_state(blob):
    u = lambda o: struct.unpack_from('<I', blob, o)[0]
    for o in range(0, len(blob) - 45000):
        if u(o) == 8192 and u(o + 4 + 8192) == 32768:
            chr_ram = blob[o + 4:o + 4 + 8192]
            w = o + 8 + 8192
            wram = blob[w:w + 32768]
            q = w + 32768
            assert u(q) == 0 and u(q + 4) == 2048, 'unexpected BaseMapper layout'
            nt = blob[q + 8:q + 8 + 2048]
            fds = None
            for h in range(q + 8 + 2048, len(blob) - 400):
                if u(h) == 314 and u(h + 4) == 314:
                    b = blob[h + 8:h + 8 + 314]
                    fds = dict(irq_reload=struct.unpack_from('<H', b, 0)[0], irq_counter=struct.unpack_from('<H', b, 2)[0],
                               irq_enabled=b[4], motor=b[9], reset=b[10], read=b[11], crc_control=b[12],
                               ready=b[13], irq_disk_enabled=b[14], read_data=b[19],
                               side=u(h + 8 + 21), position=u(h + 8 + 25), delay=u(h + 8 + 29),
                               gap_ended=b[34], scanning=b[35], transfer=b[36])
                    break
            return dict(chr=chr_ram, prg=wram, nt=nt, fds=fds)
    raise RuntimeError('no BaseMapper arrays in the state blob')


def nesref_frame(args, root, frame, script_lines):
    work = root / f'f{frame:05d}'
    shot, state = work / 'shot.png', work / 'state.bin'
    if not (shot.exists() and state.exists()):
        work.mkdir(parents=True, exist_ok=True)
        extra = {'NESREF_FRAMES': str(frame), 'NESREF_SHOT': str(frame), 'NESREF_SHOT_FILE': str(shot),
                 'NESREF_STATEDUMP': f'{frame}:{state}', 'NESREF_TRACE_FILE': str(work / 'trace.jsonl'),
                 'NESREF_FDS_BOOT_DISK': args.boot}
        if script_lines:
            (work / 'script.txt').write_text('\n'.join(script_lines) + f'\nWAIT {frame + 10}\n')
            extra['NESREF_SCRIPT'] = str(work / 'script.txt')
        run_nesref(args, work, extra, work / 'nesref.log')
    return frame, read_png_rgb(shot), parse_state(state.read_bytes())


# ---------------------------------------------------------------- cyc side
def run_cyc(args, work, frames, extra, name):
    log = work / f'{name}.log'
    cmd = [str(args.cyc), str(args.image), '--fds-bios', str(args.bios), '--frames', str(frames),
           '--ram-init', 'zeros', '--frame-log', str(work / f'{name}.frames'), '--frame-log-at', args.phase,
           '--ring-out', str(work / f'{name}.ring')] + extra
    p = subprocess.run(cmd, cwd=work, capture_output=True, text=True, timeout=1800, creationflags=NO_WINDOW)
    log.write_text(' '.join(cmd) + '\n' + p.stdout + p.stderr)
    if p.returncode:
        raise RuntimeError(f'cyc exit {p.returncode}; see {log}')
    return p.stdout, read_frame_log(work / f'{name}.frames')


def read_frame_log(path):
    data = Path(path).read_bytes()
    version = struct.unpack_from('<I', data, 8)[0]
    assert data[:8] == b'CYCFRAME' and version in (1, 2), version
    pos, out = 12, {}
    while pos < len(data):
        frame, lo, hi, n_ram, n_ciram, n_cart, n_chr, n_pic = struct.unpack_from('<8I', data, pos)
        n_audio = struct.unpack_from('<I', data, pos + 32)[0] if version >= 2 else 0
        pos += 32 + (4 if version >= 2 else 0)
        rec = {'cycles': lo | hi << 32, 'palette': data[pos:pos + 32], 'oam': data[pos + 32:pos + 288]}
        pos += 288
        for key, n in (('cpu', n_ram), ('nt', n_ciram), ('prg', n_cart), ('chr', n_chr)):
            rec[key] = data[pos:pos + n]
            pos += n
        rec['pic'] = struct.unpack_from(f'<{n_pic // 2}H', data, pos)
        pos += n_pic
        rec['fds_audio'] = data[pos:pos + n_audio]
        pos += n_audio
        out[frame] = rec
    return out


def cyc_drive_positions(ring_path):
    """The drive position at the end of each frame, from the ring's byte events."""
    last = {}
    for line in Path(ring_path).read_text().splitlines():
        if line.startswith('#'):
            continue
        parts = line.split(' ')
        if parts[3] == 'fds.byte':
            last[int(parts[1])] = int(parts[4].split('=')[1])
    return last


# ---------------------------------------------------------------- comparison
def picture_diff(pic, rgb):
    bad, emphasis = 0, 0
    for i, idx in enumerate(pic):
        if idx >> 6:
            emphasis += 1
        if MESEN_PALETTE[idx & 0x3F] != rgb[i]:
            bad += 1
    return bad, emphasis


def first_diff(a, b):
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i, x, y
    return None


def compare(cyc, ref_ram, ref_frames, offset, report, positions=None):
    rows, first = [], {}
    for f, rgb, state in ref_frames:
        k = f - offset
        rec = cyc.get(k)
        if rec is None:
            continue
        row = {'frame': f}
        ram = ref_ram.get(f)
        row['cpu'] = None if ram is None else sum(x != y for x, y in zip(rec['cpu'], ram))
        if row['cpu']:
            row['cpu_diffs'] = [f'{a:04X}:{x:02X}/{y:02X}' for a, (x, y) in enumerate(zip(rec['cpu'], ram)) if x != y][:12]
        row['prg'] = sum(x != y for x, y in zip(rec['prg'], state['prg']))
        row['nt'] = sum(x != y for x, y in zip(rec['nt'][:2048], state['nt']))
        row['chr'] = sum(x != y for x, y in zip(rec['chr'], state['chr']))
        row['pic'], row['emphasis'] = picture_diff(rec['pic'], rgb)
        row['mesen_pos'] = state['fds']['position'] if state['fds'] else None
        row['mesen_fds'] = state['fds']
        row['cyc_last_byte'] = positions.get(k) if positions else None
        row['cyc_cycles'] = rec['cycles']
        for key in ('cpu', 'prg', 'nt', 'chr', 'pic'):
            if row[key] and key not in first:
                where = None
                if key in ('cpu', 'prg', 'nt', 'chr'):
                    ref = ram if key == 'cpu' else state[key]
                    where = first_diff(rec[key], ref)
                first[key] = (f, where)
        rows.append(row)
    for r in rows:
        report.write(json.dumps(r) + '\n')
    return rows, first


def summarize(name, rows, first):
    n = len(rows)
    ok = {k: sum(1 for r in rows if r[k] == 0) for k in ('cpu', 'prg', 'nt', 'chr', 'pic')}
    print(f'{name}: {n} frames compared; identical frames: ' +
          ', '.join(f'{k} {v}/{n}' for k, v in ok.items()))
    for k, (f, where) in sorted(first.items(), key=lambda kv: kv[1][0]):
        detail = f' first byte {where[0]:#06x} cyc={where[1]:#04x} mesen={where[2]:#04x}' if where else ''
        print(f'  first {k} divergence at nesref frame {f}{detail}')
    return all(v == n for v in ok.values())


def pairing_offset(cyc, ref_ram, frames):
    best = None
    for off in range(-3, 4):
        score = sum(1 for f in frames if f in ref_ram and (f - off) in cyc and cyc[f - off]['cpu'] == ref_ram[f])
        if best is None or score > best[1]:
            best = (off, score)
    return best


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gate', choices=['nodisk', 'boot', 'otocky', 'input', 'insert'], required=True)
    ap.add_argument('--input', type=Path, help='--gate input: the cyc --input route both machines run')
    ap.add_argument('--cyc', type=Path, required=True, help='cyc_interp or a compiled FDS program')
    ap.add_argument('--cyc-args', default='', help='extra cyc arguments (space separated)')
    ap.add_argument('--nesref', type=Path, required=True)
    ap.add_argument('--core', type=Path)
    ap.add_argument('--image', type=Path, required=True)
    ap.add_argument('--bios', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--frames', help='nesref frames to compare: A:B[:STEP] or a,b,c (default per gate)')
    ap.add_argument('--offset', type=int, help='nesref frame - (cyc record + 1); measured if omitted')
    ap.add_argument('--jobs', type=int, default=8)
    ap.add_argument('--phase', choices=['mesen', 'vblank'], default='mesen',
                    help="where cyc's snapshot is taken: Mesen's frame end (scanline 240, default) or cyc's own (VBlank)")
    args = ap.parse_args()
    args.core = args.core or args.nesref.parent / 'cores' / 'mesen_libretro.dll'
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    args.sysdir = out / 'system'
    args.sysdir.mkdir(exist_ok=True)
    shutil.copyfile(args.bios, args.sysdir / 'disksys.rom')
    image = out / ('disk' + args.image.suffix.lower())
    shutil.copyfile(args.image, image)
    args.image = image

    script, cyc_extra = [], args.cyc_args.split()
    if args.gate == 'nodisk':
        args.boot, default_frames = 'none', '60,155,177,300,600,1200'
        cyc_extra += ['--fds-boot-disk', 'none', '--fds-hle', 'no-auto-insert']
    elif args.gate == 'insert':
        # Where cyc's auto insert puts side A in (the end of frame N = a host
        # event before frame N + 1), and nesref's DISK_INSERT at the same point
        # (nesref WAIT n applies at f = n - 1, as the otocky gate's 1199 / WAIT 1200).
        args.boot, default_frames = 'none', '5,8,9,10,20,60,155,300,600,751,800'
        cyc_extra += ['--fds-boot-disk', 'none']
        probe = subprocess.run([str(args.cyc.resolve()), str(args.image), '--fds-bios', str(args.bios), '--frames', '120',
                                '--fds-boot-disk', 'none'] + args.cyc_args.split(), capture_output=True, text=True,
                               creationflags=NO_WINDOW)
        import re as _re
        m = _re.search(r'side A auto-inserted at the end of frame (\d+)', probe.stdout)
        if not m:
            raise SystemExit('cyc did not auto-insert: ' + probe.stdout + probe.stderr)
        event = int(m.group(1)) + 1
        print(f'cyc auto insert: end of frame {event - 1} (= a host insert before frame {event}); nesref WAIT {event + 1}')
        script = [f'WAIT {event + 1}', 'DISK_INSERT A']
    elif args.gate == 'boot':
        args.boot, default_frames = '0', '1:800'
        cyc_extra += ['--fds-boot-disk', '0']
    elif args.gate == 'input':
        if not args.input or not args.frames:
            raise SystemExit('--gate input needs --input and --frames')
        args.boot, default_frames = '0', args.frames
        cyc_extra += ['--fds-boot-disk', '0', '--input', str(args.input.resolve())]
    else:
        # nesref WAIT n advances n - 1 frames: eject at f=1199, select B and
        # insert at f=1258 (the phase-1 validation script).
        args.boot, default_frames = '0', '1190,1199,1230,1258,1300,1500,1800,2400,3000'
        script = ['WAIT 1200', 'DISK_EJECT', 'WAIT 60', 'DISK_SELECT B', 'DISK_INSERT']
        cyc_extra += ['--fds-boot-disk', '0', '--fds-event', '1199:eject', '--fds-event', '1258:insert=1']
    spec = args.frames or default_frames
    if ':' in spec:
        a, b, *step = (int(x) for x in spec.split(':'))
        frames = list(range(a, b + 1, step[0] if step else 1))
    else:
        frames = [int(x) for x in spec.split(',')]
    if args.gate == 'input':
        script = nesref_script(args.input, 0)[:-1]

    # One long nesref run for the per-frame CPU RAM trace.
    trace = out / 'nesref_trace.jsonl'
    if not trace.exists():
        extra = {'NESREF_FRAMES': str(max(frames) + 2), 'NESREF_TRACE_FILE': str(trace), 'NESREF_FDS_BOOT_DISK': args.boot}
        if script:
            (out / 'script.txt').write_text('\n'.join(script) + f'\nWAIT {max(frames) + 10}\n')
            extra['NESREF_SCRIPT'] = str(out / 'script.txt')
        run_nesref(args, out, extra, out / 'nesref_trace.log')
    ref_ram = ram_trace(trace)

    stdout, cyc = run_cyc(args, out, max(frames) + 4, cyc_extra, 'cyc')
    print(stdout.strip())
    offset, score = (args.offset, None) if args.offset is not None else pairing_offset(cyc, ref_ram, frames)
    if score is not None:
        print(f'pairing: nesref frame f = cyc frame record {offset:+d} (CPU RAM equal on {score}/{len(frames)} frames)')

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        ref_frames = sorted(pool.map(lambda f: nesref_frame(args, out / 'nesref', f, script), frames))
    positions = cyc_drive_positions(out / 'cyc.ring')
    with open(out / 'compare.jsonl', 'w') as report:
        rows, first = compare(cyc, ref_ram, ref_frames, offset, report, positions)
    drift = [(r['frame'], positions.get(r['frame'] - offset), r['mesen_pos']) for r in rows
             if r['mesen_pos'] is not None and positions.get(r['frame'] - offset) is not None and
             positions.get(r['frame'] - offset) + 1 != r['mesen_pos']]
    if drift:
        f, mine, theirs = drift[0]
        print(f'  first drive-position difference at nesref frame {f}: cyc last byte {mine}, Mesen next byte {theirs}')
    ok = summarize(args.gate, rows, first)
    print('PASS' if ok else 'DIFFERENCES (see compare.jsonl)')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
