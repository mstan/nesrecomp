import json

with open('C:/temp/mesen_boot.json','r') as f:
    frames = json.load(f)

print(f'Total frames captured: {len(frames)}')
if not frames:
    print('EMPTY - boot trace not yet written')
    exit()

print(f'Frame range: {frames[0]["frame"]} - {frames[-1]["frame"]}')
print()

# Print state transitions
prev = {}
for fr in frames:
    changed = False
    for key in ('submode','ppu_slot','r074E','r0756','nodisplay','ppumask','upload_hi','upload_lo'):
        if fr.get(key) != prev.get(key):
            changed = True
            break
    if changed or fr['frame'] <= frames[0]['frame'] + 2:
        print('frame=%4d submode=%d ppu_slot=%d r074E=%d r0756=%d nodisplay=%d ppumask=0x%02x upload=%s/%s' % (
            fr['frame'],
            fr.get('submode',0), fr.get('ppu_slot',0), fr.get('r074E',0),
            fr.get('r0756',0), fr.get('nodisplay',0), fr.get('ppumask',0),
            hex(fr.get('upload_hi',0)), hex(fr.get('upload_lo',0))))
        prev = dict(fr)
