#!/usr/bin/env python3
"""Datach scanner stimulus: independently specified EAN waveform and bus parity."""
import argparse,re,subprocess
from pathlib import Path
from cyc_verify import first_difference

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--fixtures',type=Path,required=True)
    ap.add_argument('--interp',type=Path,required=True)
    ap.add_argument('--oracle',type=Path,required=True)
    args=ap.parse_args();root=args.fixtures.resolve();name='bandai_nes2_barcode_probe';case=root/name
    suffix='.exe' if hasattr(subprocess,'CREATE_NO_WINDOW') else ''
    native=root/'build'/'Release'/(name+suffix)
    if not native.exists():native=root/'build'/(name+suffix)
    # GS1 EAN-13 5901234123457, parity LGGLLG, dark=1.
    bars=('101' '0001011' '0100111' '0110011' '0010011' '0111101' '0011101'
          '01010' '1100110' '1101100' '1000010' '1011100' '1001110' '1000100' '101')
    light='1'*33+''.join('1' if c=='0' else '0' for c in bars)+'1'*32
    levels='0'+light+'0'
    edges=sum(a!=b for a,b in zip(levels,levels[1:]));checks=0
    for speed in (500,1000,2000):
        for align in range(4):
            traces={}
            for mode,exe,extra in [('native',native,[]),('embedded',native,['--interp-only']),
                                  ('standalone',args.interp.resolve(),[]),('oracle',args.oracle.resolve(),[])]:
                trace=case/f'barcode_{speed}_{align}_{mode}.txt';mem=trace.with_suffix('.mem')
                p=subprocess.run([str(exe),str(case/(name+'.nes')),'--frames','24','--align',str(align),
                    '--barcode','5901234123457','--barcode-frame','2','--barcode-module-cycles',str(speed),
                    '--hash-out',str(trace),'--mem-frame','23','--mem-out',str(mem),*extra],
                    capture_output=True,text=True,timeout=30,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
                trace.with_suffix('.log').write_text(p.stdout+p.stderr)
                assert p.returncode==0,(mode,p.stderr)
                match=re.search(r'ram 0000:\s+([0-9A-Fa-f]{2})',mem.read_text())
                assert match and int(match[1],16)==edges,(mode,edges,mem.read_text()[:200])
                traces[mode]=trace;checks+=1
            for mode in ('embedded','standalone','oracle'):
                assert first_difference(traces['native'],traces[mode],mode!='oracle') is None,(speed,align,mode)
    print(f'{checks} scanner runs passed: {edges} expected edges, three swipe speeds, native/interpreter/oracle parity')

if __name__=='__main__':main()
