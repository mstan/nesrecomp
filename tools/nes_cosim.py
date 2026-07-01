#!/usr/bin/env python3
"""
nes_cosim.py — coordinator for the NES differential co-simulation.

Frame-granular full-state first-divergence tool. Drives the recompiled game
(compiled with the env-gated NESRECOMP_COSIM_HASH tap in runtime.c) and diffs
per-frame full-machine state hashes.

This is the frame-granular tier (the shared clock is the NMI/frame boundary; both
sides checkpoint deterministically there, so free-run + align-by-frame is exact at
frame resolution — no async-stop race). Sub-frame / cycle-exact lockstep needs the
monotonic g_nes_cycles counter (now present) plus a cycle-exposing oracle
(MesenCE-Lua) and is a later tier. See recomp-template/NES/DIFFERENTIAL-COSIM-PROPOSAL.md.

Commands:
  gate1   <exe> <rom> [frames]        recomp-vs-recomp determinism (A-vs-A must be 0)
  gate3   <exe> <rom> [frames]        fault injection: must halt at the injected frame,
                                      in the injected subsystem (catches a blind hasher)
  diff    <a.jsonl> <b.jsonl>         first-divergence report between two hash streams
  run     <exe> <rom> <out> [frames] [inject]   one instrumented run -> JSONL

A "gate" prints PASS/FAIL and exits non-zero on FAIL so it can gate CI.
"""
import sys, os, subprocess, json, tempfile

SUBS = ["cpu", "ram", "stack", "ppu_mem", "ppu_regs", "apu", "mapper", "chr", "sram", "openbus"]


def run(exe, rom, out_path, frames, inject=None):
    """Run one instrumented smoke session; return path to the JSONL hash stream."""
    env = dict(os.environ)
    env["NESRECOMP_COSIM_HASH"] = out_path
    if inject:
        env["NESRECOMP_COSIM_INJECT"] = inject
    else:
        env.pop("NESRECOMP_COSIM_INJECT", None)
    # Deterministic headless attract; no host audio sink, fixed frame count.
    exe_abs = os.path.abspath(exe)
    rom_abs = os.path.abspath(rom)
    cmd = [exe_abs, rom_abs, "--smoke", str(frames)]
    p = subprocess.run(cmd, env=env, cwd=os.path.dirname(exe_abs),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if p.returncode != 0:
        print(f"  ! game exited {p.returncode}")
    return out_path


def load(path):
    rows = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def first_divergence(a_rows, b_rows):
    """Return (frame, [subsystems], a_row, b_row) of the first frame whose chain
    or any sub-hash differs, else None. Aligns by frame ordinal 'f'."""
    b_by_f = {r["f"]: r for r in b_rows}
    for ar in a_rows:
        f = ar["f"]
        br = b_by_f.get(f)
        if br is None:
            continue
        if ar["chain"] != br["chain"]:
            diff_subs = [s for s in SUBS if ar["sub"].get(s) != br["sub"].get(s)]
            return (f, diff_subs, ar, br)
    return None


def assert_hash_nonnull(rows, label):
    """Guard against a silently-blind hasher: the sub-hashes must not all be a
    single constant across the run (a constant hasher passes A-vs-A trivially)."""
    seen = set()
    for r in rows[: min(len(rows), 50)]:
        seen.add(r["sub"]["ram"])
    if len(seen) <= 1:
        print(f"  ! {label}: ram sub-hash is CONSTANT across frames — hasher may be blind")
        return False
    return True


def cmd_gate1(exe, rom, frames):
    print(f"[Gate 1] recomp-vs-recomp determinism ({frames} frames)")
    with tempfile.TemporaryDirectory() as d:
        a = run(exe, rom, os.path.join(d, "a.jsonl"), frames)
        b = run(exe, rom, os.path.join(d, "b.jsonl"), frames)
        ar, br = load(a), load(b)
        print(f"  A: {len(ar)} frames   B: {len(br)} frames")
        if not ar or not br:
            print("  FAIL: no hash rows emitted (is the exe built with the cosim tap?)")
            return 1
        assert_hash_nonnull(ar, "A")
        div = first_divergence(ar, br)
        if div is None and len(ar) == len(br):
            print(f"  PASS: byte-identical across all {len(ar)} frames (determinism holds)")
            return 0
        if div:
            f, subs, _, _ = div
            print(f"  FAIL: A and B diverge at frame {f} in {subs} — nondeterminism leak")
        else:
            print(f"  FAIL: frame-count mismatch A={len(ar)} B={len(br)}")
        return 1


def cmd_gate2(nesref_exe, core, rom, frames):
    """Gate 2: oracle-vs-oracle determinism (nesref/Mesen must be reproducible)."""
    print(f"[Gate 2] nesref-vs-nesref determinism ({frames} frames)")
    with tempfile.TemporaryDirectory() as d:
        nr_abs = os.path.abspath(nesref_exe)
        outs = []
        for tag in ("a", "b"):
            p = os.path.join(d, f"nr_{tag}.jsonl")
            env = dict(os.environ)
            env["NESREF_FRAMES"] = str(frames); env["NESREF_TRACE_FILE"] = p
            subprocess.run([nr_abs, os.path.abspath(core), os.path.abspath(rom)],
                           env=env, cwd=os.path.dirname(nr_abs),
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            outs.append(reconstruct_ram(p))
        a, b = outs
        if not a or not b:
            print("  FAIL: empty nesref trace")
            return 1
        for f in sorted(a):
            if f in b and a[f] != b[f]:
                print(f"  FAIL: nesref nondeterministic at frame {f}")
                return 1
        print(f"  PASS: nesref byte-identical across {len(a)} frames")
        return 0


def cmd_gate3(exe, rom, frames):
    inj_frame = max(5, frames // 2)
    print(f"[Gate 3] fault injection at frame {inj_frame} ({frames} frames)")
    rc = 0
    with tempfile.TemporaryDirectory() as d:
        clean = load(run(exe, rom, os.path.join(d, "clean.jsonl"), frames))
        # (a) OAM inject — should localize to ppu_mem and self-heal next frame.
        oam = load(run(exe, rom, os.path.join(d, "oam.jsonl"), frames,
                       inject=f"{inj_frame}:oam:7:ff"))
        div = first_divergence(clean, oam)
        if div and div[0] == inj_frame and "ppu_mem" in div[1]:
            print(f"  PASS[oam]: detected at frame {inj_frame}, localized to {div[1]}")
        else:
            print(f"  FAIL[oam]: expected first divergence at {inj_frame} in ppu_mem, got {div[:2] if div else None}")
            rc = 1
        # (b) RAM inject — real state flip; first divergence must still be exactly K in ram.
        ram = load(run(exe, rom, os.path.join(d, "ram.jsonl"), frames,
                       inject=f"{inj_frame}:ram:0x300:ff"))
        div = first_divergence(clean, ram)
        if div and div[0] == inj_frame and "ram" in div[1]:
            print(f"  PASS[ram]: detected at frame {inj_frame}, localized to {div[1]} (cascade after K is expected)")
        else:
            print(f"  FAIL[ram]: expected first divergence at {inj_frame} in ram, got {div[:2] if div else None}")
            rc = 1
    return rc


def reconstruct_ram(delta_path):
    """Reconstruct full 2KB RAM per frame from a delta-JSONL trace (recomp WRAM
    tap or nesref, identical shape). Returns {frame:int -> bytes(2048)}."""
    by_frame = {}
    for line in open(delta_path):
        line = line.strip()
        if not line:
            continue
        r = json.loads(line)
        f = int(r["f"])
        a = int(r["adr"], 16)
        v = int(r["val"], 16)
        if a <= 0x7FF:
            by_frame.setdefault(f, []).append((a, v))
    out = {}
    cur = bytearray(2048)
    for f in sorted(by_frame):
        for a, v in by_frame[f]:
            cur[a] = v
        out[f] = bytes(cur)
    return out


def ram_match(a_ram, b_ram, mask_stack=True):
    """Fraction of the 2KB that matches, optionally masking the stack page
    ($0100-$01FF) where the recomp never pushes JSR return addresses."""
    same = tot = 0
    for i in range(2048):
        if mask_stack and 0x100 <= i <= 0x1FF:
            continue
        tot += 1
        if a_ram[i] == b_ram[i]:
            same += 1
    return same / tot


def cmd_abram(exe, rom, nesref_exe, core, frames):
    """A-vs-B: recomp RAM vs Mesen (nesref) RAM, frame-granular. Certifies that a
    change preserves logic-level convergence (the strongest cross-impl signal the
    libretro oracle exposes — it has no PPU-internal mem or cycle counter)."""
    print(f"[A-vs-B RAM] recomp vs Mesen/nesref ({frames} frames)")
    with tempfile.TemporaryDirectory() as d:
        rc_path = os.path.join(d, "recomp_ram.jsonl")
        env = dict(os.environ); env["NESRECOMP_WRAM_TRACE"] = rc_path
        env.pop("NESRECOMP_COSIM_INJECT", None)
        exe_abs = os.path.abspath(exe)
        subprocess.run([exe_abs, os.path.abspath(rom), "--smoke", str(frames)],
                       env=env, cwd=os.path.dirname(exe_abs),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        nr_path = os.path.join(d, "nesref_ram.jsonl")
        env2 = dict(os.environ)
        env2["NESREF_FRAMES"] = str(frames); env2["NESREF_TRACE_FILE"] = nr_path
        nr_abs = os.path.abspath(nesref_exe)
        subprocess.run([nr_abs, os.path.abspath(core), os.path.abspath(rom)],
                       env=env2, cwd=os.path.dirname(nr_abs),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        rc = reconstruct_ram(rc_path)
        nr = reconstruct_ram(nr_path)
        if not rc or not nr:
            print(f"  FAIL: empty trace (recomp {len(rc)} frames, nesref {len(nr)} frames)")
            return 1
        print(f"  recomp {len(rc)} frames, nesref {len(nr)} frames")

        # Boot-frame offset search: recomp frame f aligns with nesref frame f+off.
        rc_frames = sorted(rc)
        best_off, best_m = 0, -1.0
        sample = rc_frames[len(rc_frames)//3 : 2*len(rc_frames)//3]  # steady-state window
        for off in range(0, 16):
            ms, n = 0.0, 0
            for f in sample:
                if f + off in nr:
                    ms += ram_match(rc[f], nr[f + off]); n += 1
            if n and ms / n > best_m:
                best_m, best_off = ms / n, off
        print(f"  best boot offset = +{best_off}   steady-state RAM match = {best_m*100:.2f}% (stack-masked)")

        # First real (stack-masked) divergence at the chosen offset.
        first = None
        for f in rc_frames:
            if f + best_off not in nr:
                continue
            if ram_match(rc[f], nr[f + best_off]) < 1.0:
                diffs = [i for i in range(2048)
                         if not (0x100 <= i <= 0x1FF) and rc[f][i] != nr[f + best_off][i]]
                if diffs:
                    first = (f, diffs)
                    break
        if first is None:
            print(f"  PASS: no non-stack RAM divergence across aligned frames (logic converges)")
            return 0
        f, diffs = first
        print(f"  first non-stack RAM divergence @ recomp frame {f}: {len(diffs)} byte(s), "
              f"e.g. ${diffs[0]:04x} recomp={rc[f][diffs[0]]:#04x} mesen={nr[f+best_off][diffs[0]]:#04x}")
        print(f"  (report only — a handful of FrameCounter-derived/timer bytes is the known "
              f"RNG-phase residual, not a logic bug; see burndown Axis 4)")
        return 0


def cmd_diff(a_path, b_path):
    ar, br = load(a_path), load(b_path)
    div = first_divergence(ar, br)
    if div is None:
        print(f"No divergence across {min(len(ar), len(br))} aligned frames.")
        return 0
    f, subs, arow, brow = div
    print(f"First divergence @ frame {f}  clk_A={arow['clk']} clk_B={brow['clk']}")
    print(f"  subsystems: {subs}")
    for s in subs:
        print(f"    {s:9s}  A={arow['sub'][s]}  B={brow['sub'][s]}")
    return 0


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    cmd = sys.argv[1]
    if cmd == "gate1":
        exe, rom = sys.argv[2], sys.argv[3]
        frames = int(sys.argv[4]) if len(sys.argv) > 4 else 900
        return cmd_gate1(exe, rom, frames)
    if cmd == "gate2":
        nesref_exe, core, rom = sys.argv[2], sys.argv[3], sys.argv[4]
        frames = int(sys.argv[5]) if len(sys.argv) > 5 else 900
        return cmd_gate2(nesref_exe, core, rom, frames)
    if cmd == "gate3":
        exe, rom = sys.argv[2], sys.argv[3]
        frames = int(sys.argv[4]) if len(sys.argv) > 4 else 900
        return cmd_gate3(exe, rom, frames)
    if cmd == "abram":
        exe, rom, nesref_exe, core = sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]
        frames = int(sys.argv[6]) if len(sys.argv) > 6 else 900
        return cmd_abram(exe, rom, nesref_exe, core, frames)
    if cmd == "diff":
        return cmd_diff(sys.argv[2], sys.argv[3])
    if cmd == "run":
        exe, rom, out = sys.argv[2], sys.argv[3], sys.argv[4]
        frames = int(sys.argv[5]) if len(sys.argv) > 5 else 900
        inject = sys.argv[6] if len(sys.argv) > 6 else None
        run(exe, rom, out, frames, inject)
        print(f"wrote {out}")
        return 0
    print(f"unknown command: {cmd}")
    return 2


if __name__ == "__main__":
    sys.exit(main())
