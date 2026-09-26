"""Executable MMC5 contracts, including writable code and folded PCM reads."""
from mapper_fixtures import handoff
from mapper_ppu_fixtures import ppu_contract
from bandai_fixtures import Program
from cart_variant_fixtures import nes2

class M5Program(Program):
    def label(self,name): self.labels[name]=0xe000+len(self.code)
    def read(self,a): self.emit(0xad,a&255,a>>8)

def m5image(name,p,ram=7,extra=None,expected='final:A=42'):
    code=p.finish(); assert len(code)<8192-6
    prg=bytearray([255])*131072
    prg[-8192:-8192+len(code)]=code
    prg[-6:]=bytes([0,0xe0])*3
    if extra: extra(prg,p)
    h=b'NES\x1a'+bytes([8,0,0x50,8,0,0,ram,7,0,0,0,0])
    return 'mmc5_'+name,h+prg,'0f:e000\n',expected

def register_program():
    p=M5Program(); p.emit(0x78,0xd8,0xa2,255,0x9a)
    for a,b in ((0,255),(255,255),(127,199),(42,42)):
        p.store(0x5205,a); p.store(0x5206,b)
        p.read(0x5205); p.expect(a*b&255); p.read(0x5206); p.expect(a*b>>8)
    p.store(0x5104,2); p.store(0x5c23,0xa5); p.read(0x5c23); p.expect(0xa5)
    p.store(0x5104,3); p.store(0x5c23,0); p.read(0x5c23); p.expect(0xa5)
    p.store(0x5102,2); p.store(0x5103,1); p.store(0x6000,0x42)
    p.store(0x5114,0); p.read(0x8000); p.expect(0x42)
    p.store(0x5103,0); p.store(0x8000,0xee); p.read(0x6000); p.expect(0x42)
    p.emit(0xa9,0x42)
    return m5image('registers',p)

def ram_program():
    p=M5Program(); p.emit(0x78,0xd8,0xa2,255,0x9a)
    p.store(0x5102,2); p.store(0x5103,1)
    # Writable slot contains a different instruction stream than ROM bank 0.
    # After interpreting it, jump back to compiled E000 code, which reads A.
    stub=[0xa9,0x42,0x8d,1,0,0x60]
    for i,v in enumerate(stub): p.store(0x6000+i,v)
    p.store(0x5114,0); p.emit(0x20,0,0x80); p.read(1); p.expect(0x42)
    # Change the immediate in RAM and call again: no stale native cache.
    p.store(0x8001,0x37); p.emit(0x20,0,0x80); p.read(1); p.expect(0x37)
    p.emit(0xa9,0x42)
    return m5image('ram_execution',p,expected='final:fallback:A=42')

def pcm_program(opcode=False,dma=False):
    p=M5Program(); p.emit(0x78,0xd8,0xa2,255,0x9a)
    p.store(0x5010,0x81)
    if opcode:
        # BRK and its padding are zero-valued folded reads. The BRK handler
        # runs in E000 space, so later opcodes cannot replace the DAC input.
        p.store(0,0)
        p.emit(0x20,0,0x80)
    elif dma:p.store(0x4014,0x80)
    else:p.read(0x8100)
    if opcode:p.read(0);p.expect(1)
    else:p.read(0x5010);p.expect(0x81)
    p.read(0x5010); p.expect(1)
    p.store(0x5010,128); p.store(0x5011,0); p.read(0x5010); p.expect(129)
    p.store(0x5010,0); p.emit(0xa9,0x42)
    if opcode:
        p.jump('done');p.label('handler');p.read(0x5010);p.expect(129);p.emit(0xe6,0,0x40)
    def data(prg,_):
        prg[:3]=bytes([0,0,0x60]);prg[0xff]=0;prg[0x100]=0
        if opcode:
            a=p.labels['handler'];prg[-2:]=bytes([a&255,a>>8])
    case=m5image('pcm_'+('opcode' if opcode else 'dma' if dma else 'data'),p,extra=data)
    return (*case[:2],case[2]+'00:8000\n',case[3])

def timer_program(dma=False):
    p=M5Program(); p.emit(0x78,0xd8,0xa2,255,0x9a)
    p.store(0x4017,64); p.store(0,0); p.store(0x520a,1); p.store(0x5209,0)
    if dma:p.store(0x4014,2)
    p.emit(0x58);p.label('wait');p.read(0);p.emit(0xf0,0xfb);p.expect(1)
    p.emit(0xa9,0x42);p.jump('done')
    p.label('handler');p.read(0x5209);p.expect(128);p.emit(0xe6,0,0x40)
    def vector(prg,p):
        a=p.labels['handler'];prg[-2:]=bytes([a&255,a>>8])
    return m5image('timer'+('_dma' if dma else ''),p,extra=vector)

def render_program(exmode=0,split=0):
    p=M5Program();p.emit(0x78,0xd8,0xa2,255,0x9a)
    p.store(0x2000,0);p.store(0x2001,0);p.store(0x4017,64)
    p.emit(*([0x2c,2,0x20,0x10,0xfb]*2))
    p.store(0x5104,2)
    # Four 256-byte regions, distinct attributes/banks for substitutions.
    for i in range(4):
        p.emit(0xa2,0,0xa9,(0x42+i*0x40)&255,0x9d,0,0x5c+i,0xe8,0xd0,0xfa)
    p.store(0x5104,exmode);p.store(0x5105,0xff);p.store(0x5106,3);p.store(0x5107,1)
    p.store(0x5101,0);p.store(0x5127,1);p.store(0x512b,0)
    p.store(0x5200,split);p.store(0x5201,0);p.store(0x5202,2)
    p.store(0x2006,0x3f);p.store(0x2006,0)
    for color in (0x0f,0x11,0x21,0x31)*8:p.store(0x2007,color)
    p.store(0x2006,0);p.store(0x2006,0);p.store(0x2005,0);p.store(0x2005,0)
    p.store(0x2000,0x20);p.store(0x2001,0x1e)
    # Poll an IRQ generated from real PPU fetches, not a synthetic line tick.
    p.store(0x5203,5);p.label('wait');p.read(0x5204);p.emit(0x10,0xfb)
    p.emit(0x29,0x40);p.expect(0x40);p.emit(0xa9,0x42)
    name,img,seeds,expected=m5image(f'render_{exmode}_{split:02x}',p)
    h=bytearray(img[:16]);h[5]=16;h[11]=0
    # Each tile's row/plane distinguish banks and fine Y, with no solid-color
    # degeneracy that would hide incorrect split or CHR selection.
    chr_=bytes(((i//1024*23+i//16*7+i%8*31)^(0x55 if i%16<8 else 0xaa))&255 for i in range(131072))
    return name,bytes(h)+img[16:]+chr_,seeds,expected

def tone_program(channel=0):
    p=M5Program();p.emit(0x78,0xd8);p.store(0x4015,0);p.store(0x4017,64)
    p.store(0x5011,1);p.store(0x5015,1<<channel)
    p.store(0x5000+channel*4,0xbf);p.store(0x5002+channel*4,253);p.store(0x5003+channel*4,0)
    p.emit(0xa9,0x42)
    return m5image(f'tone{channel}',p)

def save_program(mixed=False):
    p=M5Program();p.emit(0x78,0xd8,0xa2,255,0x9a)
    p.store(0x5102,2);p.store(0x5103,1);p.store(0x5104,2)
    if mixed:
        p.store(0x5113,4);p.read(0x6000);p.expect(0)
        p.store(0x6000,0x77);p.store(0x5113,0)
    for addr in (0x6000,0x5c00):
        p.read(addr);p.emit(0x18,0x69,1,0x8d,addr&255,addr>>8)
    p.emit(0xcd,0,0x60,0xf0,3);p.jump('fail')
    name,img,seeds,_=m5image('save_mixed' if mixed else 'save',p,ram=0x77 if mixed else 0x70)
    img=bytearray(img);img[6]|=2
    return name,bytes(img),seeds,'final:A=01'

def mmc5_fixtures():
    for mode in range(4):
        for addressing in ('absolute','indexed','indirect'):
            reg=(0x5117,0x5115,0x5115,0x5114)[mode]
            bank=(4,6,6,7)[mode]
            yield handoff(f'mmc5_prg_{mode}_{addressing}',5,[(0x5100,mode),(reg,0x87)],bank,addressing=addressing)
        yield handoff(f'mmc5_vectors_{mode}',5,[(0x5100,mode),(0x5117,0x87)],7,start=0xe000)
    for mode in range(4):
        for sprite16 in (False,True):
            for b in (False,True):
                ops=[('cpu',0x5101,mode),('cpu',0x2000,32 if sprite16 else 0)]
                ops += [('cpu',0x5120+r,2+r) for r in range(12)]
                if not b:ops += [('cpu',0x5127,9)]
                for page in range(8):
                    size=8>>mode;r=page//size*size+size-1
                    if sprite16 and b:r=8+r%4
                    bank=((2+r)*size&~(size-1))+(page&(size-1))
                    ops += [('read',page*1024,bank)]
                case=ppu_contract(5,128,128,ops,f'_{mode}_{int(sprite16)}_{int(b)}')
                yield ('mmc5_'+case[0],*case[1:])
    yield register_program();yield ram_program()
    yield pcm_program();yield pcm_program(opcode=True);yield pcm_program(dma=True)
    yield timer_program();yield timer_program(True)
    for mode in (0,1):
        for split in (0,0x90,0xd0):yield render_program(mode,split)
    yield tone_program();yield tone_program(1)
    yield save_program();yield save_program(True)
