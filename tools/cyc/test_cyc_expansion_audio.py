#!/usr/bin/env python3
"""Measure rendered expansion PCM from the execution-harness tone fixtures.

First build fixtures with test_cyc_runtime.py --case-prefix exp_tone_. Pass its
--out directory as --fixtures here. Requires no ROMs or third-party Python libs.
"""
import argparse, json, math, struct, subprocess, wave
from pathlib import Path

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--fixtures',type=Path,required=True)
    args=ap.parse_args();root=args.fixtures.resolve();measurements=[]
    for mapper in (24,26):
        for voice,divisor in (('pulse',16*254),('saw',14*128)):
            name=f'exp_tone_{mapper}_{voice}';case=root/name
            suffix='.exe' if hasattr(subprocess,'CREATE_NO_WINDOW') else ''
            exe=root/'build'/'Release'/(name+suffix)
            if not exe.exists():exe=root/'build'/(name+suffix)
            for align in range(4):
                rendered=[]
                for mode,extra in (('native',[]),('interp',['--interp-only'])):
                    wav=case/f'pcm_a{align}_{mode}.wav'
                    p=subprocess.run([str(exe),str(case/(name+'.nes')),'--frames','90','--align',str(align),'--wav-out',str(wav)]+extra,
                        capture_output=True,text=True,timeout=60,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
                    (wav.with_suffix('.log')).write_text(p.stdout+p.stderr)
                    assert p.returncode==0,(name,mode,p.stderr)
                    with wave.open(str(wav)) as f:
                        rate=f.getframerate();data=f.readframes(f.getnframes())
                        assert f.getnchannels()==1 and f.getsampwidth()==2
                    rendered.append(data)
                assert rendered[0]==rendered[1],(name,align,'native/interpreter PCM differs')
                samples=struct.unpack('<'+'h'*(len(data)//2),data)[rate//2:]
                assert len(samples)>rate//2
                center=sum(samples)/len(samples)
                crossings=[i for i in range(1,len(samples)) if samples[i-1]<=center<samples[i]]
                assert len(crossings)>100
                frequency=rate*(len(crossings)-1)/(crossings[-1]-crossings[0])
                expected=(21477272.7272727/12)/divisor
                assert abs(frequency-expected)<1,(name,align,frequency,expected)
                rms=math.sqrt(sum((x-center)**2 for x in samples)/len(samples))
                assert rms>50 and max(samples)<32767 and min(samples)>-32768,(name,rms)
                measurements.append(dict(mapper=mapper,voice=voice,align=align,frequency=frequency,expected=expected,rms=rms))
    (root/'pcm-measurements.json').write_text(json.dumps(measurements,indent=2))
    print(f'{len(measurements)} PCM measurements passed; native/interpreter WAVs identical')

if __name__=='__main__':main()
