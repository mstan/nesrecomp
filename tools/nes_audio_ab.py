#!/usr/bin/env python3
"""nes_audio_ab.py - drift-tolerant A/B comparison of recomp NES audio vs an
accuracy-grade emulator reference (Mesen via tools/nesref NESREF_WAV).

NES port of snesrecomp tools/audio_ab_diff.py + cosim/dspout_compare.py, and a
reimplementation of the lost accuracy-burndown audio-slice analyzer (the
"timbre L1" metric — see NES_ACCURACY_BURNDOWN.md Axis 5b; committed this time).

WHY drift-tolerant (not bit-exact): the recomp emits one 735-sample frame per
NMI at the video-frame cadence, while Mesen resamples its cycle-exact APU to
48000 Hz on its own clock. Even a perfect APU will phase-drift against the
oracle at the ppm level, so a sample-exact diff is meaningless. What matters is
whether the *music* matches: same notes, same onsets, same pitch, same timbre,
with bounded time drift. This tool measures exactly those.

Inputs are two WAVs at (possibly) different native rates:
  REF : tools/nesref  NESREF_WAV=oracle.wav  (Mesen core, 48000 Hz stereo;
        headless NESREF_FRAMES runs are deterministic + faster than realtime)
  TEST: the recomp's t1_apu.wav from RECOMP_AUDIO_DEBUG=<dir> +
        RECOMP_AUDIO_DEBUG_DUMP_SECS=<n>  (44100 Hz mono, pre-volume APU mix)

Metrics (all numpy-only):
  1. global alignment  - FFT cross-correlation -> best lag + peak correlation
  2. drift             - per-window local lag slope (ms/s and ppm): "off-cue"
  3. onset timing      - spectral-flux onsets matched across streams (median /
                         IQR / p90 ms + match rate): rhythm fidelity
  4. pitch             - (a) dominant-pitch-track median ratio in cents and
                         (b) ALIGNMENT-FREE log-frequency PSD cross-correlation
                         (a uniform pitch offset is a constant shift in log-f;
                         pass = |cents| < 3, inaudible): "off-tune"
  5. timbre            - band-energy L1: third-octave band energies of the
                         time-averaged spectrum, each stream normalized to
                         sum 1, L1 distance. Alignment- and level-invariant.
                         This is the burndown's headline metric (0.083 on SMB
                         with a self-floor ~0.04-0.06). Plus log-spectral
                         distance (dB) and spectral-centroid delta (Hz).
  6. quality           - click rate + noise floor per stream (round-2
                         detectors; catches per-stream artifacts an A/B miss).

Metric caveats (inherited from the SNES campaign — measured, not guessed):
  - raw global xcorr can be low even when perceptually identical once any
    drift exists; trust onsets + spectrum, not the correlation number.
  - dominant-pitch tracking is unreliable for polyphony; the log-f PSD cents
    is the robust pitch verdict.
  - always compare against a SELF-FLOOR (two independent recomp captures of
    the same window) before reading meaning into an absolute number.

Usage:
    python nes_audio_ab.py --ref oracle.wav --test t1_apu.wav
                           [--rate 44100] [--start-s 0] [--dur-s 0]
                           [--max-lag-s 3] [--json out.json] [--label X]
Import API (used by nes_cosim.py abaudio):
    compare(ref_path, test_path, rate=44100, start_s=0, dur_s=0) -> dict
"""
import sys, os, wave, argparse, json
import numpy as np


# ----------------------------------------------------------------------------- io
def load_wav(path):
    """Robust WAV loader: tolerates an UNFINALIZED header (data-size==0 or
    wrong) from a force-killed capture by taking all bytes after the data
    chunk start. Falls back to raw parse if stdlib wave rejects the header."""
    try:
        with wave.open(path, "rb") as w:
            ch, rate, n = w.getnchannels(), w.getframerate(), w.getnframes()
            raw = w.readframes(n)
        if len(raw) >= (n * ch * 2) and n > 0:
            x = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
            return (x.reshape(-1, ch).mean(axis=1) if ch > 1 else x), rate
    except Exception:
        pass
    b = open(path, "rb").read()
    assert b[:4] == b"RIFF" and b[8:12] == b"WAVE", "not a WAV"
    ch, rate, bits, off = 2, 44100, 16, 12
    data_off, data_len = None, 0
    while off + 8 <= len(b):
        cid = b[off:off + 4]; sz = int.from_bytes(b[off + 4:off + 8], "little"); body = off + 8
        if cid == b"fmt ":
            ch = int.from_bytes(b[body + 2:body + 4], "little")
            rate = int.from_bytes(b[body + 4:body + 8], "little")
            bits = int.from_bytes(b[body + 14:body + 16], "little")
        elif cid == b"data":
            data_off = body
            data_len = sz if 0 < sz <= len(b) - body else (len(b) - body)
            break
        off = body + sz + (sz & 1)
    raw = b[data_off:data_off + (data_len - data_len % (ch * (bits // 8)))]
    x = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    return (x.reshape(-1, ch).mean(axis=1) if ch > 1 else x), rate


def resample_linear(x, src, dst):
    if src == dst or x.size == 0:
        return x
    n_out = int(round(x.size * dst / src))
    t = np.arange(n_out) * (src / dst)
    i0 = np.clip(np.floor(t).astype(int), 0, x.size - 2)
    frac = t - i0
    return x[i0] * (1.0 - frac) + x[i0 + 1] * frac


def dc_block(x, rate, win_ms=25.0):
    """Remove DC / infrasonic pedestal with a moving-average high-pass.

    The recomp's t1 tap is the raw nonlinear-DAC mix, which is UNIPOLAR (the
    NES DAC only outputs positive levels), and idle channels park at nonzero
    levels — so t1 rides a stepped DC pedestal. Mesen DC-blocks internally.
    Comparing without removing DC skews the RMS level match, the noise-floor
    stat, and (through level normalization) every spectral metric. Analysis
    bands start at 50 Hz; this HP is -3 dB well below that."""
    w = max(1, int(rate * win_ms / 1000.0))
    cs = np.cumsum(np.concatenate([[0.0], x]))
    idx = np.arange(x.size)
    lo = np.maximum(0, idx - w); hi = np.minimum(x.size, idx + w + 1)
    return x - (cs[hi] - cs[lo]) / np.maximum(1, hi - lo)


def active_region(x, thr=1e-4):
    nz = np.where(np.abs(x) > thr)[0]
    if nz.size == 0:
        return 0, x.size
    return int(nz[0]), int(nz[-1]) + 1


# --------------------------------------------------------------------- alignment
def best_lag(a, b, max_lag):
    n = 1 << int(np.ceil(np.log2(a.size + b.size)))
    fa = np.fft.rfft(a - a.mean(), n)
    fb = np.fft.rfft(b - b.mean(), n)
    cc = np.fft.irfft(fa * np.conj(fb), n)
    cc = np.concatenate([cc[-max_lag:], cc[: max_lag + 1]])
    lags = np.arange(-max_lag, max_lag + 1)
    k = int(np.argmax(cc))
    denom = (np.linalg.norm(a) * np.linalg.norm(b)) or 1.0
    return int(lags[k]), float(cc[k] / denom)


def drift_track(a, b, rate, win_s=0.75, hop_s=0.375, search_ms=120):
    win = int(win_s * rate); hop = int(hop_s * rate); ml = int(search_ms * rate / 1000)
    n = min(a.size, b.size)
    centers, lags, corrs = [], [], []
    pos = 0
    while pos + win <= n:
        aw = a[pos:pos + win]; bw = b[pos:pos + win]
        if np.linalg.norm(aw) > 1e-3 and np.linalg.norm(bw) > 1e-3:
            lag, c = best_lag(aw, bw, ml)
            centers.append((pos + win / 2) / rate); lags.append(lag / rate * 1000.0); corrs.append(c)
        pos += hop
    if len(centers) < 3:
        return None
    centers = np.array(centers); lags = np.array(lags); corrs = np.array(corrs)
    wts = np.clip(corrs, 0, None)
    if wts.sum() < 1e-6:
        wts = np.ones_like(corrs)
    A = np.vstack([centers, np.ones_like(centers)]).T
    W = np.diag(wts)
    coef = np.linalg.lstsq(W @ A, W @ lags, rcond=None)[0]
    slope = float(coef[0])
    return {
        "n_windows": len(centers),
        "lag_ms_mean": float(lags.mean()),
        "lag_ms_std": float(lags.std()),
        "drift_ms_per_s": slope,
        "drift_ppm": slope * 1000.0,
        "local_corr_median": float(np.median(corrs)),
    }


# ------------------------------------------------------------------------ onsets
def onsets(x, rate, fft=1024, hop=256, k=1.5):
    if x.size < fft:
        return np.array([])
    win = np.hanning(fft)
    frames = 1 + (x.size - fft) // hop
    mag = np.empty((frames, fft // 2 + 1))
    for i in range(frames):
        mag[i] = np.abs(np.fft.rfft(x[i * hop:i * hop + fft] * win))
    flux = np.maximum(0.0, np.diff(mag, axis=0)).sum(axis=1)
    if flux.size == 0:
        return np.array([])
    flux /= (flux.max() or 1.0)
    w = 16
    thr = np.array([flux[max(0, i - w):i + w].mean() + k * flux[max(0, i - w):i + w].std()
                    for i in range(flux.size)])
    pk = (flux[1:-1] > flux[:-2]) & (flux[1:-1] >= flux[2:]) & (flux[1:-1] > thr[1:-1])
    return (np.where(pk)[0] + 1) * hop / rate


def onset_match(ref_t, test_t, tol_ms=50):
    if ref_t.size == 0 or test_t.size == 0:
        return None
    errs, matched = [], 0
    for t in test_t:
        d = np.abs(ref_t - t)
        j = int(np.argmin(d))
        if d[j] * 1000.0 <= tol_ms:
            errs.append((t - ref_t[j]) * 1000.0); matched += 1
    if not errs:
        return {"matched": 0, "ref_onsets": int(ref_t.size), "test_onsets": int(test_t.size)}
    errs = np.array(errs)
    return {
        "ref_onsets": int(ref_t.size), "test_onsets": int(test_t.size),
        "matched": matched,
        "match_rate": matched / max(1, test_t.size),
        "err_ms_median": float(np.median(errs)),
        "err_ms_abs_median": float(np.median(np.abs(errs))),
        "err_ms_iqr": float(np.percentile(errs, 75) - np.percentile(errs, 25)),
        "err_ms_p90": float(np.percentile(np.abs(errs), 90)),
    }


# ---------------------------------------------------------------- pitch & timbre
def avg_spectrum(x, rate, fft=4096, hop=2048):
    if x.size < fft:
        return None, None
    win = np.hanning(fft); frames = 1 + (x.size - fft) // hop
    acc = np.zeros(fft // 2 + 1)
    for i in range(frames):
        acc += np.abs(np.fft.rfft(x[i * hop:i * hop + fft] * win))
    return np.fft.rfftfreq(fft, 1.0 / rate), acc / frames


def dominant_pitch_track(x, rate, fft=4096, hop=2048, fmin=80, fmax=2000):
    if x.size < fft:
        return np.array([])
    win = np.hanning(fft); frames = 1 + (x.size - fft) // hop
    freqs = np.fft.rfftfreq(fft, 1.0 / rate)
    band = (freqs >= fmin) & (freqs <= fmax)
    out = []
    for i in range(frames):
        sp = np.abs(np.fft.rfft(x[i * hop:i * hop + fft] * win))
        if sp[band].max() < 1e-3:
            continue
        out.append(freqs[band][int(np.argmax(sp[band]))])
    return np.array(out)


def welch_psd(x, rate, nperseg=8192):
    """Minimal Welch PSD (Hann, 50% overlap) — numpy-only."""
    if x.size < nperseg:
        nperseg = max(1024, 1 << int(np.log2(max(2, x.size))))
    step = nperseg // 2
    win = np.hanning(nperseg)
    acc, n = np.zeros(nperseg // 2 + 1), 0
    for pos in range(0, x.size - nperseg + 1, step):
        seg = x[pos:pos + nperseg] * win
        acc += np.abs(np.fft.rfft(seg)) ** 2
        n += 1
    if n == 0:
        return None, None
    return np.fft.rfftfreq(nperseg, 1.0 / rate), acc / n


def logf_pitch_offset_cents(ref, test, rate, npts=4096, fmin=40.0, fmax=None):
    """Alignment-free pitch verdict (port of snesrecomp dspout_compare.py):
    a uniform pitch offset scales every frequency by a constant ratio = a
    constant SHIFT in log-frequency. Cross-correlate the two log-f PSDs; the
    peak shift gives the ratio directly. Positive cents = test is SHARP."""
    if fmax is None:
        fmax = rate / 2.0
    f, pr = welch_psd(ref, rate)
    _, pt = welch_psd(test, rate)
    if pr is None or pt is None:
        return None
    lg = np.geomspace(fmin, fmax, npts)
    a = np.interp(lg, f, 10 * np.log10(np.maximum(pr, 1e-20)))
    b = np.interp(lg, f, 10 * np.log10(np.maximum(pt, 1e-20)))
    a -= a.mean(); b -= b.mean()
    n = 1 << int(np.ceil(np.log2(2 * npts)))
    corr = np.fft.irfft(np.fft.rfft(a, n) * np.conj(np.fft.rfft(b, n)), n)
    corr = np.concatenate([corr[-(npts - 1):], corr[:npts]])
    lags = np.arange(-(npts - 1), npts)
    dlog = np.log(lg[-1] / lg[0]) / (npts - 1)
    max_bins = int(np.log(2.0) / dlog)          # limit to +/- 1 octave
    m = np.abs(lags) <= max_bins
    cm, lm = corr[m], lags[m]
    k = int(np.argmax(cm))
    norm = np.sqrt(np.sum(a * a) * np.sum(b * b)) + 1e-9
    # corr(a,b)[lag] peaks where b shifted by +lag matches a; b at HIGHER
    # log-f than a (test sharp) shows up as a positive lag here.
    shift = int(lm[k])
    ratio = float(np.exp(shift * dlog))
    return {
        "shift_bins": shift,
        "peak": float(cm[k] / norm),
        "zero_shift_sim": float(corr[lags == 0][0] / norm),
        "ratio_test_over_ref": ratio,
        "cents": float(1200.0 * np.log2(ratio)),
    }


def third_octave_l1(fr, sr_, ft, st_, fmin=50.0, fmax=15000.0):
    """Band-energy timbre L1: third-octave band energies of the time-averaged
    spectra, each normalized to sum 1, L1-distance. Alignment- and
    level-invariant. Headline metric of the accuracy-burndown audio slice."""
    edges = [fmin]
    while edges[-1] < fmax:
        edges.append(edges[-1] * (2.0 ** (1.0 / 3.0)))
    edges = np.array(edges)
    def bands(f, s):
        e = np.zeros(edges.size - 1)
        for i in range(edges.size - 1):
            m = (f >= edges[i]) & (f < edges[i + 1])
            e[i] = (s[m] ** 2).sum()
        t = e.sum()
        return e / t if t > 0 else e
    br = bands(fr, sr_); bt = bands(ft, st_)
    return float(np.abs(br - bt).sum()), edges, br, bt


# ----------------------------------------------------------- per-stream quality
def click_rate(x, rate, k=6.0, rms_ms=10.0):
    if x.size < 4:
        return 0.0
    d1 = np.abs(np.diff(x, prepend=x[0])); d2 = np.abs(np.diff(x, n=2, prepend=[x[0], x[0]]))
    w = max(8, int(rate * rms_ms / 1000.0))
    cs = np.cumsum(np.concatenate([[0.0], x * x]))
    lo = np.maximum(0, np.arange(x.size) - w); hi = np.minimum(x.size, np.arange(x.size) + w)
    rms = np.sqrt((cs[hi] - cs[lo]) / np.maximum(1, hi - lo))
    score = np.maximum(d1, d2) / np.maximum(rms, 1e-4)
    idx = np.where(score > k)[0]
    if idx.size:
        keep = [idx[0]]
        for i in idx[1:]:
            if i - keep[-1] >= 3:
                keep.append(i)
        idx = np.array(keep)
    return idx.size / (x.size / rate)


def noise_floor_db(x):
    if x.size == 0:
        return float("nan")
    q = np.sqrt(np.mean(np.sort(x * x)[: max(1, x.size // 10)]))
    return 20.0 * np.log10(q + 1e-12)


# --------------------------------------------------------------------------- api
def compare(ref_path, test_path, rate=0, start_s=0.0, dur_s=0.0, max_lag_s=3.0):
    xr, rr = load_wav(ref_path); xt, rt = load_wav(test_path)
    # Default analysis rate = the HIGHER of the two native rates, so the
    # common-rate conversion only ever UPsamples (lossless below the source
    # Nyquist). Downsampling one stream but not the other imposes an
    # asymmetric high-frequency droop that reads as a phantom timbre/centroid
    # tilt — the same measurement artifact the SNES campaign root-caused
    # (~+400 Hz phantom centroid here at a forced 44100 vs a 48000 oracle).
    if not rate:
        rate = max(rr, rt)
    xr = resample_linear(xr, rr, rate); xt = resample_linear(xt, rt, rate)
    xr = dc_block(xr, rate); xt = dc_block(xt, rate)
    r0, r1 = active_region(xr); t0, t1 = active_region(xt)
    xr = xr[r0:r1]; xt = xt[t0:t1]
    if start_s or dur_s:
        s = int(start_s * rate); e = xr.size if dur_s == 0 else s + int(dur_s * rate)
        xr = xr[s:e]; xt = xt[s:min(e, xt.size)]
    n = min(xr.size, xt.size)
    xr = xr[:n]; xt = xt[:n]
    if n < rate:
        raise RuntimeError("<1s of overlapping active audio; check captures")

    # level-normalize the test to the ref RMS so LSD reads shape, not volume
    rms_r = np.sqrt(np.mean(xr * xr)); rms_t = np.sqrt(np.mean(xt * xt))
    lvl_db = 20.0 * np.log10((rms_t + 1e-12) / (rms_r + 1e-12))
    if rms_t > 1e-9:
        xt = xt * (rms_r / rms_t)

    ml = int(max_lag_s * rate)
    glag, gcorr = best_lag(xr, xt, ml)
    if glag >= 0:
        xa, xb = xr[glag:], xt[: n - glag]
    else:
        xa, xb = xr[: n + glag], xt[-glag:]
    m = min(xa.size, xb.size); xa = xa[:m]; xb = xb[:m]

    drift = drift_track(xa, xb, rate)
    onset = onset_match(onsets(xa, rate), onsets(xb, rate))

    fr, sr_ = avg_spectrum(xa, rate); ft, st_ = avg_spectrum(xb, rate)
    timbre = {}
    if sr_ is not None and st_ is not None:
        l1, _, _, _ = third_octave_l1(fr, sr_, ft, st_)
        timbre["band_l1"] = l1
        lo = (fr >= 50) & (fr <= 15000)
        ls = np.log(sr_[lo] + 1e-9); lt = np.log(st_[lo] + 1e-9)
        timbre["log_spectral_dist_db"] = float(np.sqrt(np.mean((ls - lt) ** 2)) * (20 / np.log(10)))
        cen = lambda s: float((fr[lo] * s[lo]).sum() / (s[lo].sum() + 1e-9))
        timbre["centroid_ref_hz"] = cen(sr_); timbre["centroid_test_hz"] = cen(st_)
        timbre["centroid_delta_hz"] = timbre["centroid_test_hz"] - timbre["centroid_ref_hz"]

    pr = dominant_pitch_track(xa, rate); pt = dominant_pitch_track(xb, rate)
    pitch = {}
    if pr.size and pt.size:
        ratio = np.median(pt) / np.median(pr)
        pitch["dominant_track_cents"] = float(1200.0 * np.log2(ratio))
    logf = logf_pitch_offset_cents(xa, xb, rate)
    if logf:
        pitch["logf_psd"] = logf

    return {
        "ref": os.path.basename(ref_path), "test": os.path.basename(test_path),
        "rate": rate, "overlap_s": round(m / rate, 2),
        "level_delta_db": lvl_db,
        "global_align": {"lag_ms": glag / rate * 1000.0, "peak_corr": gcorr},
        "drift": drift, "onset": onset, "pitch": pitch, "timbre": timbre,
        "quality": {
            "ref":  {"click_per_s": click_rate(xa, rate), "noise_floor_db": noise_floor_db(xa)},
            "test": {"click_per_s": click_rate(xb, rate), "noise_floor_db": noise_floor_db(xb)},
        },
    }


def print_report(r, label=""):
    print(f"\n=== A/B audio diff{(' [' + label + ']') if label else ''}: "
          f"{r['test']}  vs  REF {r['ref']} ===")
    print(f"  overlap {r['overlap_s']}s @ {r['rate']} Hz; test level {r['level_delta_db']:+.1f} dB vs ref")
    ga = r["global_align"]
    print(f"  global align : lag {ga['lag_ms']:+.1f} ms, peak corr {ga['peak_corr']:.3f} "
          f"({'STRONG' if ga['peak_corr'] > 0.5 else 'weak (expected under drift; trust onsets/spectrum)'})")
    d = r["drift"]
    if d:
        print(f"  drift        : {d['drift_ms_per_s']:+.2f} ms/s ({d['drift_ppm']:+.0f} ppm), "
              f"lag std {d['lag_ms_std']:.1f} ms, local corr {d['local_corr_median']:.2f}")
    o = r["onset"]
    if o:
        if o.get("matched"):
            print(f"  onsets       : {o['matched']}/{o['test_onsets']} matched "
                  f"({o['match_rate'] * 100:.0f}%), |err| median {o['err_ms_abs_median']:.1f} ms, "
                  f"p90 {o['err_ms_p90']:.1f} ms")
        else:
            print(f"  onsets       : 0 matched (ref {o['ref_onsets']}, test {o['test_onsets']})")
    p = r["pitch"]
    if p.get("logf_psd"):
        lf = p["logf_psd"]
        verdict = ("MATCHES (inaudible)" if abs(lf["cents"]) < 3 else
                   ("test SHARP" if lf["cents"] > 0 else "test FLAT"))
        print(f"  pitch (logf) : {lf['cents']:+.2f} cents, {verdict} "
              f"(peak {lf['peak']:.3f}, zero-shift sim {lf['zero_shift_sim']:.3f})")
    if "dominant_track_cents" in p:
        print(f"  pitch (track): {p['dominant_track_cents']:+.1f} cents (polyphony-unreliable)")
    t = r["timbre"]
    if t:
        print(f"  timbre       : band L1 {t['band_l1']:.3f}, log-spectral dist "
              f"{t['log_spectral_dist_db']:.1f} dB, centroid delta {t['centroid_delta_hz']:+.0f} Hz")
    q = r["quality"]
    print(f"  quality      : clicks ref {q['ref']['click_per_s']:.2f}/s test {q['test']['click_per_s']:.2f}/s; "
          f"noise floor ref {q['ref']['noise_floor_db']:.0f} dB test {q['test']['noise_floor_db']:.0f} dB")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", required=True)
    ap.add_argument("--test", required=True)
    ap.add_argument("--rate", type=int, default=0,
                    help="common analysis rate; 0 = max of the two native rates")
    ap.add_argument("--start-s", type=float, default=0.0)
    ap.add_argument("--dur-s", type=float, default=0.0)
    ap.add_argument("--max-lag-s", type=float, default=3.0)
    ap.add_argument("--json", default=None)
    ap.add_argument("--label", default="")
    a = ap.parse_args()
    r = compare(a.ref, a.test, a.rate, a.start_s, a.dur_s, a.max_lag_s)
    print_report(r, a.label)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(r, f, indent=2)
        print(f"  wrote {a.json}")


if __name__ == "__main__":
    main()
