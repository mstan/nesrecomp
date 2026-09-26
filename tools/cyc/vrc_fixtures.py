"""VRC wiring, CPU IRQs, DMA clocking, CHR high bits and the VRC2 bit latch."""
from mapper_fixtures import handoff
from mapper_ppu_fixtures import ppu_contract
from cart_variant_fixtures import nes2

ACTIVE_IDS = (21, 22, 23, 25, 73)

BOARDS=((21,1,2,4),(21,2,64,128),(22,0,2,1),
        (23,1,1,2),(23,2,4,8),(23,3,1,2),
        (25,1,2,1),(25,2,8,4),(25,3,2,1))


def irq_program(mapper, sub=0, high=0, next_=0, scanline=False, dma=False, width8=False):
    code=bytearray([0x78,0xd8,0xa2,0xff,0x9a])
    def store(a,v):code.extend([0xa9,v,0x8d,a&255,a>>8])
    store(0,0);store(0x4017,0x40)
    if mapper==73:
        for a,v in ((0x8000,14),(0x9000,15),(0xa000,15),(0xb000,15)):store(a,v)
        control=0xc000;store(control,6 if width8 else 2)
    else:
        store(0xf000,15);store(0xf000+high,15)
        control=0xf000+next_;store(control,2 if scanline else 6)
    if dma:store(0x4014,2)
    # Wait for exactly one IRQ. The handler disables its source and increments $00.
    code.extend([0x58,0xa5,0,0xf0,0xfc,0xc9,1,0xf0,5,0xa9,0xee])
    fail=0x8000+len(code);code.extend([0x4c,fail&255,fail>>8,0xa9,0x42])
    done=0x8000+len(code);code.extend([0x4c,done&255,done>>8])
    handler=bytes([0xa9,0,0x8d,control&255,control>>8,0xe6,0,0x40])
    prg=bytearray([0xff])*131072
    for bank in range(16):
        start=bank*8192;prg[start:start+len(code)]=code
        prg[start+0x100:start+0x108]=handler
        prg[start+8192-6:start+8192]=bytes([0,0x81,0,0x80,0,0x81])
    header=b'NES\x1a'+bytes([8,0,(mapper&15)<<4,mapper&240])+bytes(8)
    name=f'vrc_irq_{mapper}_{sub}_'+('scan' if scanline else 'cycle')+('_dma' if dma else '')+('_8bit' if width8 else '')
    case=(name,header+prg,'00:8000\n00:8100\n','final:A=42')
    return nes2(case,sub=sub,chr_ram=7)


def _vrc_fixtures():
    for mapper,sub,high,next_ in BOARDS:
        if mapper not in ACTIVE_IDS: continue
        is2=mapper==22 or sub==3
        case=handoff(f'vrc_prg_{mapper}_{sub}',mapper,[(0x8000,3)],3,prg_kb=256,chr_kb=8)
        yield nes2(case,sub=sub)
        operations=[('cpu',0xb000,10),('cpu',0xb000+high,31),
                    ('read',0,125 if mapper==22 else 250),('read',1,0 if is2 else 1)]
        if is2:
            operations += [('cpu',0x9000,3),('write',0x2000,0x55),('read',0x2400,0x55),
                           ('cpu',0x6000,1),('cpu_read',0x6000,0x61),('cpu_read',0x7000,0x70)]
        else:
            operations += [('cpu',0x9000+next_,1),('cpu',0x6000,0x55),('cpu_read',0x6000,0x55),
                           ('cpu',0x9000,3),('write',0x2000,0x56),('read',0x2c00,0x56)]
        case=ppu_contract(mapper,256,512,operations,f'_vrc{sub}')
        name,image,seeds,expected=nes2(case,sub=sub,ram=0 if is2 else 7)
        image=bytearray(image)
        for page in range(512):image[16+262144+page*1024+1]=page>>8
        yield name,bytes(image),seeds,expected
        if not is2:
            yield irq_program(mapper,sub,high,next_)
            yield irq_program(mapper,sub,high,next_,scanline=True,dma=True)
    if 73 not in ACTIVE_IDS: return
    yield handoff('vrc_prg_73',73,[(0xf000,3)],6,chr_kb=0)
    yield irq_program(73)
    yield irq_program(73,width8=True,dma=True)


def vrc_fixtures():
    for name,image,seeds,expected in _vrc_fixtures():
        yield 'vrc_'+name,image,seeds,expected
