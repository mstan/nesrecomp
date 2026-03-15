with open('C:/temp/ppu_trace.csv') as f:
    lines = f.readlines()

print(f'Total entries: {len(lines)-1}')
print('--- First 30 entries ---')
for l in lines[1:31]:
    print(l.strip())
print()

nt  = [l for l in lines[1:] if ',$20' in l or ',$21' in l or ',$22' in l or ',$23' in l]
pal = [l for l in lines[1:] if ',$3F' in l]
print(f'Nametable ($20xx-$23xx) writes: {len(nt)}')
print(f'Palette ($3Fxx) writes: {len(pal)}')
if nt:
    print('--- First 5 nametable writes ---')
    for l in nt[:5]:
        print(l.strip())
