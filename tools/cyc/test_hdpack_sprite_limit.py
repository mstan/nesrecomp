#!/usr/bin/env python3
"""Display a ninth OAM sprite without altering the eight-sprite guest machine."""
import argparse
from pathlib import Path
import subprocess
from test_cyc_hdpack import PATTERNS, fixture, png_read, png_write


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--host',type=Path,required=True);ap.add_argument('--out',type=Path,required=True)
    args=ap.parse_args();out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    for ram in (False,True):
        for tall in (False,True):
            folder=out/f'{"ram" if ram else "rom"}-{16 if tall else 8}';folder.mkdir(exist_ok=True)
            image=bytearray(fixture(ram,tall));code=image[16:272]
            assert code.count(bytes.fromhex('BD4082'))==1 and code.count(bytes.fromhex('E014'))==1
            image[16:272]=code.replace(bytes.fromhex('BD4082'),bytes.fromhex('BD8082')).replace(bytes.fromhex('E014'),bytes.fromhex('E024'))
            objects=[v for i in range(8) for v in (31,1,0,16+i*16)]
            objects += [31,0 if tall else 1,0xc0,200]
            image[16+0x280:16+0x280+36]=bytes(objects)
            rom=folder/'fixture.nes';rom.write_bytes(image)
            for enabled in (False,True):
                pack=folder/('expanded' if enabled else 'normal');pack.mkdir(exist_ok=True)
                png_write(pack/'under.png',512,480,lambda x,y:(31,63,95,255))
                png_write(pack/'art.png',32,16,lambda x,y:(20,210,70,255) if x<16 else (220,30,40,255))
                manifest='<ver>108\n<scale>2\n<img>art.png\n<background>under.png,1,0,0,0\n'
                if enabled:manifest+='<options>disableSpriteLimit\n'
                for bank in ((0,) if ram else (0,512)):
                    for tile in (0,1):
                        key=PATTERNS[tile].hex() if ram else f'{bank+tile:X}'
                        manifest+=f'<tile>0,{key},FF111213,{tile*16},0,1,N\n'
                    key=PATTERNS[0].hex() if ram else f'{bank:X}'
                    # The authored BG is transparent; use a separate image.
                    png_write(pack/'empty.png',16,16,lambda x,y:(0,0,0,0))
                    if bank==0:manifest+='<img>empty.png\n'
                    manifest+=f'<tile>1,{key},0F010203,0,0,1,N\n'
                (pack/'hires.txt').write_text(manifest)

                def run(tag,extra=()):
                    result=subprocess.run([str(args.host.resolve()),str(rom),'--frames','24','--no-save','--hdpack',str(pack),
                        '--present-out',str(pack/(tag+'.png')),'--screenshot',str(pack/(tag+'-native.png')),
                        '--hash-out',str(pack/(tag+'.hash')),*map(str,extra)],capture_output=True,text=True,timeout=90,
                        creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
                    (pack/(tag+'.log')).write_text(result.stdout+result.stderr);assert result.returncode==0,result.stdout+result.stderr
                state=pack/'state.cycstate';run('full',['--save-state',f'18:{state}']);run('resume',['--load-state',state])
                assert (pack/'full.png').read_bytes()==(pack/'resume.png').read_bytes()
                assert (pack/'full.hash').read_text().splitlines()[19:]==(pack/'resume.hash').read_text().splitlines()
                rows=png_read(pack/'full.png')[2]
                def pixel(x,y):return rows[y*2][x*2*3:(x*2+1)*3]
                expected=bytes((220,30,40)) if enabled else bytes((31,63,95))
                assert pixel(200,34)==expected,(folder,enabled,list(pixel(200,34)))
                if enabled and tall:assert pixel(200,44)==bytes((20,210,70)),folder
            normal=folder/'normal';expanded=folder/'expanded'
            assert (normal/'full.hash').read_bytes()==(expanded/'full.hash').read_bytes()
            assert (normal/'full-native.png').read_bytes()==(expanded/'full-native.png').read_bytes()
            print(folder.name,'ninth sprite, unchanged guest, current-build state PASS',flush=True)


if __name__=='__main__':main()
