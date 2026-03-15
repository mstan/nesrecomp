with open('C:/temp/ppu_trace.csv') as f:
    lines = f.readlines()

ppu_addr = 0
addr_latch = 0
non24_nt_writes = []
all_2007_writes = []

for line in lines[1:]:
    parts = line.strip().split(',')
    if len(parts) < 3:
        continue
    reg = parts[1]
    val = int(parts[2].replace('$',''), 16)
    frame = parts[4] if len(parts) > 4 else '?'
    frame = frame.replace('F=','')

    if reg == '$2006':
        if addr_latch == 0:
            ppu_addr = val << 8
        else:
            ppu_addr |= val
        addr_latch ^= 1
    elif reg == '$2007':
        effective = ppu_addr & 0x3FFF
        all_2007_writes.append((effective, val, frame))
        if effective < 0x3F00 and val != 0x24:
            non24_nt_writes.append((effective, val, frame))
        ppu_addr += 1  # simplified (ignores PPUCTRL bit2)

print(f'Total $2007 writes: {len(all_2007_writes)}')
print(f'Non-$24 nametable writes (to $0000-$3EFF): {len(non24_nt_writes)}')
if non24_nt_writes:
    print('--- First 20 non-$24 nametable writes ---')
    for addr, val, fr in non24_nt_writes[:20]:
        region = 'NT' if addr < 0x3F00 else 'PAL'
        print(f'  F={fr:>4} PPU=${addr:04X} ({region}) = ${val:02X}')
else:
    print('NO non-$24 nametable writes found!')
    print()
    print('--- All unique $2007 destination addresses ---')
    seen = set()
    for addr, val, fr in all_2007_writes:
        key = addr & 0xFF00
        if key not in seen:
            seen.add(key)
            print(f'  F={fr:>4} PPU base=${addr:04X} val=${val:02X}')
