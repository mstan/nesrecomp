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


def clear_saves(exe):
    """Delete the recomp's battery SRAM before a run so every session boots from
    the same canonical fresh state (matching nesref, which boots fresh). Without
    this, battery games (Zelda, Faxanadu) fail Gate 1: the first instance writes
    <exe_dir>/saves/*.srm, the second loads it → a different boot path and a
    spurious 'sram nondeterminism' divergence that is really just save-state
    carryover, not a determinism leak."""
    saves = os.path.join(os.path.dirname(os.path.abspath(exe)), "saves")
    if os.path.isdir(saves):
        for fn in os.listdir(saves):
            if fn.endswith(".srm"):
                try:
                    os.remove(os.path.join(saves, fn))
                except OSError:
                    pass


# Co-sim drive script. Set from a trailing CLI arg (env is unreliable across the
# msys2 python boundary — COSIM_SCRIPT silently doesn't propagate). CLI arg wins;
# COSIM_SCRIPT env is a fallback for environments where it does propagate.
COSIM_SCRIPT_PATH = None


def cosim_script():
    return COSIM_SCRIPT_PATH or os.environ.get("COSIM_SCRIPT")


def recomp_drive_args(frames):
    """Recomp CLI for a co-sim session. Default: headless attract (--smoke N).
    With a drive script set, drive the recomp through that input script instead
    (windowed) so the co-sim can reach input-gated screens (world map, 1-1).
    Both engines read the SAME script (nesref via NESREF_SCRIPT)."""
    s = cosim_script()
    if s:
        return ["--script", os.path.abspath(s)]
    return ["--smoke", str(frames)]


def nesref_script_env(env):
    """Mirror the drive script into the nesref oracle's NESREF_SCRIPT (set in the
    subprocess env dict, which DOES propagate — unlike the parent's env)."""
    s = cosim_script()
    if s:
        env["NESREF_SCRIPT"] = os.path.abspath(s)
    return env


def parse_benign(default=""):
    """Addresses to exclude from the divergence verdict as KNOWN host-modeled /
    RNG-phase noise (frame-sync ticks, NMI-protected temps, RNG pool). Without
    this the first-divergence report fixates on a benign temp at frame ~4 and
    hides the real state split. Format: COSIM_BENIGN='0x01,0x10,0x15,0x781-0x789'.
    Ranges are inclusive. Env overrides `default`."""
    spec = os.environ.get("COSIM_BENIGN", default)
    s = set()
    for tok in spec.split(","):
        tok = tok.strip()
        if not tok:
            continue
        if "-" in tok:
            a, b = tok.split("-", 1)
            s.update(range(int(a, 0), int(b, 0) + 1))
        else:
            s.add(int(tok, 0))
    return s


def run(exe, rom, out_path, frames, inject=None):
    """Run one instrumented smoke session; return path to the JSONL hash stream."""
    clear_saves(exe)
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
    or any sub-hash differs, else None. Aligns by frame ordinal 'f'.

    Both sides are deduped last-wins per 'f': the cycle-derived video-frame index
    can map two emits (e.g. an NMI-off boundary emit and the following NMI-on emit
    at a boot/scene transition) to the same frame number. reconstruct_ram() already
    takes the last state per frame, so this matches that semantics — otherwise the
    first f=N row of A would be compared against the last f=N row of B, a spurious
    'divergence' that is really just intra-frame double-emit, not nondeterminism."""
    a_by_f = {r["f"]: r for r in a_rows}
    b_by_f = {r["f"]: r for r in b_rows}
    for f in sorted(a_by_f):
        ar = a_by_f[f]
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


def adaptive_offset_match(rc, nr, rc_frames, win=40, drift=3):
    """Windowed alignment that FOLLOWS a mid-run frame-offset shift (the NMI-off
    frame-count desync, e.g. Gumshoe): the recomp counts NMI callbacks, the oracle
    counts video frames, so an NMI-off stretch bumps the offset. Per window, search
    only within +/-drift of the current offset (small, so it can track a genuine
    shift but can't wander far enough to mask a real logic divergence). Returns
    (overall_match, [(frame, old_off, new_off), ...]). If the match stays high only
    with shifts at discrete points, the spread was alignment, not divergence; if it
    stays low at every offset (Zelda FrameCounter-phase, MM3 fibers), it is real."""
    if not rc_frames:
        return 0.0, []
    def win_best(frames, lo, hi):
        best_o, best_m = None, -1.0
        for o in range(max(0, lo), hi + 1):
            ms = n = 0.0
            for f in frames:
                if f + o in nr:
                    ms += ram_match(rc[f], nr[f + o]); n += 1
            if n and ms / n > best_m:
                best_m, best_o = ms / n, o
        return best_o, best_m
    cur, _ = win_best(rc_frames[:win], 0, 15)
    if cur is None:
        cur = 0
    resyncs, tot_same, tot = [], 0, 0
    for i in range(0, len(rc_frames), win):
        wf = rc_frames[i:i + win]
        o, _ = win_best(wf, cur - drift, cur + drift)
        if o is None:
            o = cur
        if o != cur:
            resyncs.append((wf[0], cur, o)); cur = o
        for f in wf:
            if f + cur in nr:
                a, b = rc[f], nr[f + cur]
                for k in range(2048):
                    if 0x100 <= k <= 0x1FF:
                        continue
                    tot += 1
                    if a[k] == b[k]:
                        tot_same += 1
    return (tot_same / tot if tot else 0.0), resyncs


def cmd_abram(exe, rom, nesref_exe, core, frames):
    """A-vs-B: recomp RAM vs Mesen (nesref) RAM, frame-granular. Certifies that a
    change preserves logic-level convergence (the strongest cross-impl signal the
    libretro oracle exposes — it has no PPU-internal mem or cycle counter)."""
    print(f"[A-vs-B RAM] recomp vs Mesen/nesref ({frames} frames)")
    with tempfile.TemporaryDirectory() as d:
        rc_path = os.path.join(d, "recomp_ram.jsonl")
        env = dict(os.environ); env["NESRECOMP_WRAM_TRACE"] = rc_path
        env.pop("NESRECOMP_COSIM_INJECT", None)
        clear_saves(exe)
        exe_abs = os.path.abspath(exe)
        subprocess.run([exe_abs, os.path.abspath(rom), *recomp_drive_args(frames)],
                       env=env, cwd=os.path.dirname(exe_abs),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        nr_path = os.path.join(d, "nesref_ram.jsonl")
        env2 = dict(os.environ)
        env2["NESREF_FRAMES"] = str(frames); env2["NESREF_TRACE_FILE"] = nr_path
        nesref_script_env(env2)
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

        # Benign mask: host-modeled/RNG-phase addresses that always diverge
        # innocuously (COSIM_BENIGN). Excluding them is what turns the verdict
        # HONEST — otherwise the first-divergence latches onto a frame-4 temp and
        # a real state split (recomp in a level, oracle on the map) is reported
        # as "converged". SMB3 default: Temp_Var1/2 $01/$15, VBlank_Tick $10,
        # scroll temp $69, Random_Pool $781-$789.
        benign = parse_benign("0x01,0x10,0x15,0x69,0x781-0x789")
        def masked(i):
            return (0x100 <= i <= 0x1FF) or (i in benign)

        # First real (stack- and benign-masked) divergence at the chosen offset.
        first = None
        for f in rc_frames:
            if f + best_off not in nr:
                continue
            diffs = [i for i in range(2048)
                     if not masked(i) and rc[f][i] != nr[f + best_off][i]]
            if diffs:
                first = (f, diffs)
                break
        # Divergent-address histogram over steady state (skip boot ramp): how
        # often each byte differs. A short list of always-diverging addresses =
        # frame-sync flags / FrameCounter-derived RNG-phase (benign snapshot-timing
        # artifacts); a broad spread = a real logic divergence to chase.
        from collections import Counter
        hist = Counter(); hist_benign = Counter(); ncmp = 0
        for f in rc_frames:
            if f < 200 or f + best_off not in nr:
                continue
            ncmp += 1
            a, b = rc[f], nr[f + best_off]
            for i in range(2048):
                if 0x100 <= i <= 0x1FF:
                    continue
                if a[i] != b[i]:
                    (hist_benign if i in benign else hist)[i] += 1
        print(f"  steady-state divergent addresses (benign-masked): {len(hist)} real, "
              f"{len(hist_benign)} benign, over {ncmp} frames")
        for a, c in hist.most_common(12):
            print(f"    ${a:04x}: {c}/{ncmp} ({c/ncmp*100:.0f}%)  REAL")
        # HONEST verdict: the first NON-BENIGN divergence is the real state split.
        # Do NOT declare convergence when real state bytes differ (the old code
        # averaged them away / re-aligned past them -> false "CONVERGED").
        if first is None and not hist:
            print(f"  => CONVERGED: no real (non-benign) RAM divergence across aligned frames.")
        elif first is not None:
            f, diffs = first
            print(f"  ***DIVERGED*** first REAL RAM divergence @ recomp frame {f}: {len(diffs)} byte(s), "
                  f"e.g. ${diffs[0]:04x} recomp={rc[f][diffs[0]]:#04x} mesen={nr[f+best_off][diffs[0]]:#04x}")
            print(f"     (state split — NOT masked by benign/RNG. Chase this frame in the game logic.)")
            return 1
        else:
            print(f"  first real divergence not localized to a single frame; "
                  f"{len(hist)} real divergent addr(s) in steady state -> genuine divergence.")
            return 1
        # Broad spread? Re-check with an adaptive (piecewise) offset. If a discrete
        # offset shift recovers a high match, the spread was a frame-count desync at
        # an NMI-off transition (Gumshoe-class), NOT a logic divergence. If it stays
        # low, the divergence is real/phase (Zelda FrameCounter, MM3 fibers).
        if len(hist) > 3:
            adapt_m, resyncs = adaptive_offset_match(rc, nr, rc_frames)
            print(f"  adaptive-offset RAM match = {adapt_m*100:.2f}%  ({len(resyncs)} offset shift(s))")
            for fr, o0, o1 in resyncs[:6]:
                print(f"    offset shift @ frame {fr}: +{o0} -> +{o1}  (NMI-off frame-count desync)")
            if adapt_m > 0.99 and resyncs:
                print(f"  => CONVERGED once re-aligned: the fixed-offset spread was NMI-off frame-count "
                      f"desync, not a logic divergence.")
            elif adapt_m > 0.99:
                print(f"  => CONVERGED: logic matches; residual is frame-sync/RNG-phase ZP flags.")
            else:
                print(f"  => still {adapt_m*100:.1f}% after re-align => genuine divergence or "
                      f"FrameCounter/host-state phase (not fixable by offset).")
        return 0


def cmd_abcycle(exe, rom, nesref_exe, core, frames):
    """A-vs-B CYCLE: recomp g_nes_cycles vs Mesen's cycle counter (extracted
    in-process from nesref's retro_serialize blob -- no external Mesen.exe). Both
    count real guest cycles, so the per-frame advance rate should match hardware;
    the residual is the Axis-2 frame-length skew (recomp OPS_PER_FRAME=29781 vs
    NTSC 29780.5). This is the cross-impl cycle channel the floor was missing, and
    the baseline the Rung-2 dot-master clock must drive toward zero."""
    print(f"[A-vs-B CYCLE] recomp g_nes_cycles vs Mesen (in-process, {frames} frames)")
    with tempfile.TemporaryDirectory() as d:
        rc_path = os.path.join(d, "cosim.jsonl")
        env = dict(os.environ); env["NESRECOMP_COSIM_HASH"] = rc_path
        env.pop("NESRECOMP_COSIM_INJECT", None)
        clear_saves(exe)
        exe_abs = os.path.abspath(exe)
        subprocess.run([exe_abs, os.path.abspath(rom), "--smoke", str(frames)],
                       env=env, cwd=os.path.dirname(exe_abs),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cyc_path = os.path.join(d, "nesref_cyc.jsonl")
        env2 = dict(os.environ)
        env2["NESREF_FRAMES"] = str(frames); env2["NESREF_CYCLE_FILE"] = cyc_path
        env2["NESREF_TRACE_FILE"] = os.path.join(d, "nr_ram.jsonl")
        nr_abs = os.path.abspath(nesref_exe)
        subprocess.run([nr_abs, os.path.abspath(core), os.path.abspath(rom)],
                       env=env2, cwd=os.path.dirname(nr_abs),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        # Use bclk (cycles sampled AT the frame-boundary fire, before the NMI
        # handler) not clk (post-handler) -- clk carries the handler's frame-to-
        # frame length variance as spurious jitter. Fall back to clk if absent.
        rc = {r["f"]: int(r.get("bclk", r["clk"])) for r in load(rc_path)}
        nr = {}
        for line in open(cyc_path):
            line = line.strip()
            if line:
                r = json.loads(line); nr[r["f"]] = int(r["cyc"])
        if not rc or not nr:
            print(f"  FAIL: empty (recomp {len(rc)}, nesref {len(nr)})")
            return 1

        # Per-frame advance rate + JITTER on each side (offset-independent).
        # The recomp frame boundary is a cycle THRESHOLD with deferred/nested NMI
        # firing, so per-frame length is not constant -- report the spread, not
        # just the mean, or the number misleads (a clean "29777" hides 24k-33k swings).
        import statistics
        def stats(series):
            fs = sorted(series); fs = fs[len(fs)//5:]  # drop first 20% (boot)
            if len(fs) < 3:
                return None
            deltas = [series[fs[i]] - series[fs[i-1]]
                      for i in range(1, len(fs)) if fs[i] == fs[i-1] + 1]
            mean = (series[fs[-1]] - series[fs[0]]) / (fs[-1] - fs[0])
            return {"mean": mean, "n": len(fs),
                    "std": statistics.pstdev(deltas) if len(deltas) > 1 else 0.0,
                    "min": min(deltas) if deltas else 0, "max": max(deltas) if deltas else 0}
        rc_s, nr_s = stats(rc), stats(nr)
        print(f"  recomp: {rc_s['mean']:.3f} cyc/frame  (std {rc_s['std']:.0f}, "
              f"min {rc_s['min']}, max {rc_s['max']}, n={rc_s['n']})")
        print(f"  Mesen : {nr_s['mean']:.3f} cyc/frame  (std {nr_s['std']:.0f}, "
              f"min {nr_s['min']}, max {nr_s['max']}, n={nr_s['n']})")
        drift = rc_s['mean'] - nr_s['mean']
        print(f"  mean drift = {drift:+.3f} cyc/frame")
        if abs(drift) < 0.1:
            print(f"  => CONVERGED: recomp frame length matches Mesen (dot-accurate budget on).")
        else:
            print(f"  => recomp frame length is constant {rc_s['mean']:.1f} (std {rc_s['std']:.0f}) vs Mesen "
                  f"{nr_s['mean']:.1f}; drift {drift:+.2f} = the fixed OPS_PER_FRAME 29781 vs NTSC 29780.5.")
            print(f"     Rung-2 dot-accurate frame budget (NESRECOMP_DOT_CLOCK) closes this to ~0.")
        return 0


def reconstruct_image(delta_path, size):
    """Reconstruct a per-frame byte image (size bytes) from a delta-JSONL trace."""
    by_frame = {}
    for line in open(delta_path):
        line = line.strip()
        if not line:
            continue
        r = json.loads(line)
        a = int(r["adr"], 16)
        if a < size:
            by_frame.setdefault(int(r["f"]), []).append((a, int(r["val"], 16)))
    out = {}
    cur = bytearray(size)
    for f in sorted(by_frame):
        for a, v in by_frame[f]:
            cur[a] = v
        out[f] = bytes(cur)
    return out


def adaptive_offset_ppu(rc, nr, rc_keys, nr_keys, win=40, drift=3):
    """Windowed alignment for the sparse PPU-mem traces — the abppu twin of
    adaptive_offset_match. Follows a mid-run frame-offset shift (the NMI-off
    frame-count desync, Gumshoe-class: recomp counts NMI callbacks, oracle counts
    video frames) so the whole-run OAM/palette/NT verdict isn't dragged down by a
    discrete re-sync point. Per window, search only +/-drift of the current offset
    (small enough to track a genuine shift but not to wander far enough to mask a
    real divergence); alignment is scored on OAM (the discriminating region).
    Returns (oam, pal, nt, [(frame, old_off, new_off), ...])."""
    import bisect
    PAL_MIRROR = {0x100, 0x110, 0x114, 0x118, 0x11c}

    def asof(f):
        i = bisect.bisect_right(nr_keys, f) - 1
        return nr[nr_keys[i]] if i >= 0 else None

    def rmatch(a, b, lo, hi, skip=()):
        idx = [i for i in range(lo, hi) if i not in skip]
        return sum(1 for i in idx if a[i] == b[i]) / len(idx) if idx else 1.0

    def win_best(frames, lo, hi):
        best_o, best_m = None, -1.0
        for o in range(max(0, lo), hi + 1):
            ms = n = 0.0
            for f in frames:
                b = asof(f + o)
                if b is not None:
                    ms += rmatch(rc[f], b, 0, 0x100); n += 1
            if n and ms / n > best_m:
                best_m, best_o = ms / n, o
        return best_o

    if not rc_keys:
        return 0.0, 0.0, 0.0, []
    cur = win_best(rc_keys[:win], 0, 15)
    if cur is None:
        cur = 0
    resyncs = []
    oam_tot = pal_tot = nt_tot = 0.0; n = 0
    for i in range(0, len(rc_keys), win):
        wf = rc_keys[i:i + win]
        o = win_best(wf, cur - drift, cur + drift)
        if o is None:
            o = cur
        if o != cur:
            resyncs.append((wf[0], cur, o)); cur = o
        for f in wf:
            b = asof(f + cur)
            if b is not None:
                oam_tot += rmatch(rc[f], b, 0x000, 0x100)
                pal_tot += rmatch(rc[f], b, 0x100, 0x120, skip=PAL_MIRROR)
                nt_tot  += rmatch(rc[f], b, 0x200, 0xA00)
                n += 1
    if not n:
        return 0.0, 0.0, 0.0, resyncs
    return oam_tot / n, pal_tot / n, nt_tot / n, resyncs


def scroll_phase_match(rc, nr, rc_keys, nr_keys, off, step=2):
    """Disambiguate a low nametable match that a frame offset can't fix: attract
    demos SCROLL, and a small demo-timing phase (the NMI-off frame-count desync)
    leaves the recomp at a different horizontal scroll COLUMN than the oracle at
    the same aligned frame. Operate on the VISIBLE primary nametable (NT0, $2000)
    only -- the secondary NT is an off-screen prefetch buffer that legitimately
    differs at different scroll positions. Per frame, recover the best whole-screen
    horizontal column shift (32-col torus). High recovery => correct rendering, off
    only by demo scroll-phase, not a rendering divergence. The residual after
    recovery is the handful of columns that scrolled into view (each side prefetched
    a different edge). Returns (mean_recovered, median_shift, n)."""
    import bisect, statistics

    def asof(f):
        i = bisect.bisect_right(nr_keys, f) - 1
        return nr[nr_keys[i]] if i >= 0 else None

    def grid(img):
        return [[img[0x200 + r * 32 + c] for c in range(32)] for r in range(30)]

    def best_shift(ga, gb):
        best_m, best_s = -1.0, 0
        for s in range(32):
            m = sum(1 for r in range(30) for c in range(32)
                    if ga[r][c] == gb[r][(c + s) % 32])
            if m > best_m:
                best_m, best_s = m, s
        return best_m / (30 * 32), best_s

    tot, shifts = 0.0, []
    n = 0
    for f in rc_keys[::step]:
        b = asof(f + off)
        if b is None:
            continue
        m, s = best_shift(grid(rc[f]), grid(b))
        tot += m; shifts.append(s); n += 1
    if not n:
        return 0.0, 0, 0
    return tot / n, int(statistics.median(shifts)), n


def cmd_abppu(exe, rom, nesref_exe, core, frames):
    """A-vs-B PPU-mem: recomp OAM+palette vs Mesen's (extracted in-process from
    nesref's retro_serialize blob). Closes the PPU-internal gap the libretro
    memory API can't (VIDEO_RAM is null on all NES cores). Image layout matches
    NESRECOMP_PPUMEM_TRACE: [0x000-0x0FF]=OAM, [0x100-0x11F]=palette."""
    print(f"[A-vs-B PPU] recomp OAM+palette vs Mesen/nesref (in-process, {frames} frames)")
    with tempfile.TemporaryDirectory() as d:
        rc_path = os.path.join(d, "rc_ppu.jsonl")
        env = dict(os.environ); env["NESRECOMP_PPUMEM_TRACE"] = rc_path
        clear_saves(exe)
        exe_abs = os.path.abspath(exe)
        # Capture the recomp boot banner to detect CHR-RAM: CHR-RAM (0 CHR banks)
        # is serialized inline in Mesen's savestate, pushing the nametable region
        # +0x2000 in the blob vs CHR-ROM games. Auto-set NESREF_PPU_NT_D so abppu
        # is correct-by-default on both (no manual override).
        p = subprocess.run([exe_abs, os.path.abspath(rom), *recomp_drive_args(frames)],
                           env=env, cwd=os.path.dirname(exe_abs),
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        import re
        m = re.search(r"(\d+)\s*CHR banks", p.stdout or "")
        chr_ram = (m is not None and int(m.group(1)) == 0)
        nr_path = os.path.join(d, "nr_ppu.jsonl")
        env2 = dict(os.environ)
        env2["NESREF_FRAMES"] = str(frames); env2["NESREF_PPU_FILE"] = nr_path
        env2["NESREF_TRACE_FILE"] = os.path.join(d, "nr_ram.jsonl")
        nesref_script_env(env2)
        if chr_ram and "NESREF_PPU_NT_D" not in os.environ:
            env2["NESREF_PPU_NT_D"] = "-0x2ab4"   # CHR-RAM NT offset
            print("  (CHR-RAM detected -> NT offset +0x2000)")
        nr_abs = os.path.abspath(nesref_exe)
        subprocess.run([nr_abs, os.path.abspath(core), os.path.abspath(rom)],
                       env=env2, cwd=os.path.dirname(nr_abs),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        rc = reconstruct_image(rc_path, 0xA00)
        nr = reconstruct_image(nr_path, 0xA00)
        if not rc or not nr:
            print(f"  FAIL: empty (recomp {len(rc)}, nesref {len(nr)})")
            return 1
        print(f"  recomp {len(rc)} change-frames, nesref {len(nr)} change-frames")

        # Traces are sparse (a row only when OAM/palette changed), so index the
        # image AS-OF a frame = the last snapshot at or before it.
        rc_keys, nr_keys = sorted(rc), sorted(nr)
        import bisect
        def asof(d, keys, f):
            i = bisect.bisect_right(keys, f) - 1
            return d[keys[i]] if i >= 0 else None
        # Palette mirror slots: recomp stores 0 at $3F10/14/18/1C (+backdrop $3F00
        # mirror class); Mesen stores real values. Exclude them from palette match.
        PAL_MIRROR = {0x100, 0x110, 0x114, 0x118, 0x11c}

        def region_match(a, b, lo, hi, skip=()):
            idx = [i for i in range(lo, hi) if i not in skip]
            same = sum(1 for i in idx if a[i] == b[i])
            return same / len(idx)
        # Boot-offset search maximizing OAM agreement (the discriminating region).
        sample = rc_keys[len(rc_keys)//3: 2*len(rc_keys)//3]
        best_off, best_m = 0, -1.0
        for off in range(0, 16):
            ms, n = 0.0, 0
            for f in sample:
                b = asof(nr, nr_keys, f + off)
                if b is not None:
                    ms += region_match(rc[f], b, 0, 0x100); n += 1
            if n and ms / n > best_m:
                best_m, best_off = ms / n, off
        # Report OAM, (mirror-normalized) palette, and nametable agreement.
        oam_tot = pal_tot = nt_tot = 0.0; n = 0
        for f in rc_keys:
            b = asof(nr, nr_keys, f + best_off)
            if b is not None:
                oam_tot += region_match(rc[f], b, 0x000, 0x100)
                pal_tot += region_match(rc[f], b, 0x100, 0x120, skip=PAL_MIRROR)
                nt_tot  += region_match(rc[f], b, 0x200, 0xA00)
                n += 1
        print(f"  best boot offset = +{best_off}")
        print(f"  OAM       match = {oam_tot/n*100:.2f}%   (256 bytes/frame, {n} frames)")
        print(f"  palette   match = {pal_tot/n*100:.2f}%   (27 non-mirror bytes/frame; $3F10/14/18/1C excluded)")
        print(f"  nametable match = {nt_tot/n*100:.2f}%   (2KB/frame)")
        if oam_tot/n > 0.90:
            print(f"  => offset VALIDATED: Mesen OAM extracted from the serialize blob agrees with the recomp.")
        else:
            print(f"  => LOW OAM agreement: extraction offset may be wrong, or a real PPU divergence. Investigate.")
        # Adaptive (piecewise) offset — the abppu twin of abram's. A single boot
        # offset can't track a mid-run frame-count desync (Gumshoe NMI-off), which
        # drags the whole-run NT/OAM verdict down even when the state matches once
        # re-aligned. Re-check with a windowed offset when the fixed pass looks
        # divergent: if a discrete shift recovers a high match it was alignment;
        # if it stays low it is a real PPU divergence / FrameCounter-phase.
        OAM_OK, NT_OK = 0.90, 0.95   # OAM never hits 100% (sprite-eval timing phase)
        if oam_tot/n < OAM_OK or nt_tot/n < NT_OK:
            a_oam, a_pal, a_nt, resyncs = adaptive_offset_ppu(rc, nr, rc_keys, nr_keys)
            print(f"  adaptive-offset match: OAM {a_oam*100:.2f}%  palette {a_pal*100:.2f}%  "
                  f"nametable {a_nt*100:.2f}%  ({len(resyncs)} offset shift(s))")
            for fr, o0, o1 in resyncs[:6]:
                print(f"    offset shift @ frame {fr}: +{o0} -> +{o1}  (NMI-off frame-count desync)")
            if a_oam > OAM_OK and a_nt > NT_OK:
                tail = " once re-aligned (fixed-offset spread was NMI-off frame-count desync)" if resyncs else ""
                print(f"  => CONVERGED{tail}: PPU state matches.")
            elif a_oam > OAM_OK:
                # OAM/palette converge but NT doesn't and no frame offset helps.
                # Before calling it a divergence, test scroll-phase: attract demos
                # scroll, so a demo-timing phase shows up as a horizontal column
                # shift the frame offset can't undo.
                sp_m, sp_shift, sp_n = scroll_phase_match(rc, nr, rc_keys, nr_keys, best_off)
                print(f"  scroll-phase NT match = {sp_m*100:.2f}%  (median shift {sp_shift} cols, {sp_n} frames)")
                if sp_m > NT_OK:
                    print(f"  => CONVERGED modulo attract scroll-phase: OAM/palette match and the NT "
                          f"recovers under a per-frame horizontal scroll shift => correct rendering, off "
                          f"only by demo timing (the NMI-off frame-count desync), not a rendering bug.")
                else:
                    print(f"  => still NT {sp_m*100:.1f}% under scroll-shift => genuine PPU/rendering "
                          f"divergence to chase.")
            else:
                print(f"  => still OAM {a_oam*100:.1f}% / NT {a_nt*100:.1f}% after re-align => genuine PPU "
                      f"divergence or FrameCounter/host-state phase (not fixable by offset).")
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


def cmd_abstate(exe, rom, nesref_exe, core, frames):
    """SINGLE-POINT STATE DIFF at a script anchor. Drive BOTH engines (COSIM_SCRIPT,
    which should end at a WAIT_RAM8 game-state anchor + short settle) and diff their
    FINAL reconstructed RAM + OAM + palette + nametable. No frame alignment: both
    are anchored to the SAME game state, so any byte difference is a real
    divergence (benign-masked). This sidesteps the load-timing frame drift that
    made the frame-by-frame abram/abppu report false convergence on input-driven
    runs. Prereq: COSIM_SCRIPT set."""
    if not cosim_script():
        print("  abstate needs a drive script arg (trailing) ending at a WAIT_RAM8 anchor")
        return 2
    print(f"[A-vs-B STATE] single-point diff at script anchor (up to {frames} frames)")
    import re
    benign = parse_benign("0x01,0x10,0x15,0x69,0x781-0x789")
    PALMIRROR = {0x110, 0x114, 0x118, 0x11c}   # $3F10/14/18/1C mirror $3F00/...
    with tempfile.TemporaryDirectory() as d:
        rc_ram = os.path.join(d, "rc_ram.jsonl"); rc_ppu = os.path.join(d, "rc_ppu.jsonl")
        env = dict(os.environ)
        env["NESRECOMP_WRAM_TRACE"] = rc_ram; env["NESRECOMP_PPUMEM_TRACE"] = rc_ppu
        env.pop("NESRECOMP_COSIM_INJECT", None); clear_saves(exe)
        exe_abs = os.path.abspath(exe)
        p = subprocess.run([exe_abs, os.path.abspath(rom), *recomp_drive_args(frames)],
                           env=env, cwd=os.path.dirname(exe_abs),
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        nr_ram = os.path.join(d, "nr_ram.jsonl"); nr_ppu = os.path.join(d, "nr_ppu.jsonl")
        env2 = dict(os.environ)
        env2["NESREF_FRAMES"] = str(frames)
        env2["NESREF_TRACE_FILE"] = nr_ram; env2["NESREF_PPU_FILE"] = nr_ppu
        nesref_script_env(env2)
        # Mesen serialize-blob nametable offset. NROM default (-0xab4) is wrong for
        # MMC3: its mapper state pushes the NT +0x2000 (empirically calibrated on
        # SMB3 = -0x2ab4, same as the CHR-RAM layout). Auto-detect mapper 4 from the
        # recomp boot banner; env override (COSIM_NT_D) wins.
        nt_d = os.environ.get("COSIM_NT_D")
        if not nt_d:
            m4 = re.search(r"Mapper\s*4\b", p.stdout or "")
            if m4:
                nt_d = "-0x2ab4"
        if nt_d:
            env2["NESREF_PPU_NT_D"] = nt_d
            print(f"  (NT blob-offset override NESREF_PPU_NT_D={nt_d})")
        nr_abs = os.path.abspath(nesref_exe)
        subprocess.run([nr_abs, os.path.abspath(core), os.path.abspath(rom)],
                       env=env2, cwd=os.path.dirname(nr_abs),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        rc_r = reconstruct_ram(rc_ram); nr_r = reconstruct_ram(nr_ram)
        rc_p = reconstruct_image(rc_ppu, 0xA00); nr_p = reconstruct_image(nr_ppu, 0xA00)
        if not rc_r or not nr_r or not rc_p or not nr_p:
            print(f"  FAIL: empty trace (rc_ram {len(rc_r)}, nr_ram {len(nr_r)}, "
                  f"rc_ppu {len(rc_p)}, nr_ppu {len(nr_p)})")
            return 1
        RA = rc_r[max(rc_r)]; NA = nr_r[max(nr_r)]
        RP = rc_p[max(rc_p)]; NP = nr_p[max(nr_p)]
        print(f"  recomp final RAM@f{max(rc_r)} PPU@f{max(rc_p)}; "
              f"nesref final RAM@f{max(nr_r)} PPU@f{max(nr_p)}")

        ram_d = [i for i in range(2048)
                 if not (0x100 <= i <= 0x1FF) and i not in benign and RA[i] != NA[i]]
        oam_d = [i for i in range(0x000, 0x100) if RP[i] != NP[i]]
        pal_d = [i for i in range(0x100, 0x120) if i not in PALMIRROR and RP[i] != NP[i]]
        nt_d  = [i for i in range(0x200, 0xA00) if RP[i] != NP[i]]
        print(f"  final-state diffs: RAM {len(ram_d)} real, OAM {len(oam_d)}/256, "
              f"palette {len(pal_d)}/28, nametable {len(nt_d)}/2048")
        zp = [i for i in ram_d if i < 0x100]
        p2 = [i for i in ram_d if 0x200 <= i < 0x300]
        other = [i for i in ram_d if i not in zp and i not in p2]
        print(f"    RAM ZP({len(zp)}): " + " ".join(f"${i:02x}" for i in zp))
        print(f"    RAM page2/shadowOAM({len(p2)}): " + " ".join(f"${i:03x}" for i in p2))
        print(f"    RAM other({len(other)}): " + " ".join(f"${i:04x}" for i in other))
        print(f"    OAM diff offsets: " + " ".join(f"${i:03x}" for i in oam_d))
        for i in zp + other:
            print(f"    RAM ${i:04x}: recomp={RA[i]:#04x} mesen={NA[i]:#04x}")
        # Decode every sprite touched by an OAM diff, both engines side by side.
        for s in sorted({i // 4 for i in oam_d}):
            r = RP[s*4:s*4+4]; n = NP[s*4:s*4+4]
            print(f"    OAM spr{s:02d}: recomp Y={r[0]:02x} T={r[1]:02x} A={r[2]:02x} X={r[3]:02x}"
                  f"  | mesen Y={n[0]:02x} T={n[1]:02x} A={n[2]:02x} X={n[3]:02x}")
        for s in sorted({(i - 0x200) // 4 for i in p2}):
            r = RA[0x200+s*4:0x200+s*4+4]; n = NA[0x200+s*4:0x200+s*4+4]
            print(f"    SHDW spr{s:02d}: recomp Y={r[0]:02x} T={r[1]:02x} A={r[2]:02x} X={r[3]:02x}"
                  f"  | mesen Y={n[0]:02x} T={n[1]:02x} A={n[2]:02x} X={n[3]:02x}")
        for i in pal_d[:12]:
            print(f"    PAL  $3f{i-0x100:02x}: recomp={RP[i]:#04x} mesen={NP[i]:#04x}")
        real = bool(ram_d or oam_d or pal_d) or len(nt_d) > 64
        if real:
            print(f"  ***DIVERGED*** at the anchored game state — REAL divergence "
                  f"(both engines confirmed at the same WAIT_RAM8 anchor).")
            return 1
        print(f"  => CONVERGED at anchored state (RAM/OAM/palette identical, "
              f"NT within {len(nt_d)} bytes).")
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
        if len(sys.argv) > 7:
            globals()["COSIM_SCRIPT_PATH"] = sys.argv[7]
        return cmd_abram(exe, rom, nesref_exe, core, frames)
    if cmd == "abcycle":
        exe, rom, nesref_exe, core = sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]
        frames = int(sys.argv[6]) if len(sys.argv) > 6 else 900
        return cmd_abcycle(exe, rom, nesref_exe, core, frames)
    if cmd == "abppu":
        exe, rom, nesref_exe, core = sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]
        frames = int(sys.argv[6]) if len(sys.argv) > 6 else 900
        return cmd_abppu(exe, rom, nesref_exe, core, frames)
    if cmd == "abstate":
        exe, rom, nesref_exe, core = sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]
        frames = int(sys.argv[6]) if len(sys.argv) > 6 else 3000
        if len(sys.argv) > 7:
            globals()["COSIM_SCRIPT_PATH"] = sys.argv[7]
        return cmd_abstate(exe, rom, nesref_exe, core, frames)
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
