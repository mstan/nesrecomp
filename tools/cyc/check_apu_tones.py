#!/usr/bin/env python3
"""
check_apu_tones.py - check a recording of make_apu_tone_rom.py's program
against the APU's documented behavior (nesdev wiki: APU Pulse, Triangle,
Noise, DMC, Envelope, Length Counter).

This is a specification check, not a reference comparison: it confirms the
pitch of each channel from its period register, that noise is broadband, that
the envelope decays over the documented time and that the length counter
silences the channel on time.

  python check_apu_tones.py tones.wav
"""
import sys
import wave

import numpy as np

CPU_HZ = 21477272.0 / 12.0
FRAME = 1.0 / 60.0988


def load(path):
    with wave.open(path, 'rb') as w:
        rate = w.getframerate()
        data = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64)
    return rate, data


def segments(rate, x, threshold):
    """Contiguous stretches of sound, from 10 ms RMS windows."""
    win = rate // 100
    n = len(x) // win
    rms = np.sqrt(np.mean(x[:n * win].reshape(n, win) ** 2, axis=1))
    on = rms > threshold
    out, start = [], None
    for i, v in enumerate(on):
        if v and start is None:
            start = i
        elif not v and start is not None:
            if i - start >= 3:
                out.append((start * win, i * win))
            start = None
    if start is not None:
        out.append((start * win, n * win))
    return out, rms, win


def peak_frequency(rate, x):
    x = x - np.mean(x)
    spec = np.abs(np.fft.rfft(x * np.hanning(len(x)), n=1 << 20))
    k = int(np.argmax(spec[1:])) + 1
    a, b, c = spec[k - 1], spec[k], spec[k + 1]
    k += 0.5 * (a - c) / (a - 2 * b + c)
    return k * rate / (1 << 20), spec


def flatness(spec):
    s = spec[len(spec) // 100:len(spec) // 2] + 1e-9
    return float(np.exp(np.mean(np.log(s))) / np.mean(s))


def main():
    rate, x = load(sys.argv[1])
    segs, rms, win = segments(rate, x, threshold=100.0)
    ok = True

    def report(name, good, detail):
        nonlocal ok
        ok &= good
        print(f'  [{"PASS" if good else "FAIL"}] {name}: {detail}')

    print(f'{len(segs)} sound segments found')
    if len(segs) < 8:
        print('expected 8 segments')
        return 1

    expected = [
        ('pulse 1 period $0FD', CPU_HZ / (16 * (0x0FD + 1))),
        ('pulse 2 period $07E', CPU_HZ / (16 * (0x07E + 1))),
        ('triangle period $0FD', CPU_HZ / (32 * (0x0FD + 1))),
    ]
    for (name, f_exp), (a, b) in zip(expected, segs[:3]):
        mid = x[a + (b - a) // 4:b - (b - a) // 4]
        f, _ = peak_frequency(rate, mid)
        report(name, abs(f / f_exp - 1) < 0.005, f'{f:.1f} Hz, expected {f_exp:.1f} Hz')

    a, b = segs[3]
    _, spec = peak_frequency(rate, x[a + (b - a) // 4:b - (b - a) // 4])
    fl = flatness(spec)
    report('noise is broadband', fl > 0.3, f'spectral flatness {fl:.2f}')

    a, b = segs[4]
    f_exp = CPU_HZ / 54 / 8
    f, _ = peak_frequency(rate, x[a + (b - a) // 4:b - (b - a) // 4])
    report('DMC sample of $F0 at rate 15', abs(f / f_exp - 1) < 0.005, f'{f:.1f} Hz, expected {f_exp:.1f} Hz')

    # Envelope: volume 15 -> 0, one step per 16 quarter frames (4 per frame
    # ~ 240 Hz): about 1 s until silent.
    a, b = segs[5]
    dur = (b - a) / rate
    report('envelope period 15 decays in ~1 s', 0.9 < dur < 1.15, f'audible for {dur:.2f} s')
    first = rms[a // win + 2:a // win + 8].mean()
    last = rms[b // win - 8:b // win - 2].mean()
    report('envelope decays', last < first * 0.3, f'RMS {first:.0f} -> {last:.0f}')

    # Length counter: 40 half frames (120 Hz) = 0.333 s, give or take the
    # position of the first half-frame clock.
    a, b = segs[6]
    dur = (b - a) / rate
    report('length counter 40 half frames', 0.30 < dur < 0.37, f'audible for {dur:.3f} s')

    # Sweep: pulse 1 from $3FF, negate, shift 3, every 3 half frames: the
    # pitch rises until the period drops below 8 and the channel mutes.
    a, b = segs[7]
    quarter = (b - a) // 4
    pitches = [peak_frequency(rate, x[a + i * quarter:a + (i + 1) * quarter])[0] for i in range(3)]
    f0 = CPU_HZ / (16 * (0x3FF + 1))
    report('sweep raises the pitch', pitches[0] < pitches[1] < pitches[2] and pitches[0] > f0 * 0.9,
           ' -> '.join(f'{p:.0f} Hz' for p in pitches))

    print('ALL PASS' if ok else 'FAILURES')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
