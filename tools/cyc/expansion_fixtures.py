"""Executable VRC6 banks, ROM nametables, WRAM, IRQs and sound programs."""
from pathlib import Path
import re
from mapper_fixtures import handoff
from mapper_ppu_fixtures import ppu_contract
from vrc_fixtures import irq_program

ACTIVE_IDS=(24,26)

def tone_program(mapper, pulse=True):
    def addr(a): return (a&0xfffc)|((a&1)<<1)|((a&2)>>1) if mapper==26 else a
    code=bytearray([0x78,0xd8])
    def store(a,v): code.extend([0xa9,v,0x8d,a&255,a>>8])
    store(0x4015,0);store(0x4017,0x40)
    store(0x9003,0)
    page=0x9000 if pulse else 0xb000
    store(page,0x7f if pulse else 8)
    store(addr(page+1),253 if pulse else 127);store(addr(page+2),0x80)
    code.extend([0xa9,0x42]);done=0x8000+len(code);code.extend([0x4c,done&255,done>>8])
    prg=bytearray([0xff])*131072
    for bank in range(16):
        prg[bank*8192:bank*8192+len(code)]=code
        prg[(bank+1)*8192-6:(bank+1)*8192]=bytes([0,0x80])*3
    header=b'NES\x1a'+bytes([8,0,(mapper&15)<<4,mapper&240])+bytes(8)
    return f'tone_{mapper}_'+('pulse' if pulse else 'saw'),header+prg,'00:8000\n','A=42'

def expansion_fixtures():
    pins_path=Path(__file__).resolve().parents[2]/'runner/cyc/vrc6_pin_vectors.inc'
    pins=[int(n,16) for n in re.findall(r'0x([0-9a-f]{2})',pins_path.read_text())]
    assert len(pins)==256
    patterns=((1,8,16,25,32,41,49,56),(1,1,8,8,16,16,25,25),
              (1,8,16,25,32,32,41,41),(1,8,16,25,32,32,41,41),
              (1,8,16,25,32,41,49,56),(0,1,8,9,16,17,24,25),
              (1,8,16,25,32,33,40,41),(1,8,16,25,32,33,40,41))
    cases=[]
    for mapper in ACTIVE_IDS:
        def addr(a): return (a&0xfffc)|((a&1)<<1)|((a&2)>>1) if mapper==26 else a
        cases.append(handoff(f'vrc6_prg_{mapper}',mapper,[(0x8003,3)],6))
        for mode in (0,1,2,3,0x20,0x21,0x22,0x23,0x24,0x27,0x28,0x2b,0x2c,0x2f):
            ops=[('cpu',addr(0xd000+(r//4)*4096+r%4),v) for r,v in enumerate((1,8,16,25,32,41,49,56))]
            ops += [('cpu',0xb003,mode)]
            ops += [('read',page*1024,v) for page,v in enumerate(patterns[(mode&3)+(4 if mode&32 else 0)])]
            # Populate both CIRAM pages, then use the measured pin outputs.
            ops += [('cpu',0xb003,0x20),('write',0x2000,0x50),('write',0x2400,0x51),('cpu',0xb003,mode)]
            ops += [('read',0x2000+nt*1024,0x50+(pins[mode*4+nt]&1)) for nt in range(4)]
            ops += [('cpu',0xb003,mode|16)]
            ops += [('read',0x2000+nt*1024,pins[(mode|16)*4+nt]&63) for nt in range(4)]
            ops += [('cpu',0xb003,0xa0),('cpu',0x6000,0x55),('cpu_read',0x6000,0x55)]
            cases.append(ppu_contract(mapper,256,256,ops,f'_vrc6_{mode:02x}'))
        cases += [irq_program(mapper),irq_program(mapper,scanline=True,dma=True),tone_program(mapper),tone_program(mapper,False)]
    for name,image,seeds,expected in cases: yield 'exp_'+name,image,seeds,expected
