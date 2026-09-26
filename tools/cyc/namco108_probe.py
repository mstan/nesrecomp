#!/usr/bin/env python3
"""Build an original ROM for investigating the reported Namco 108 false writes.

Hardware report: https://forums.nesdev.org/viewtopic.php?p=156136
This measures behavior; it does not assume undocumented write timing. Flash
onto the actual mapper PCB under test. An emulator/flashcart's mapper model
is not evidence about a Namco ASIC. See runner/cyc/NAMCO108_PROBE.md.
"""
import argparse
from pathlib import Path

ADDRESSES=(0x0000,0x0001,0x0100,0x0101,0x0800,0x0801,0x1800,0x1801)
WINDOWS=(0x8100,0xa100,0xc100,0xe100)

def check_memory(path):
    """Assert the ordinary mapper model's 32 measurements, independently of A."""
    results=bytearray()
    for line in path.read_text().splitlines():
        if line.startswith('ram '):
            address=int(line[4:8],16)
            if 0x700<=address<0x780:
                results.extend(bytes.fromhex(line.split(':',1)[1]))
    assert results==bytes([0,1,5,1])*32,(path,results.hex())

def fixture():
    # Original compact 3x5 hex glyphs, centered in an 8x8 tile.
    glyphs=('111101101101111','010110010010111','111001111100111','111001111001111',
            '101101111001001','111100111001111','111100111101111','111001001001001',
            '111101111101111','111101111001111','111101111101101','110101110101110',
            '111100100100111','110101101101110','111100110100111','111100110100100')
    chr_rom=bytearray(8192)
    for i,glyph in enumerate(glyphs,1):
        for row in range(5):chr_rom[i*16+row+1]=int(glyph[row*3:row*3+3],2)<<3
    # Keep the control program inside one physical 4 KiB compiler unit.
    origin=0xe400
    code=bytearray([0x78,0xd8,0xa2,255,0x9a])
    def emit(*v):code.extend(v)
    def store(a,v):emit(0xa9,v,0x8d,a&255,a>>8)
    def address(a):store(0x2006,a>>8);store(0x2006,a&255)
    store(0x2000,0);store(0x2001,0);store(0x4017,0x40)
    emit(*([0x2c,2,32,0x10,0xfb]*2))
    # Standard mapper setup happens in the safe fixed $E000 window.
    for r,value in ((0,0),(1,2),(2,4),(3,5),(4,6),(5,7)):
        store(0x8000,r);store(0x8001,value)
    for row,pc in enumerate(WINDOWS):
        for column,ram in enumerate(ADDRESSES):
            for r,value in ((6,0),(7,1)):
                store(0x8000,r);store(0x8001,value)
            store(0x8000,6)
            entry=pc+column*8;emit(0x20,entry&255,entry>>8)
            result=0x700+row*32+column*4
            # Four nibbles per probe: two PRG windows before/after a safe
            # data-port write. This detects both odd-data and even-select
            # false writes without depending on which register got changed.
            for offset,read in ((0,0x9f00),(1,0xbf00)):
                emit(0xad,read&255,read>>8,0x8d,(result+offset)&255,(result+offset)>>8)
            store(0x8001,5)
            for offset,read in ((2,0x9f00),(3,0xbf00)):
                emit(0xad,read&255,read>>8,0x8d,(result+offset)&255,(result+offset)>>8)
    # Restore CHR after an erroneous even-address write might have selected
    # a CHR register. The fixed-window control program remains executable.
    for r,value in ((0,0),(1,2),(2,4),(3,5),(4,6),(5,7)):
        store(0x8000,r);store(0x8001,value)
    address(0x2000);emit(0xa0,4,0xa2,0,0xa9,0)
    emit(0x8d,7,32,0xe8,0xd0,0xfa,0x88,0xd0,0xf7) # clear 1024 bytes
    address(0x3f00);store(0x2007,0x0f);store(0x2007,0x30)
    for row in range(4):
        address(0x2000+(4+row*3)*32);emit(0xa2,0)
        # Render all 32 raw result nibbles. Normal behavior reads 0151 eight
        # times in each row; the rows correspond to $8100/$A100/$C100/$E100.
        emit(0xbd,row*32,7,0x29,15,0x18,0x69,1,0x8d,7,32,0xe8,0xe0,32,0xd0,0xf0)
    store(0x2005,0);store(0x2005,0);address(0);store(0x2001,0x0a)
    emit(0xa9,0x42);end=origin+len(code);emit(0x4c,end&255,end>>8)
    assert len(code)<=0xf000-origin
    prg=bytearray([255])*131072
    for b in range(16):
        for n,ram in enumerate(ADDRESSES):
            off=b*8192+0x100+n*8
            prg[off:off+6]=bytes([0xa9,3,0x8d,ram&255,ram>>8,0x60])
        prg[b*8192+0x1f00]=b
    control=15*8192+(origin&0x1fff)
    prg[control:control+len(code)]=code
    prg[-6:]=bytes([origin&255,origin>>8])*3
    header=b'NES\x1a'+bytes([8,1,0xe0,0xc0])+bytes(8)
    seeds=f'0f:{origin:04x}\n'+''.join(f'{b:02x}:{pc+n*8:04x}\n' for b in range(16) for pc in WINDOWS for n in range(8))
    return 'namco108_probe',header+prg+chr_rom,seeds,'final:A=42'

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--out',type=Path,required=True)
    args=ap.parse_args();args.out.write_bytes(fixture()[1]);print(args.out)
