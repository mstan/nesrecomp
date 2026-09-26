"""6502 programs for Bandai banking, M2 IRQs and real serial EEPROM traffic."""
from mapper_fixtures import handoff
from mapper_ppu_fixtures import ppu_contract
from cart_variant_fixtures import nes2

class Program:
    def __init__(self): self.code=bytearray();self.labels={};self.fixups=[]
    def emit(self,*data):self.code.extend(data)
    def store(self,a,v):self.emit(0xa9,v,0x8d,a&255,a>>8)
    def label(self,name):self.labels[name]=0x8000+len(self.code)
    def jump(self,name,op=0x4c):self.emit(op,0,0);self.fixups.append((len(self.code)-2,name))
    def expect(self,v):self.emit(0xc9,v,0xf0,3);self.jump('fail')
    def finish(self):
        self.label('done');self.jump('done')
        self.label('fail');self.emit(0xa9,0xee);self.label('failed');self.jump('failed')
        for offset,label in self.fixups:
            a=self.labels[label];self.code[offset:offset+2]=bytes([a&255,a>>8])
        return self.code

def image_case(name,mapper,p,sub=0,nv=0):
    code=p.finish();assert len(code)<8192-6
    prg=bytearray([255])*65536
    for bank in range(8):
        prg[bank*8192:bank*8192+len(code)]=code
        prg[(bank+1)*8192-6:(bank+1)*8192]=bytes([0,0x80])*3
    h=b'NES\x1a'+bytes([4,0,(mapper&15)<<4,mapper&240])+bytes(8)
    return nes2((name,h+prg,'00:8000\n','final:A=42'),sub=sub,ram=nv<<4,chr_ram=7)

def eeprom_program(mapper=16,counter=False,external=False):
    p=Program();p.emit(0x78,0xd8,0xa2,255,0x9a)
    sub=5 if mapper==16 else 0
    port=0x800d
    if mapper==157:
        for r in range(4):p.store(0x8000+r,0)
    def drive(v):
        if external:p.emit(0xa9,v);p.jump('external_line',0x20)
        else:p.store(port,v)
    def line(c,d):drive((32 if c else 0)|(64 if d else 0))
    def start():line(0,1);line(1,1);line(1,0);line(0,0)
    def stop():line(0,0);line(1,0);line(1,1)
    def ack(expect=0):
        drive(128);drive(160);p.emit(0xad,0,0x60,0x29,16);p.expect(expect);drive(128)
    def put(v):
        for i in range(7,-1,-1):line(0,(v>>i)&1);line(1,(v>>i)&1);line(0,(v>>i)&1)
        ack()
    def seek(a,read=False):
        start()
        if mapper==159 or external:put(a*2+int(read))
        else:
            put(0xa0);put(a)
            if read:start();put(0xa1)
    def get(continue_=False):
        p.store(0,0)
        for _ in range(8):
            drive(160);p.emit(0xad,0,0x60,0x29,16,0xc9,16,0x26,0);drive(128)
        line(0,not continue_);line(1,not continue_);line(0,not continue_)
        p.emit(0xa5,0)
    def wait():
        # More than 5 ms. EEPROM's internal timer runs during OAM DMA too.
        p.store(0x4014,2)
        p.emit(0xa0,10,0xa2,0,0xca,0xd0,0xfd,0x88,0xd0,0xf8)
    if counter:
        seek(0x23,True);get();stop()
        p.emit(0xa5,0,0x18,0x69,1,0x85,2)
        # Erased FF becomes zero; each later process sees and increments it.
        seek(0x23)
        for _ in range(8):
            p.emit(0x06,2,0xa9,0,0x90,2,0xa9,64)
            for operation in ((),(0x09,32),(0x29,64)):
                p.emit(*operation)
                if external:p.jump('external_line',0x20)
                else:p.emit(0x8d,13,128)
        ack();stop();wait();seek(0x23,True);get();stop();p.emit(0xa5,0)
    else:
        seek(0x23);put(0xa5);put(0x3c);stop();wait()
        # Page wrap writes the second byte at 0x20, not 0x24.
        seek(0x23,True);get(True);p.expect(0xa5);get();p.expect(255);stop()
        seek(0x20,True);get();p.expect(0x3c);stop();p.emit(0xa9,0x42)
    if external:
        p.jump('done')
        p.label('external_line');p.emit(0x85,3,0x29,32,0xf0,3);p.jump('external_high')
        for r in range(4):p.store(0x8000+r,0)
        p.emit(0xa5,3,0x29,192,0x8d,13,128,0xa5,3,0x60)
        p.label('external_high');p.emit(0xa5,3,0x29,192,0x8d,13,128)
        for r in range(4):p.store(0x8000+r,8)
        p.emit(0xa5,3,0x60)
    nv=1 if mapper==159 or external else 0 if mapper==157 else 2
    case=image_case(f'eeprom_{mapper}'+('_external' if external else '')+('_counter' if counter else ''),mapper,p,sub,nv)
    if counter:case=(*case[:3],'final:A=00')
    return case

def irq_program(sub=0,dma=False,mapper=16):
    p=Program();p.emit(0x78,0xd8,0xa2,255,0x9a)
    base=0x6000 if sub==4 else 0x8000
    p.store(0,0);p.store(0x4017,64)
    p.store(base+11,2);p.store(base+12,0);p.store(base+10,1)
    if dma:p.store(0x4014,2)
    p.emit(0x58,0xa5,0,0xf0,0xfc);p.expect(1);p.emit(0xa9,0x42);p.jump('done')
    p.label('handler');p.store(base+10,0);p.emit(0xe6,0,0x40)
    case=image_case(f'irq{mapper}_{sub}'+('_dma' if dma else ''),mapper,p,sub,1 if mapper==159 else 7 if mapper==153 else 0)
    name,image,seeds,expected=case;image=bytearray(image);a=p.labels['handler']
    for bank in range(8):
        end=16+(bank+1)*8192
        image[end-2:end]=bytes([a&255,a>>8])
    return name,bytes(image),seeds+f'00:{a:04x}\n',expected

def bandai_fixtures():
    cases=[]
    for sub in (0,4,5):
        base=0x6000 if sub==4 else 0x8000
        writes=[(base+8,3)]
        if sub==5:writes.append(((base^0xe000)+8,7))
        cases.append(nes2(handoff(f'bandai16_prg_{sub}',16,writes,6,prg_kb=256),sub=sub))
        ops=[('cpu',base+r,241+r) for r in range(8)]
        ops += [('read',r*1024,241+r) for r in range(8)]
        ops += [('cpu',base+9,3),('write',0x2000,0x55),('read',0x2c00,0x55)]
        cases.append(nes2(ppu_contract(16,256,256,ops,f'_bandai{sub}'),sub=sub))
        cases += [irq_program(sub),irq_program(sub,True)]
    cases += [nes2(handoff('bandai16_lowwrite',16,[(0x6008,3)],6,prg_kb=256)),
              eeprom_program(),eeprom_program(counter=True),eeprom_program(159),eeprom_program(159,True),
              irq_program(mapper=159),irq_program(mapper=159,dma=True),
              nes2(handoff('bandai159_prg',159,[(0x8008,5),(0x6008,7)],10,prg_kb=256),ram=0x10)]
    cases += list(mapper153_fixtures())
    cases += [eeprom_program(157),eeprom_program(157,True),eeprom_program(157,external=True),
              eeprom_program(157,True,True),irq_program(mapper=157),irq_program(mapper=157,dma=True)]
    cases.append(nes2(handoff('datach_prg',157,[(0x8008,5)],10,prg_kb=256,chr_kb=0),chr_ram=7))
    cases.append(nes2(ppu_contract(157,256,0,[('write',0,0xa5),('cpu',0x8000,31),
        ('read',0,0xa5),('cpu',0x8009,3),('write',0x2000,0x55),('read',0x2c00,0x55)],'_datach'),chr_ram=7))
    p=Program();p.emit(0x78,0xd8);p.store(0,0);p.store(1,0);p.store(0x800d,128)
    p.label('poll');p.emit(0xad,0,96,0x29,8,0xc5,1,0xf0,4,0x85,1,0xe6,0,0xa6,0);p.jump('poll')
    name,image,seeds,_=image_case('barcode_probe',157,p)
    cases.append((name,image,seeds,'final:X=00'))
    for name,image,seeds,expected in cases:yield 'bandai_'+name,image,seeds,expected

def mapper153_fixtures():
    for start in (0x8000,0xc000):
        for high in (0,1):
            writes=[(0x8000+r,high) for r in range(4)]+[(0x8008,3)]
            expected=high*32+(6 if start==0x8000 else 30)
            yield nes2(handoff(f'jump2_prg_{start:04x}_{high}',153,writes,expected,
                               prg_kb=512,chr_kb=0,start=start),ram=0x70,chr_ram=7)
    ops=[('cpu',0x800d,32),('cpu_read',0x6000,255),('cpu',0x6000,0xa5),
         ('cpu_read',0x6000,0xa5),('cpu',0x800d,0),('cpu',0x6000,0),
         ('cpu_read',0x6000,0x60),('cpu',0x800d,32),('cpu_read',0x6000,0xa5),
         ('cpu',0x800d,128),('cpu_read',0x7000,0x60),('cpu',0x800d,0)]
    ops += [('cpu',0x8000+r,r%2) for r in range(4)]
    for select in range(4):
        ops += [('write',0x2000+select*1024,0x55),('cpu_read',0xbf00,(select%2)*16)]
    ops += [('write',0x1400,0x5a),('read',0x1400,0x5a)]
    name,image,seeds,_=nes2(ppu_contract(153,512,0,ops,'_outer_ppu'),ram=0x70,chr_ram=7)
    image=bytearray(image)
    for bank in range(32):image[16+bank*16384+0x3f00]=bank
    yield name,bytes(image),seeds,'final:mixed:A=42'
    yield irq_program(mapper=153)
    yield irq_program(mapper=153,dma=True)
    p=Program();p.emit(0x78,0xd8);p.store(0x800d,32)
    p.emit(0xad,0,96,0x18,0x69,1,0x8d,0,96)
    name,image,seeds,_=image_case('sram_153_counter',153,p,nv=7)
    yield name,image,seeds,'final:A=00'
