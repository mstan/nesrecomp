"""Compare PAL batching against the existing coupled-clock path."""
import argparse
import subprocess
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--probe', required=True)
a = p.parse_args()
outputs = []
for mode in ('reference', 'candidate'):
    run = subprocess.run([a.probe, mode], capture_output=True, text=True, timeout=120)
    if run.returncode:
        raise AssertionError((mode, run.stderr, run.stdout))
    outputs.append(run.stdout.splitlines())
assert len(outputs[0]) == len(outputs[1])
for n, (before, after) in enumerate(zip(*outputs)):
    assert before == after, (n, before, after)
print('PASS: all PAL phases, NROM/CNROM, CHR writes, DMA, split PPU traffic, APU modes, Zapper, snapshots, machine/picture state and PCM')
