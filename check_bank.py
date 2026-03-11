rom = open('F:/Projects/nesrecomp/baserom.nes','rb').read()
offset = 0x10 + 5*0x4000
print('Bank 5 ROM offset:', hex(offset))
chunk = rom[offset:offset+32]
print('First 32 bytes from ROM:', chunk.hex())
bank = open('F:/Projects/nesrecomp/banks/bank05.bin','rb').read()
print('First 32 bytes from bank05.bin:', bank[:32].hex())
print('Match:', rom[offset:offset+0x4000] == bank)
# Find first non-zero in bank
for i,b in enumerate(bank):
    if b:
        print('First non-zero at offset', hex(i), 'NES addr', hex(0x8000+i), 'val', hex(b))
        break
