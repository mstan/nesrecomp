#!/usr/bin/env python3
"""Authored transparent replacements and native color zero reveal the layers below."""
import argparse
from pathlib import Path
import subprocess
from test_cyc_hdpack import fixture, png_read, png_write


def check_background_pages(args,out):
    folder=out/'nametable-pages';folder.mkdir(exist_ok=True)
    pack=folder/'pack';pack.mkdir(exist_ok=True)
    png_write(pack/'atlas.png',1024,960,lambda x,y:((x//2)%251,(y//2)%251,29,255))
    png_write(pack/'transparent.png',16,16,lambda x,y:(0,0,0,0))
    (pack/'hires.txt').write_text('<ver>108\n<scale>2\n<options>disableOriginalTiles\n'
        '<img>transparent.png\n<tile>0,FFF,0F010203,0,0,1,N\n'
        '<background>atlas.png,1,1,1,10\n')
    for page in range(4):
        rom=folder/f'page-{page}.nes';rom.write_bytes(fixture(False,False,nametable=page))
        output=folder/f'page-{page}.png'
        run=subprocess.run([str(args.host.resolve()),str(rom),'--frames','24','--no-save','--hdpack',str(pack),'--present-out',str(output)],capture_output=True,text=True,timeout=90,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        (folder/f'page-{page}.log').write_text(run.stdout+run.stderr)
        assert run.returncode==0,run.stdout+run.stderr
        w,h,rows=png_read(output);assert (w,h)==(512,480)
        for x,y in [(100,100),(200,180)]:
            expected=bytes([(x+5+(256 if page&1 else 0))%251,(y+3+(240 if page&2 else 0))%251,29])
            actual=rows[y*2][x*2*3:(x*2+1)*3]
            assert actual==expected,(page,x,y,list(actual),list(expected),'background nametable page')
    print('background nametable pages PASS',flush=True)


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
    check_background_pages(args,out)


if __name__=='__main__':main()
