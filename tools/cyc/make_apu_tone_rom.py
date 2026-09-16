#!/usr/bin/env python3
"""
make_apu_tone_rom.py - an NROM program that plays known APU settings one after
another, for checking NESRecomp's audio output against the APU's documented
behavior (check_apu_tones.py) and against NES_MiSTer's APU
(tools/cyc/mister_apu). Nothing in the program depends on the result.

Segments (frames counted by NMI, each followed by 10 silent frames):

  1 pulse 1, 50% duty, period $0FD           440.4 Hz   60 frames
  2 pulse 2, 25% duty, period $07E           880.8 Hz   60 frames
  3 triangle, period $0FD                    220.2 Hz   60 frames
  4 noise, period index 4 (broadband)                   60 frames
  5 DMC sample of $F0 bytes looping, rate 15 4143.0 Hz  60 frames
  6 pulse 1, envelope period 15, no loop     decays to silence in 1 s, 90 frames
  7 pulse 1, length counter 40 half-frames   0.333 s of sound, 60 frames
  8 pulse 1, sweep down (period 2, shift 3) from $3FF                 60 frames

  python make_apu_tone_rom.py tones.nes
  cyc_interp tones.nes --frames 720 --wav-out tones.wav
  python check_apu_tones.py tones.wav
"""
import sys

SILENCE = 10
ORG = 0xC000
FRAME_COUNTER = 0x00


def build():
    code = bytearray()
    wait_refs = []

    def emit(*bs):
        code.extend(b & 0xFF for b in bs)

    def write(addr, value):
        emit(0xA9, value, 0x8D, addr & 0xFF, addr >> 8)  # LDA #v / STA abs

    def wait(frames):
        emit(0xA2, frames, 0x20, 0, 0)                   # LDX #n / JSR wait
        wait_refs.append(len(code) - 2)

    emit(0x78, 0xD8, 0xA2, 0xFF, 0x9A)                   # SEI CLD LDX #$FF TXS
    write(0x2001, 0x00)
    write(0x4017, 0x40)
    write(0x4015, 0x0F)
    write(0x2000, 0x80)                                  # NMI on: it counts frames
    wait(2)

    # 1 pulse 1
    write(0x4000, 0xBF); write(0x4001, 0x08); write(0x4002, 0xFD); write(0x4003, 0x00)
    wait(60); write(0x4000, 0xB0); wait(SILENCE)
    # 2 pulse 2, 25% duty
    write(0x4004, 0x7F); write(0x4005, 0x08); write(0x4006, 0x7E); write(0x4007, 0x00)
    wait(60); write(0x4004, 0x70); wait(SILENCE)
    # 3 triangle
    write(0x4008, 0xFF); write(0x400A, 0xFD); write(0x400B, 0x00)
    wait(60); write(0x4008, 0x80); wait(SILENCE)
    # 4 noise
    write(0x400C, 0x3F); write(0x400E, 0x04); write(0x400F, 0x00)
    wait(60); write(0x400C, 0x30); wait(SILENCE)
    # 5 DMC: 17 bytes of $F0 at $F000 (address byte $C0), loop, rate 15
    write(0x4011, 0x20); write(0x4010, 0x4F); write(0x4012, 0xC0); write(0x4013, 0x01)
    write(0x4015, 0x1F)
    wait(60); write(0x4015, 0x0F); write(0x4010, 0x00); wait(SILENCE)
    # 6 envelope: decay, no loop; length index 1 (254 half frames) outlasts it
    write(0x4000, 0x8F); write(0x4001, 0x08); write(0x4002, 0xFD); write(0x4003, 0x08)
    wait(90); write(0x4000, 0xB0); wait(SILENCE)
    # 7 length counter: constant volume, length index 4 (40 half frames)
    write(0x4000, 0x9F); write(0x4002, 0xFD); write(0x4003, 0x20)
    wait(60)
    # 8 sweep: enabled, period 2, negate, shift 3, from $3FF
    write(0x4000, 0xBF); write(0x4001, 0xAB); write(0x4002, 0xFF); write(0x4003, 0x03)
    wait(60); write(0x4000, 0xB0); write(0x4001, 0x08)
    loop = ORG + len(code)
    emit(0x4C, loop & 0xFF, loop >> 8)                   # JMP *

    # wait: X frames of NMIs
    wait_addr = ORG + len(code)
    emit(0xA5, FRAME_COUNTER)                            # w1: LDA frames
    emit(0xC5, FRAME_COUNTER)                            # w2: CMP frames
    emit(0xF0, 0xFC)                                     #     BEQ w2
    emit(0xCA)                                           #     DEX
    emit(0xD0, 0xF7)                                     #     BNE w1
    emit(0x60)                                           #     RTS
    for at in wait_refs:
        code[at] = wait_addr & 0xFF
        code[at + 1] = wait_addr >> 8

    nmi = ORG + len(code)
    emit(0xE6, FRAME_COUNTER, 0x40)                      # INC frames / RTI
    irq = ORG + len(code)
    emit(0x40)                                           # RTI

    prg = bytearray([0xEA] * 0x4000)
    prg[:len(code)] = code
    sample = bytes([0xF0] * 17)
    prg[0xF000 - ORG:0xF000 - ORG + len(sample)] = sample
    vec = 0xFFFA - ORG
    prg[vec:vec + 6] = bytes([nmi & 0xFF, nmi >> 8, ORG & 0xFF, ORG >> 8, irq & 0xFF, irq >> 8])
    header = b'NES\x1a' + bytes([1, 1, 0, 0]) + bytes(8)
    return header + bytes(prg) + bytes(0x2000)


if __name__ == '__main__':
    open(sys.argv[1] if len(sys.argv) > 1 else 'tones.nes', 'wb').write(build())
