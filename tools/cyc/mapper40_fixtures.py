"""NTDEC 2722 mapping, ROM at $6000 and 4096-M2 IRQ programs."""
from mapper_fixtures import handoff
from mapper_ppu_fixtures import ppu_contract
from bandai_fixtures import Program

def program(dma=False,low=False):
    p=Program();p.emit(0x78,0xd8,0xa2,255,0x9a);p.store(0x4017,0x40);p.store(0,0)
    if low:
        p.emit(0x20,0,0x60);p.expect(0x33)
    else:
        p.store(0x8000,0);p.store(0xa000,0)
        if dma:
            # Repeated DMA stalls cover the entire counter interval.
            for _ in range(8):p.store(0x4014,2)
        p.emit(0x58);p.label('wait');p.emit(0xa5,0,0xd0,3);p.jump('wait');p.expect(1)
    p.emit(0xa9,0x42);code=p.finish()
    prg=bytearray([255])*65536
    for b in range(8):
        prg[b*8192:b*8192+len(code)]=code
        prg[b*8192+0x100:b*8192+0x108]=bytes([0xa9,0,0x8d,0,128,0xe6,0,0x40])
        prg[(b+1)*8192-6:(b+1)*8192]=bytes([0,129,0,128,0,129])
    if low:prg[6*8192:6*8192+3]=bytes([0xa9,0x33,0x60])
    h=b'NES\x1a'+bytes([4,1,0x80,0x20])+bytes(8)
    name='mapper40_'+('low_code' if low else 'irq_dma' if dma else 'irq')
    return name,h+prg+bytes(8192),'04:8000\n04:8100\n','final:fallback:A=42' if low else 'final:A=42'

def mapper40_fixtures():
    for addr in (0xe000,0xffff):
        yield handoff(f'mapper40_bank_{addr:x}',40,[(addr,0xfb)],3,prg_kb=64,start=0xc000)
    case=ppu_contract(40,64,8,[('cpu_read',0x7e00,6),('cpu',0x7e00,0x55),('cpu_read',0x7e00,6),
        ('cpu_read',0x9e00,4),('cpu_read',0xbe00,5),('cpu_read',0xfe00,7)])
    _,image,seeds,expected=case;image=bytearray(image)
    for b in range(8):image[16+b*8192+0x1e00]=b
    yield 'mapper40_fixed',bytes(image),seeds,expected
    yield program();yield program(dma=True);yield program(low=True)
