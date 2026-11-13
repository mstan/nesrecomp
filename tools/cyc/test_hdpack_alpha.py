#!/usr/bin/env python3
"""Authored transparent replacements and native color zero reveal the layers below."""
import argparse
from pathlib import Path
import subprocess
from test_cyc_hdpack import fixture, png_read, png_write


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--host',type=Path,required=True);ap.add_argument('--out',type=Path,required=True)
    args=ap.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    for name,background_tile in [('authored',0),('native-zero',1),('sprite',0)]:
        folder=out/name;folder.mkdir(exist_ok=True)
        rom=folder/'fixture.nes';rom.write_bytes(fixture(False,False,background_tile))
        pack=folder/'pack';pack.mkdir(exist_ok=True)
        png_write(pack/'transparent.png',16,16,lambda x,y:(0,0,0,0))
        manifest='<ver>108\n<scale>2\n<img>transparent.png\n'
        if name=='sprite':
            for key in (1,257,513,769):manifest+=f'<tile>0,{key:X},FF111213,0,0,1,N\n'
        else:
            png_write(pack/'under.png',512,480,lambda x,y:(31,63,95,255))
            manifest+='<background>under.png,1,0,0,10\n'
            if name=='authored':
                for key in (0,512):manifest+=f'<tile>0,{key:X},0F010203,0,0,1,N\n'
        (pack/'hires.txt').write_text(manifest)
        run=subprocess.run([str(args.host.resolve()),str(rom),'--frames','24','--no-save','--hdpack',str(pack),'--present-out',str(folder/'hd.png'),'--screenshot',str(folder/'native.png')],capture_output=True,text=True,timeout=90,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        (folder/'run.log').write_text(run.stdout+run.stderr)
        if run.returncode:raise AssertionError(run.stdout+run.stderr)
        _,_,hd=png_read(folder/'hd.png');_,_,native=png_read(folder/'native.png')
        x,y=(16,34) if name=='sprite' else (102,100)
        expected=native[y][200*3:201*3] if name=='sprite' else bytes([31,63,95])
        actual=hd[y*2][x*2*3:(x*2+1)*3]
        if actual!=expected:raise AssertionError((name,list(actual),list(expected)))
        print(name,'PASS',flush=True)


if __name__=='__main__':main()
