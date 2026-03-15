with open('C:/temp/ppu_trace.csv') as f:
    lines = f.readlines()

ppu_addr = 0
addr_latch = 0
tile_writes = []  # writes to NT tile area ($2000-$23BF or $2400-$27BF), non-$24
attr_writes = []  # writes to attribute area ($23C0+ or $27C0+), non-$00

for line in lines[1:]:
    parts = line.strip().split(',')
    if len(parts) < 3:
        continue
    reg = parts[1]
    val = int(parts[2].replace('$',''), 16)
    frame = parts[4] if len(parts) > 4 else '?'
    frame = frame.replace('F=','').strip()

    if reg == '$2006':
        if addr_latch == 0:
            ppu_addr = val << 8
        else:
            ppu_addr |= val
        addr_latch ^= 1
    elif reg == '$2007':
        effective = ppu_addr & 0x3FFF
        in_tile_area = (0x2000 <= effective < 0x23C0) or (0x2400 <= effective < 0x27C0)
        in_attr_area = (0x23C0 <= effective < 0x2400) or (0x27C0 <= effective < 0x2800)
        in_palette   = effective >= 0x3F00

        if in_tile_area and val != 0x24:
            tile_writes.append((effective, val, frame))
        if in_attr_area and val != 0x00:
            attr_writes.append((effective, val, frame))
        ppu_addr += 1

print(f'Non-$24 tile writes to $2000-$27BF: {len(tile_writes)}')
print(f'Non-$00 attribute writes: {len(attr_writes)}')

if tile_writes:
    print('--- First 30 tile writes ---')
    for addr, val, fr in tile_writes[:30]:
        print(f'  F={fr:>4} PPU=${addr:04X} = ${val:02X}')
else:
    print('ZERO actual tile writes to nametable! Screen is always blank.')
