#!/usr/bin/env python3
"""Whole-machine HD fetch pipeline: CHR RAM/ROM, banks, scroll, sprite flips,
8/16px sprites, grayscale/emphasis, original fallback, and saved continuation.
Fixtures and PNGs are synthetic; no game ROM or texture pack is required.
"""
import argparse
from pathlib import Path
import struct
import subprocess
import zlib


def png_write(path, width, height, pixel):
    def chunk(kind, data):
        return struct.pack('>I', len(data))+kind+data+struct.pack('>I', zlib.crc32(kind+data))
    rows=b''.join(b'\0'+bytes(c for x in range(width) for c in pixel(x,y)) for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b''))


def png_read(path):
    data=path.read_bytes();offset=8;compressed=bytearray()
    while offset<len(data):
        length=struct.unpack_from('>I',data,offset)[0];kind=data[offset+4:offset+8];payload=data[offset+8:offset+8+length];offset+=12+length
        if kind==b'IHDR':width,height,depth,mode,_,_,_=struct.unpack('>IIBBBBB',payload);assert depth==8 and mode==2
        if kind==b'IDAT':compressed.extend(payload)
    raw=zlib.decompress(compressed);stride=width*3
    assert all(raw[y*(stride+1)]==0 for y in range(height)) # cyc_png.c emits RGB, filter 0.
    return width,height,[raw[y*(stride+1)+1:(y+1)*(stride+1)] for y in range(height)]


PATTERNS=[bytes([255]*8+[0]*8),bytes([0xf0^(y*3) for y in range(8)]+[0xaa^(y*5) for y in range(8)])]


def fixture(chr_ram, sprite16):
    prg=bytearray([0xea]*32768);code=bytearray();labels={};fixups=[]
    def emit(*v):code.extend(v)
    def label(name):labels[name]=0x8000+len(code)
    def branch(op,name):emit(op,0);fixups.append((len(code)-1,name))
    def store(address,value):emit(0xa9,value,0x8d,address&255,address>>8)
    def upload(address,source,count):
        store(0x2006,address>>8);store(0x2006,address&255);emit(0xa2,0);name='upload'+str(len(code));label(name)
        emit(0xbd,source&255,source>>8,0x8d,7,0x20,0xe8,0xe0,count);branch(0xd0,name)
    emit(0x78,0xd8,0xa2,0xff,0x9a,0xa9,0,0x85,0);store(0x2000,0);store(0x2001,0)
    for n in range(2):label('vblank'+str(n));emit(0x2c,2,0x20);branch(0x10,'vblank'+str(n))
    if chr_ram:upload(0,0x8200,32);upload(0x1000,0x8200,32)
    upload(0x3f00,0x8220,32)
    store(0x2006,0x20);store(0x2006,0);emit(0xa0,4,0xa2,0,0xa9,0);label('nt');emit(0x8d,7,0x20,0xe8);branch(0xd0,'nt');emit(0x88);branch(0xd0,'nt')
    emit(0xa2,0,0xa9,0xff);label('oam');emit(0x9d,0,2,0xe8);branch(0xd0,'oam')
    emit(0xa2,0);label('sprites');emit(0xbd,0x40,0x82,0x9d,0,2,0xe8,0xe0,20);branch(0xd0,'sprites')
    store(0x4014,2);emit(0x2c,2,0x20);store(0x2005,5);store(0x2005,3);store(0x2000,0xa0 if sprite16 else 0x80);store(0x2001,0x1e)
    label('spin');emit(0x4c,labels['spin']&255,labels['spin']>>8)
    for off,name in fixups:delta=labels[name]-(0x8000+off+1);assert -128<=delta<=127;code[off]=delta&255
    assert len(code)<256;prg[:len(code)]=code
    # Change CHR bank during VBlank. Original color bytes are identical across
    # banks; only absolute tile identities differ, so HD must use fetched bank.
    nmi=bytes.fromhex('48 8A 48 E6 00 A5 00 29 10 4A 4A 4A 4A 8D 00 F0 A5 00 29 03 AA BD 60 82 8D 01 20 2C 02 20 A9 05 8D 05 20 A9 03 8D 05 20 68 AA 68 40')
    prg[0x100:0x100+len(nmi)]=nmi;prg[0x200:0x220]=b''.join(PATTERNS)
    palette=bytes([0x0f,1,2,3]*4+[0x0f,0x11,0x12,0x13]*4);prg[0x220:0x240]=palette
    prg[0x240:0x250]=bytes(v for i,attr in enumerate([0,0x40,0x80,0xc0]) for v in [31,1,attr,16+i*24])
    prg[0x250:0x254]=bytes([31,0,0x20,112]);prg[0x7000]=0xff # CNROM bus-conflict-safe bank writes.
    prg[0x260:0x264]=bytes([0x1e,0x1f,0x3e,0x3f]);prg[-6:]=struct.pack('<HHH',0x8100,0x8000,0x8100)
    chr_data=bytearray(16384)
    for bank in range(2):
        for base in (bank*8192,bank*8192+4096):chr_data[base:base+32]=b''.join(PATTERNS)
    mapper=0 if chr_ram else 3
    return b'NES\x1a'+bytes([2,0 if chr_ram else 2,mapper<<4,0])+bytes(8)+prg+(b'' if chr_ram else chr_data)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--host',type=Path,required=True);parser.add_argument('--out',type=Path,required=True);args=parser.parse_args()
    out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
    si=None
    if hasattr(subprocess,'STARTUPINFO'):si=subprocess.STARTUPINFO();si.dwFlags|=subprocess.STARTF_USESHOWWINDOW;si.wShowWindow=0
    def run(name,rom,pack,frames=24,extra=()):
        command=[str(args.host.resolve()),str(rom),'--frames',str(frames),'--no-save','--hdpack',str(pack),'--present-out',str(out/(name+'.png')),'--screenshot',str(out/(name+'-native.png')),'--hash-out',str(out/(name+'.hash')),*map(str,extra)]
        result=subprocess.run(command,capture_output=True,text=True,timeout=90,startupinfo=si,creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0));(out/(name+'.log')).write_text(result.stdout+result.stderr);assert result.returncode==0,result.stdout+result.stderr
        return png_read(out/(name+'.png'))
    fallback=out/'fallback';fallback.mkdir(exist_ok=True);png_write(fallback/'none.png',16,16,lambda x,y:(0,0,0,0))
    (fallback/'hires.txt').write_text('<ver>106\n<scale>2\n<img>none.png\n<tile>0,0123456789ABCDEF0123456789ABCDEF,0F010203,0,0,1,N\n')
    for ram in (True,False):
        pack=out/('ram-pack' if ram else 'rom-pack');pack.mkdir(exist_ok=True);manifest='<ver>106\n<scale>2\n'
        for bank in range(1 if ram else 2):
            png_write(pack/f'art{bank}.png',32,16,lambda x,y,b=bank:(80+b*80,x*9,y*9,255) if x<16 else (10,(x-16)*9,80+y*9,255))
            manifest+=f'<img>art{bank}.png\n'
        for bank in range(1 if ram else 2):
            for tile in (0,1,256,257):
                key=PATTERNS[tile&1].hex().upper() if ram else f'{bank*512+tile:X}'
                if tile in (0,):manifest+=f'<tile>{bank},{key},0F010203,0,0,1,N\n'
                manifest+=f'<tile>{bank},{key},FF111213,16,0,1,N\n'
        (pack/'hires.txt').write_text(manifest)
        for sprite16 in (False,True):
            rom=out/f'{"ram" if ram else "rom"}-{16 if sprite16 else 8}.nes';rom.write_bytes(fixture(ram,sprite16));tag=rom.stem
            for align in range(4):
                name=f'{tag}-a{align}';w,h,rows=run(name,rom,fallback,extra=['--align',align]);nw,nh,native=png_read(out/(name+'-native.png'));assert (w,h)==(512,480)
                for y in range(nh):
                    expected=b''.join(native[y][x*3:x*3+3]*2 for x in range(nw));assert rows[y*2]==expected and rows[y*2+1]==expected,(name,y,'fallback changed pixels')
                for frames,bank in ((24,0 if ram else 1),(48,0)):
                    hdname=f'{name}-hd{frames}';w,h,rows=run(hdname,rom,pack,frames,extra=['--align',align]);assert (w,h)==(512,480)
                    # Known fine scroll, independent of recorded HD metadata.
                    for x,y in ((100,100),(101,101),(200,180),(112,34)):
                        for dx in range(2):
                            for dy in range(2):
                                actual=rows[y*2+dy][(x*2+dx)*3:(x*2+dx)*3+3];expected=bytes([80+80*bank,((x+5)%8*2+dx)*9,((y+3)%8*2+dy)*9])
                                assert actual==expected,(hdname,x,y,list(actual),list(expected),'background tile offset/bank')
                    # Four OAM objects cover all H/V flip combinations.
                    height=16 if sprite16 else 8
                    for i in range(4):
                        for ox in range(8):
                            for oy in (1,height-2):
                                source_x=7-ox if i&1 else ox;source_y=height-1-oy if i&2 else oy
                                tile=1 if not sprite16 else source_y//8;pattern=PATTERNS[tile];row=source_y&7
                                value=((pattern[row]>>(7-source_x))&1)|(((pattern[row+8]>>(7-source_x))&1)<<1)
                                if not value:continue
                                x=16+i*24+ox;y=32+oy
                                for dx in range(2):
                                    for dy in range(2):
                                        tx=source_x*2+(1-dx if i&1 else dx);ty=row*2+(1-dy if i&2 else dy)
                                        assert rows[y*2+dy][(x*2+dx)*3:(x*2+dx)*3+3]==bytes([10,tx*9,80+ty*9]),(hdname,i,ox,oy,'sprite flip/half')
                state=out/(name+'.cycstate');run(name+'-save',rom,pack,48,['--align',align,'--save-state',f'23:{state}'])
                run(name+'-replay',rom,pack,48,['--align',align,'--load-state',state])
                assert (out/(name+'-save.png')).read_bytes()==(out/(name+'-replay.png')).read_bytes()
                assert (out/(name+'-save.hash')).read_text().splitlines()[24:]==(out/(name+'-replay.hash')).read_text().splitlines()
            print(tag,'four alignments: fallback, tile offsets, banks, sprite flips and replay passed',flush=True)


if __name__=='__main__':main()
