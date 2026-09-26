"""SxROM pin-wiring tests. Expected bank numbers are physical 8 KiB offsets."""
from mapper_fixtures import handoff
from mapper_ppu_fixtures import ppu_contract
from cart_variant_fixtures import nes2

def serial(addr,value):
    return [(addr,(value>>i)&1) for i in range(5)]

def operations(addr,value):
    return [('cpu',a,v) for a,v in serial(addr,value)]

def named(case,name,mixed=False):
    _,image,seeds,expected=case
    return 'board_mmc1_'+name,image,seeds,'final:mixed:A=42' if mixed else expected

def save_fixture(board):
    code=bytearray([0x78,0xd8,0xa2,255,0x9a]);failures=[]
    def store(a,v):code.extend([0xa9,v,0x8d,a&255,a>>8])
    def bank(n):
        shift={'sorom':3,'szrom':4,'sxrom':2}[board]
        for a,v in serial(0xa000,n<<shift):store(a,v)
    if board!='sxrom':
        # Chip 0 is volatile, and must be empty after each process restart.
        code.extend([0xad,0,96,0xc9,0,0xf0,3,0x4c,0,0]);failures.append(len(code)-2)
        store(0x6000,0x77)
    for n in range(4) if board=='sxrom' else (1,):
        bank(n);code.extend([0xad,0,96,0x18,0x69,1,0x8d,0,96])
    end=0x8000+len(code);code.extend([0x4c,end&255,end>>8])
    fail=0x8000+len(code);code.extend([0xa9,0xee,0x4c,(fail+2)&255,(fail+2)>>8])
    for offset in failures:code[offset:offset+2]=bytes([fail&255,fail>>8])
    prg=bytearray([255])*262144
    for b in range(32):
        prg[b*8192:b*8192+len(code)]=code;prg[(b+1)*8192-6:(b+1)*8192]=bytes([0,128])*3
    chr_kb=64 if board=='szrom' else 0
    header=b'NES\x1a'+bytes([16,chr_kb//8,0x12,0])+bytes(8)
    return named(nes2(('save',header+prg+bytes(chr_kb*1024),''.join(f'{b:02x}:8000\n' for b in range(32)),'final:A=01'),
        ram=0x90 if board=='sxrom' else 0x77,chr_ram=0 if chr_kb else 7),'save_'+board)

def mmc1_fixtures():
    for board in ('sorom','szrom','sxrom'):yield save_fixture(board)
    ops=[('cpu',0x6000,0x51)]+operations(0xa000,16)
    ops += [('cpu_read',0x6000,0x60),('cpu',0x6000,0xee)]
    ops += operations(0xa000,0)+[('cpu_read',0x6000,0x51)]
    ops += operations(0xe000,16)+[('cpu_read',0x6000,0x60)]
    ops += operations(0xe000,0)+[('cpu_read',0x6000,0x51)]
    yield named(nes2(ppu_contract(1,128,0,ops),ram=7,chr_ram=7),'snrom')
    for mode in range(4):
        for start in (0x8000,0xc000):
            lo=[36,36,32,38][mode];hi=[38,38,38,62][mode]
            writes=serial(0xa000,16)+serial(0x8000,mode*4)+serial(0xe000,3)
            yield named(nes2(handoff('surom',1,writes,lo if start==0x8000 else hi,
                prg_kb=512,chr_kb=0,start=start),ram=7,chr_ram=7),f'surom_{mode}_{start:x}')
    for board,ram,chr_kb,shift in [('sorom',0x77,0,3),('sxrom',0x90,0,2),('szrom',0x77,64,4)]:
        ops=[];count=4 if board=='sxrom' else 2
        for bank in range(count):ops+=operations(0xa000,bank<<shift)+[('cpu',0x6000,0x51+bank)]
        for bank in range(count):ops+=operations(0xa000,bank<<shift)+[('cpu_read',0x6000,0x51+bank)]
        # In 4K mode PPU A12 also chooses the RAM chip, even outside rendering.
        ops+=operations(0xa000,0)+operations(0xc000,1<<shift)+operations(0x8000,28)
        ops += [('read',0,0),('cpu_read',0x6000,0x51),('read',0x1000,0 if not chr_kb else 0),('cpu_read',0x6000,0x52)]
        yield named(nes2(ppu_contract(1,256,chr_kb,ops),ram=ram,chr_ram=0 if chr_kb else 7),board)
    for vertical in range(2):
        ops=[]
        for mirror in range(4):
            ops+=operations(0x8000,12+mirror)+[('write',0x2000,0x51),('read',0x2800 if vertical else 0x2400,0x51)]
        case=named(nes2(ppu_contract(1,128,64,ops),sub=7),f'ks7058_{vertical}')
        name,image,seeds,expected=case;image=bytearray(image);image[6]|=vertical
        yield name,bytes(image),seeds,expected
    # The same code exists in both outer banks, but marker bytes differ. The
    # native guard must permit live PPU changes during instruction operands.
    ops=operations(0xa000,0)+operations(0xc000,16)+operations(0x8000,28)
    ops += [('read',0,0),('cpu_read',0x9e00,0),('read',0x1000,0),('cpu_read',0x9e00,32)]
    ops+=operations(0xa000,16) # restore stable ROM before the final loop
    case=nes2(ppu_contract(1,512,0,ops),ram=7,chr_ram=7)
    name,image,seeds,expected=named(case,'ppu_outer',True);image=bytearray(image)
    for bank in range(64):image[16+bank*8192+0x1e00]=bank
    yield name,bytes(image),seeds,expected
