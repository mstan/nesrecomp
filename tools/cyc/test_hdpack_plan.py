"""Compare immutable HD plans with the independent pixel/reference lookup."""
import argparse
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--reference',required=True);p.add_argument('--candidate',required=True)
a=p.parse_args()
rows=[]
for exe in (a.reference,a.candidate):
    r=subprocess.run([exe],capture_output=True,timeout=90,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    assert r.returncode==0,(exe,r.returncode,r.stderr.decode(errors='replace'))
    rows.append(r.stdout.splitlines())
assert len(rows[0])==len(rows[1])==192
for i,(before,after) in enumerate(zip(*rows)):
    assert before==after,(i,before,after)
print('PASS:192 full pictures match: CHR RAM/ROM, dialects99/106, scales1/2/3/10, conditional priority/negation/aliases, nearby/position/origin/flips, alpha/add/sub/brightness, clipping/HUD, same-frame invalidation, epoch wrap and allocation fallback')
