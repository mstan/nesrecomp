"""VRC7 PCB decode, banking/IRQ programs and a custom sine carrier."""
from mapper_fixtures import handoff
from mapper_ppu_fixtures import ppu_contract
from cart_variant_fixtures import nes2
from vrc_fixtures import irq_program

def fm_tone(sub=2, reset=False):
    code=bytearray([0x78,0xd8]);pin=8 if sub==1 else 16
    def store(a,v):code.extend([0xa9,v,0x8d,a&255,a>>8])
    def reg(r,v):store(0x9000+pin,r);store(0x9020+pin,v)
    store(0x4015,0);store(0x4017,0x40)
    # Modulator attack=0 keeps it silent; carrier multiplier=1, instant attack,
    # fixed sustain level. fnum=290, octave=4 -> 440.601 Hz.
    for r,v in enumerate((1,0x21,0x3f,0,0,0xf0,0xff,0)):reg(r,v)
    reg(0x30,0);reg(0x10,34);reg(0x20,0x19)
    if reset:
        store(0xe000,0x40)
        reg(0x30,0x30);reg(0x20,0x19) # held reset ignores writes
        store(0xe000,0)
    code.extend([0xa9,0x42]);done=0x8000+len(code);code.extend([0x4c,done&255,done>>8])
    prg=bytearray([0xff])*131072
    for bank in range(16):
        prg[bank*8192:bank*8192+len(code)]=code
        prg[(bank+1)*8192-6:(bank+1)*8192]=bytes([0,0x80])*3
    header=b'NES\x1a'+bytes([8,0,0x50,0x50])+bytes(8)
    return nes2((f'tone_85_{sub}'+('_reset' if reset else ''),header+prg,'00:8000\n','A=42'),sub=sub,chr_ram=7)

def vrc7_fixtures():
    cases=[]
    for sub in (0,1,2):
        pin=8 if sub==1 else 16
        cases.append(nes2(handoff(f'prg85_{sub}',85,[(0x8000,3)],3,prg_kb=512),sub=sub))
        ops=[('cpu',0xa000+(r//2)*4096+(r%2)*pin,240+r) for r in range(8)]
        ops += [('read',r*1024,240+r) for r in range(8)]
        ops += [('cpu',0xe000,0x83),('cpu',0x6000,0xa5),('cpu_read',0x6000,0xa5),
                ('write',0x2000,0x55),('read',0x2c00,0x55),('cpu',0xe000,0),('cpu_read',0x6000,0x60)]
        cases.append(nes2(ppu_contract(85,512,256,ops,f'_vrc7_{sub}'),sub=sub,ram=7))
        cases += [irq_program(85,sub),irq_program(85,sub,scanline=True,dma=True)]
    cases += [fm_tone(),fm_tone(1),fm_tone(reset=True)]
    for name,image,seeds,expected in cases:yield 'fm_'+name,image,seeds,expected
