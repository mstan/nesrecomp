#!/usr/bin/env python3
"""Check AWJ's MMC5 CHR hardware-test configurations and published screenshots.

Supply mmc5test_v2.nes and the six PNGs from the two attachments at
https://sourceforge.net/p/fceultra/bugs/787/ . The revised attachment contains
an 8K PRG: repeat it four times in the supplied .nes's 32K PRG region, keeping
the original header and CHR. No third-party ROM is vendored or downloaded here.
"""
import argparse, struct, subprocess, zlib
from pathlib import Path
from cyc_verify import first_difference

def png(path):
    data=path.read_bytes();assert data[:8]==b'\x89PNG\r\n\x1a\n'
    i=8;compressed=b'';palette=None
    while i<len(data):
        size=struct.unpack('!I',data[i:i+4])[0];kind=data[i+4:i+8];chunk=data[i+8:i+8+size];i+=size+12
        if kind==b'IHDR':w,h,depth,color,_,_,interlace=struct.unpack('!IIBBBBB',chunk)
        if kind==b'PLTE':palette=[tuple(chunk[x:x+3]) for x in range(0,len(chunk),3)]
        if kind==b'IDAT':compressed+=chunk
    assert depth==8 and not interlace and color in (2,3,6),(depth,color,interlace)
    bpp={2:3,3:1,6:4}[color];stride=w*bpp;raw=zlib.decompress(compressed);rows=[];previous=bytearray(stride)
    for y in range(h):
        start=y*(stride+1);mode=raw[start];row=bytearray(raw[start+1:start+1+stride])
        for x in range(stride):
            a=row[x-bpp] if x>=bpp else 0;b=previous[x];c=previous[x-bpp] if x>=bpp else 0
            if mode==1:v=a
            elif mode==2:v=b
            elif mode==3:v=(a+b)//2
            elif mode==4:
                p=a+b-c;da,db,dc=abs(p-a),abs(p-b),abs(p-c)
                v=a if da<=db and da<=dc else b if db<=dc else c
            else:assert mode==0;v=0
            row[x]=(row[x]+v)&255
        rows.append([palette[row[x]] if color==3 else tuple(row[x:x+3]) for x in range(0,stride,bpp)])
        previous=row
    return rows

def compare_picture(actual,reference):
    # FCEUX's published captures crop eight overscan rows at top and bottom.
    a=png(actual)[8:232];b=png(reference);assert len(a)==len(b)==224
    # Each emulator chooses its RGB palette. Require one consistent mapping
    # in both directions, preserving every glyph, color region and pixel.
    forward={};back={}
    for y,(ar,br) in enumerate(zip(a,b)):
        for x,(av,bv) in enumerate(zip(ar,br)):
            assert forward.setdefault(av,bv)==bv,(actual,x,y,'reference pixel mismatch')
            assert back.setdefault(bv,av)==av,(actual,x,y,'reference palette mismatch')

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    for name in ('rom','references','interp','oracle','out'):ap.add_argument('--'+name,type=Path,required=True)
    ap.add_argument('--native',type=Path)
    args=ap.parse_args();args.out.mkdir(parents=True,exist_ok=True)
    cases=[[],['RIGHT'],['START'],['START','RIGHT'],['SELECT'],['SELECT','RIGHT']]
    references=sorted(args.references.glob('mmc5test-[0-5]-*.png'));assert len(references)==6
    runs=0
    for case,buttons in enumerate(cases):
        schedule=args.out/f'case{case}.input'
        schedule.write_text(''.join(f'{10+i*10} {button}\n{11+i*10} -\n' for i,button in enumerate(buttons)),newline='\n')
        for align in range(4):
            hashes={}
            modes=[('interp',args.interp),('oracle',args.oracle)]
            if args.native:modes.append(('native',args.native))
            for mode,exe in modes:
                base=args.out/f'case{case}_a{align}_{mode}';shot=base.with_suffix('.png');trace=base.with_suffix('.txt')
                p=subprocess.run([str(exe.resolve()),str(args.rom.resolve()),'--frames','90','--align',str(align),
                    '--input',str(schedule.resolve()),'--screenshot',str(shot.resolve()),'--hash-out',str(trace.resolve())],
                    capture_output=True,text=True,timeout=90,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
                base.with_suffix('.log').write_text(p.stdout+p.stderr)
                assert p.returncode==0,(case,align,mode,p.stderr)
                compare_picture(shot,references[case]);hashes[mode]=trace;runs+=1
            assert first_difference(hashes['interp'],hashes['oracle'],False) is None,(case,align,'oracle trace mismatch')
            if args.native:assert first_difference(hashes['interp'],hashes['native'],True) is None,(case,align,'native trace mismatch')
    print(f'{runs} executions matched all six published CHR test pictures and cycle traces at four alignments')

if __name__=='__main__':main()
