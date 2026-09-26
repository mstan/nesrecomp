#!/usr/bin/env python3
"""Cross-process save round trips and failure atomicity, without game ROMs."""
import argparse, subprocess
from pathlib import Path

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--fixtures',type=Path,required=True)
    ap.add_argument('--interp',type=Path,required=True)
    ap.add_argument('--oracle',type=Path,required=True)
    args=ap.parse_args();root=args.fixtures.resolve();out=root/'save-tests';out.mkdir(exist_ok=True)
    suffix='.exe' if hasattr(subprocess,'CREATE_NO_WINDOW') else ''
    def exe(name):
        p=root/'build'/'Release'/(name+suffix)
        return p if p.exists() else root/'build'/(name+suffix)
    def run(executable,rom,save,trace,extra=(),success=True,save_flag='--save-file'):
        p=subprocess.run([str(executable),str(rom),'--frames','6',save_flag,str(save),
            '--hash-out',str(trace),*extra],capture_output=True,text=True,timeout=30,
            creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        trace.with_suffix('.log').write_text(p.stdout+p.stderr)
        assert (p.returncode==0)==success,(rom,save,p.returncode,p.stderr)
        return trace.read_text().splitlines()[-1] if success else ''
    checked=0
    for mapper,size,kind in ((16,256,'eeprom'),(159,128,'eeprom'),(153,8192,'sram'),
                             (157,256,'eeprom'),(157,128,'external')):
        name=f'bandai_nes2_{"eeprom" if kind=="external" else kind}_{mapper}'+('_external' if kind=='external' else '')+'_counter'
        rom=root/name/(name+'.nes')
        if not rom.exists():continue
        for mode,executable,extra in [('native',exe(name),[]),('embedded',exe(name),['--interp-only']),
                    ('standalone',args.interp.resolve(),[]),('oracle',args.oracle.resolve(),[])]:
            for align in range(4):
                save=out/f'{mapper}_{kind}_{mode}_{align}.sav';save.unlink(missing_ok=True)
                extra_align=[*extra,'--align',str(align)]
                save_flag='--datach-save-file' if mapper==157 and kind!='external' else '--save-file'
                if kind=='external':
                    internal=out/f'internal_{mode}_{align}.sav';internal.unlink(missing_ok=True)
                    extra_align+=['--datach-save-file',str(internal)]
                for counter in (0,1):
                    trace=out/f'{mapper}_{mode}_{align}_{counter}.txt'
                    assert f'A={counter:02X}' in run(executable,rom,save,trace,extra_align,save_flag=save_flag)
                    data=save.read_bytes();expected=bytearray([255])*size;expected[0 if mapper==153 else 0x23]=counter
                    assert data==expected,(save,data.hex());checked+=1
                    if kind=='external':assert internal.read_bytes()==bytes([255])*256
                for bad in (b'bad',data+b'extra'):
                    save.write_bytes(bad)
                    run(executable,rom,save,out/f'bad_{mapper}_{mode}_{align}.txt',extra_align,False,save_flag)
                    assert save.read_bytes()==bad;checked+=1
    # MMC5 internal ExRAM has a separate battery pin and is not part of the
    # header's PRG RAM count. Persist it after external NVRAM. ETROM's other
    # chip is volatile and must start fresh in each process.
    for name in ('mmc5_save','mmc5_save_mixed'):
        rom=root/name/(name+'.nes')
        if not rom.exists():continue
        for mode,executable,extra in [('native',exe(name),[]),('embedded',exe(name),['--interp-only']),
                    ('standalone',args.interp.resolve(),[]),('oracle',args.oracle.resolve(),[])]:
            for align in range(4):
                save=out/f'{name}_{mode}_{align}.sav';save.unlink(missing_ok=True)
                flags=[*extra,'--align',str(align)]
                for counter in (1,2):
                    line=run(executable,rom,save,out/f'{name}_{mode}_{align}_{counter}.txt',flags)
                    assert f'A={counter:02X}' in line,line
                    expected=bytearray(9216);expected[0]=expected[8192]=counter
                    assert save.read_bytes()==expected,(name,mode,align);checked+=1
                for bad in (bytes(8192),bytes(expected)+b'extra'):
                    save.write_bytes(bad)
                    run(executable,rom,save,out/f'{name}_{mode}_{align}_bad.txt',flags,False)
                    assert save.read_bytes()==bad;checked+=1
    # SOROM/SZROM battery-back chip 1; SXROM uses linear physical bank order.
    for board in ('sorom','szrom','sxrom'):
        name='board_mmc1_save_'+board;rom=root/name/(name+'.nes')
        if not rom.exists():continue
        size=32768 if board=='sxrom' else 8192
        for mode,executable,extra in [('native',exe(name),[]),('embedded',exe(name),['--interp-only']),
                    ('standalone',args.interp.resolve(),[]),('oracle',args.oracle.resolve(),[])]:
            for align in range(4):
                save=out/f'{name}_{mode}_{align}.sav';save.unlink(missing_ok=True)
                flags=[*extra,'--align',str(align)]
                for counter in (1,2):
                    line=run(executable,rom,save,out/f'{name}_{mode}_{align}_{counter}.txt',flags)
                    assert f'A={counter:02X}' in line,line
                    expected=bytearray(size)
                    for offset in range(0,size,8192):expected[offset]=counter
                    assert save.read_bytes()==expected,(name,mode,align);checked+=1
                for bad in (bytes(size-1),bytes(size+1)):
                    save.write_bytes(bad)
                    run(executable,rom,save,out/f'{name}_{mode}_{align}_bad.txt',flags,False)
                    assert save.read_bytes()==bad;checked+=1
    # Never allow the two differently-sized Datach regions to overwrite one
    # another through a spelling such as "./save". Also reject bad barcodes
    # before any save is written.
    name='bandai_nes2_eeprom_157_external_counter';rom=root/name/(name+'.nes')
    if rom.exists():
        for mode,executable in [('standalone',args.interp.resolve()),('oracle',args.oracle.resolve())]:
            save=out/f'alias-{mode}.sav';save.unlink(missing_ok=True)
            alias=str(out)+'/'+'.'+'/'+save.name
            run(executable,rom,save,out/f'alias-{mode}.txt',['--datach-save-file',alias],False)
            assert not save.exists();checked+=1
            for digits in ('5901234123458','abcdef12','123'):
                run(executable,rom,save,out/f'barcode-bad-{mode}.txt',['--barcode',digits],False)
                assert not save.exists();checked+=1
    # PRG + CHR NVRAM layout, including persistence through machine power-on.
    code=bytearray([0x78,0xd8,0xa2,255,0x9a])
    def store(a,v):code.extend([0xa9,v,0x8d,a&255,a>>8])
    store(0x2000,0);store(0x2001,0);code.extend([0x2c,2,32,0x10,0xfb]*2)
    code.extend([0xad,0,96,0x18,0x69,1,0x8d,0,96])
    store(0x2006,0);store(0x2006,0);code.extend([0xad,7,32]*2+[0x18,0x69,1,0x85,0])
    store(0x2006,0);store(0x2006,0);code.extend([0xa5,0,0x8d,7,32])
    pc=0x8000+len(code);code.extend([0x4c,pc&255,pc>>8])
    prg=bytearray([255])*32768;prg[:len(code)]=code;prg[-6:]=bytes([0,128])*3
    header=bytearray(b'NES\x1a'+bytes(12));header[4]=2;header[6]=2;header[7]=8;header[10]=header[11]=0x70
    rom=out/'ram-counter.nes';rom.write_bytes(header+prg)
    for mode,executable in [('standalone',args.interp.resolve()),('oracle',args.oracle.resolve())]:
        save=out/f'ram-{mode}.sav';save.unlink(missing_ok=True)
        for counter in (1,2):
            line=run(executable,rom,save,out/f'ram-{mode}-{counter}.txt')
            assert f'A={counter:02X}' in line,line
            data=save.read_bytes();assert len(data)==16384 and data[0]==data[8192]==counter
            assert data[1:8192]==bytes(8191) and data[8193:]==bytes(8191);checked+=1
    assert not list(out.glob('*.tmp-*')),'temporary saves were leaked'
    print(f'{checked} cross-process save/invalid-file checks passed')

if __name__=='__main__':main()
