// tric_prelude.inc - C# compatibility shims and the cartridge model used by
// tric_core.cpp. Included once, at the top of that translation unit.
//
// TriCNES models the 72-pin cartridge connector, and calls into the mapper at
// the points a real chip would see: FetchCPU/StoreCPU when the CPU accesses
// the cartridge, AccessPPU and FetchPatternAddress on a PPU bus cycle,
// Connector_CheckCIRAM for the nametable arrangement, and PPUClock/CPUClock/
// CPUClockRise every clock. This keeps the "non tilting" fast paths of
// TriCNES's Mapper base class - a normally seated cartridge - and implements
// the same mapper set the runtime does (hw_mapper.c).
//
// The mapper here is a second implementation, not shared code: its bank
// arithmetic comes from the same nesdev documentation, but the points at which
// it is *called* are TriCNES's. That is what makes it a useful check - above
// all for MMC3, whose IRQ counter clocks off rising edges of PPU A12, so the
// two models have to agree about when the PPU drives each address.

#include "../../common/nes_cart.h"

typedef uint8_t  byte;
typedef int8_t   sbyte;
typedef uint16_t ushort;
typedef uint32_t uint;
typedef uint64_t ulong;

#define LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

extern bool SeventyTwoPinConnector[72];
extern bool ConnectorPinFloating[72];
extern byte VRAM[0x1000];
extern byte dataBus;
extern ushort PPU_AddressBus;
extern byte PPU_OctalLatch;
extern bool IRQ_LevelDetector;
extern uint64_t totalCycles;

// How the cartridge drives CIRAM A10 (connector pin 22). Same numbering as
// HwMirroring in hw_mapper.h, so the two state dumps read alike.
enum { TRIC_MIRROR_HORIZONTAL = 0, TRIC_MIRROR_VERTICAL = 1, TRIC_MIRROR_SCREEN_A = 2, TRIC_MIRROR_SCREEN_B = 3 };

struct TricMapper {
    // ---- connector ----
    ushort CPU_AddressIn;
    byte   CPU_NonTiltData;
    bool   CPU_Drove;              // the cartridge answered this CPU read
    ushort PPU_AddressIn;
    byte   PPU_DataIn;
    byte   PPU_DataOut;
    byte   PPU_NonTiltData;

    // ---- bank tables: byte offsets, PRG in 4KB slots, CHR in 1KB pages ----
    uint PRG_Off[8];
    uint CHR_Off[8];

    // ---- work RAM at $6000-$7FFF ----
    byte WRAM[0x20000];
    uint WRAM_Length, WRAM_Bank;
    bool HasWRAM, WRAM_Readable, WRAM_Writable;

    byte Mirroring;

    // ---- MMC1 ----
    byte  Shift, ShiftCount, Ctrl, Chr0, Chr1, PrgReg;
    ulong LastWriteCycle;
    // ---- MMC3 ----
    byte BankSelect, Reg[8], MirrorReg, RamProtect;
    byte IrqLatch, IrqCounter;
    bool IrqReload, IrqEnable, IrqOut;
    bool A12;
    ulong A12LowCycle;
    // ---- UxROM / CNROM / AxROM / GxROM ----
    byte Latch;
    bool PatternPending;
    ushort PatternAddress;
    ushort VrcChr[8], VrcLatch16, VrcCounter16;
    int16_t IrqDivider;
    byte IrqMode;

    void Reset();
    void MapPRG4(uint Slot, int Bank);
    void MapPRG8(uint Slot, int Bank);
    void MapPRG16(uint Slot, int Bank) { MapPRG8(Slot * 2, Bank * 2); MapPRG8(Slot * 2 + 1, Bank * 2 + 1); }
    void MapPRG32(int Bank) { for (uint i = 0; i < 4; i++) MapPRG8(i, Bank * 4 + (int)i); }
    void MapCHR1(uint Page, int Bank);
    void MapCHR2(uint Page, int Bank) { MapCHR1(Page * 2, Bank * 2); MapCHR1(Page * 2 + 1, Bank * 2 + 1); }
    void MapCHR4(uint Page, int Bank) { for (uint i = 0; i < 4; i++) MapCHR1(Page * 4 + i, Bank * 4 + (int)i); }
    void MapCHR8(int Bank) { for (uint i = 0; i < 8; i++) MapCHR1(i, Bank * 8 + (int)i); }

    void MMC1Apply();
    void MMC1Write(ushort Address, byte Input);
    void MMC3Apply();
    void MMC3Write(ushort Address, byte Input);
    void MMC3ClockIRQ();
    void PatternReadLatch(ushort Address);
    void PatternReadEnd(bool Reading);

    void FetchCPU();
    byte SnoopCPU(ushort Address);
    byte SnoopPPU(ushort Address);
    void StoreCPU(ushort Address, byte Input);
    int  FetchPatternAddress(ushort Address);
    void AccessPPU();
    void PPUClock();
    void CPUClock();
    bool VRC6Board();
    unsigned VRC6Nametable(unsigned nt);
    void VRC6Apply();
    void VRC6Write(ushort Address, byte Input);
    bool VRC2Board();
    bool VRC24Board();
    unsigned VRCPort(ushort Address);
    void VRCApply();
    void VRCWrite(ushort Address, byte Input);
    void CPUClockRise() {}
    bool IRQ() { return IrqOut; }
    bool CheckCIC() { return true; }

    void Connector_SetUpPPUAddressPins() {
        PPU_AddressIn = (ushort)((PPU_AddressBus & 0x3F00) | PPU_OctalLatch);
        SeventyTwoPinConnector[64] = (PPU_AddressBus & 0x2000) != 0;
    }
    void Connector_ReadPPUAddressPins() {}
    void Connector_SetUpPPUDataPins(byte t) { PPU_NonTiltData = t; }
    byte Connector_ReadPPUDataPins(byte) { return PPU_NonTiltData; }
    void Connector_SetUpCPUAddressPins(ushort Address) {
        CPU_AddressIn = Address;
        SeventyTwoPinConnector[49] = (Address & 0x8000) == 0;
    }
    void Connector_ReadCPUAddressPins() {}
    void Connector_SetUpCPUDataPins(byte t) { CPU_NonTiltData = t; }
    byte Connector_ReadCPUDataPins() { return CPU_NonTiltData; }
    void Connector_CheckCIRAM();
    void Connector_PPU_RW(bool RD, bool WR) {
        SeventyTwoPinConnector[20] = !RD;
        SeventyTwoPinConnector[55] = !WR;
    }
};

struct TricCartridge {
    NesCartInfo Info;
    byte *PRGROM;
    int   PRGROM_Length;     // the image's own length
    int   PRGSlots;          // 4KB slots in the padded buffer, a power of two
    byte *CHRROM;
    int   CHRROM_Length;
    int   CHRPages;          // 1KB pages in the padded buffer, a power of two
    bool  UsingCHRRAM;
    bool  NametableHorizontalMirroring;
    bool  AlternativeNametableArrangement;
    int   Mapper;
    TricMapper MapperChip;
};

extern TricCartridge Cart;

// ---- bank tables ----
// A negative bank counts from the end of the ROM, and a bank past the end
// wraps, as a board's missing address lines make it (nesdev wiki, per mapper).

inline void TricMapper::MapPRG4(uint Slot, int Bank) {
    PRG_Off[Slot % 8] = ((uint)Bank & ((uint)Cart.PRGSlots - 1)) * 4096;
}

inline void TricMapper::MapPRG8(uint Slot, int Bank) {
    MapPRG4(Slot * 2, Bank * 2);
    MapPRG4(Slot * 2 + 1, Bank * 2 + 1);
}

inline void TricMapper::MapCHR1(uint Page, int Bank) {
    uint n = (uint)Cart.CHRPages;
    CHR_Off[Page & 7] = ((uint)Bank & (n - 1)) * 0x400u;
}

inline bool TricMapper::VRC6Board() { return Cart.Mapper == 24 || Cart.Mapper == 26; }
#include "tric_vrc.inc"
#include "tric_vrc6.inc"

// ---- MMC1 (nesdev wiki, MMC1) ----

inline void TricMapper::MMC1Apply() {
    static const byte MirrorOf[4] = { TRIC_MIRROR_SCREEN_A, TRIC_MIRROR_SCREEN_B, TRIC_MIRROR_VERTICAL,
                                      TRIC_MIRROR_HORIZONTAL };
    Mirroring = MirrorOf[Ctrl & 3];
    int prg = PrgReg & 0x0F;
    switch ((Ctrl & 0x0C) >> 2) {
    case 0: case 1: MapPRG32(prg >> 1); break;
    case 2: MapPRG16(0, 0); MapPRG16(1, prg); break;
    default: MapPRG16(0, prg); MapPRG16(1, -1); break;
    }
    if (Cart.Info.submapper == 5) MapPRG32(0);
    if (Ctrl & 0x10) {
        MapCHR4(0, Chr0 & 0x1F);
        MapCHR4(1, Chr1 & 0x1F);
    } else {
        MapCHR8((Chr0 & 0x1F) >> 1);
    }
    WRAM_Readable = WRAM_Writable = (PrgReg & 0x10) == 0;
}

inline void TricMapper::MMC1Write(ushort Address, byte Input) {
    if (totalCycles == LastWriteCycle + 1) return;   // ignores back-to-back writes
    LastWriteCycle = totalCycles;
    if (Input & 0x80) {
        Shift = ShiftCount = 0;
        Ctrl |= 0x0C;
        MMC1Apply();
        return;
    }
    Shift = (byte)(Shift >> 1 | (Input & 1) << 4);
    if (++ShiftCount < 5) return;
    byte v = Shift;
    Shift = ShiftCount = 0;
    switch ((Address >> 13) & 3) {
    case 0: Ctrl = v; break;
    case 1: Chr0 = v; break;
    case 2: Chr1 = v; break;
    default: PrgReg = v; break;
    }
    MMC1Apply();
}

// ---- MMC3 (nesdev wiki, MMC3) ----

inline void TricMapper::MMC3Apply() {
    if (BankSelect & 0x40) {
        MapPRG8(0, -2);
        MapPRG8(2, Reg[6]);
    } else {
        MapPRG8(0, Reg[6]);
        MapPRG8(2, -2);
    }
    MapPRG8(1, Reg[7]);
    MapPRG8(3, -1);
    if (Cart.Mapper == 206 && Cart.Info.submapper == 1) MapPRG32(0);
    uint big = (BankSelect & 0x80) ? 4u : 0u;
    uint small_ = big ^ 4u;
    MapCHR2(big / 2 + 0, Reg[0] >> 1);
    MapCHR2(big / 2 + 1, Reg[1] >> 1);
    MapCHR1(small_ + 0, Reg[2]);
    MapCHR1(small_ + 1, Reg[3]);
    MapCHR1(small_ + 2, Reg[4]);
    MapCHR1(small_ + 3, Reg[5]);
}

inline void TricMapper::MMC3Write(ushort Address, byte Input) {
    switch (Address & 0xE001) {
    case 0x8000: BankSelect = Input; MMC3Apply(); break;
    case 0x8001: Reg[BankSelect & 7] = Input; MMC3Apply(); break;
    case 0xA000:
        MirrorReg = Input;
        Mirroring = (Input & 1) ? TRIC_MIRROR_HORIZONTAL : TRIC_MIRROR_VERTICAL;
        break;
    case 0xA001:
        RamProtect = Input;
        WRAM_Readable = (Input & 0x80) != 0;
        WRAM_Writable = (Input & 0x80) != 0 && (Input & 0x40) == 0;
        break;
    case 0xC000: IrqLatch = Input; break;
    case 0xC001: IrqCounter = 0; IrqReload = true; break;
    case 0xE000: IrqEnable = false; IrqOut = false; break;
    default: IrqEnable = true; break;
    }
}

// MMC3B/C: reload when zero or requested, else decrement; assert on zero.
void TricMmc3TraceClock(byte counter, bool out, ushort vbus);

inline void TricMapper::MMC3ClockIRQ() {
    if (IrqCounter == 0 || IrqReload) IrqCounter = IrqLatch;
    else IrqCounter--;
    if (IrqCounter == 0 && IrqEnable) IrqOut = true;
    IrqReload = false;
    TricMmc3TraceClock(IrqCounter, IrqOut, PPU_AddressBus);
}

// The MMC3 counts rising edges of PPU A12, ignoring an edge unless A12 has
// been low for at least three CPU cycles. TriCNES calls this once per PPU
// clock with its own address bus, which is the independent part of the check.
inline void TricMapper::PPUClock() {
    if (Cart.Mapper != 4) return;
    if (!(PPU_AddressBus & 0x1000)) {
        if (A12) { A12 = false; A12LowCycle = totalCycles; }
        return;
    }
    if (!A12) {
        A12 = true;
        if (totalCycles - A12LowCycle >= 3) MMC3ClockIRQ();
    }
}

// ---- reset ----

inline void TricMapper::Reset() {
    for (int i = 0; i < 8; i++) PRG_Off[i] = 0;
    for (int i = 0; i < 8; i++) CHR_Off[i] = 0;
    for (int i = 0; i < 8; i++) Reg[i] = 0;
    Shift = ShiftCount = Ctrl = Chr0 = Chr1 = PrgReg = 0;
    LastWriteCycle = 0;
    BankSelect = MirrorReg = RamProtect = 0;
    IrqLatch = IrqCounter = 0;
    IrqReload = IrqEnable = IrqOut = false;
    A12 = false;
    A12LowCycle = 0;
    Latch = 0;
    PatternPending = false;
    PatternAddress = 0;
    memset(VrcChr, 0, sizeof(VrcChr));
    VrcLatch16 = VrcCounter16 = 0;
    IrqDivider = 341; IrqMode = 0;
    CPU_Drove = false;
    WRAM_Length = Cart.Info.prg_ram + Cart.Info.prg_nvram;
    WRAM_Bank = 0;
    memset(WRAM, 0, Cart.Info.prg_nvram ? Cart.Info.prg_ram : sizeof(WRAM));

    Mirroring = Cart.NametableHorizontalMirroring ? TRIC_MIRROR_HORIZONTAL : TRIC_MIRROR_VERTICAL;
    HasWRAM = WRAM_Length != 0;
    WRAM_Readable = WRAM_Writable = HasWRAM;

    switch (Cart.Mapper) {
    case 24: case 26: Ctrl=0x20; VRC6Apply(); break;
    case 21: case 22: case 23: case 25: Reg[1] = 1; VRCApply(); break;
    case 73: MapPRG16(0, 0); MapPRG16(1, -1); MapCHR8(0); break;
    case 31:
        for (uint i=0; i<8; ++i) MapPRG4(i, i == 7 ? 255 : 0);
        MapCHR8(0); break;
    case 9:
        MapPRG8(0, 0); MapPRG8(1, -3); MapPRG8(2, -2); MapPRG8(3, -1);
        Chr0 = Chr1 = 1; MapCHR4(0, 0); MapCHR4(1, 0); break;
    case 10:
        MapPRG16(0, 0); MapPRG16(1, -1);
        Chr0 = Chr1 = 1; MapCHR4(0, 0); MapCHR4(1, 0); break;
    case 13: MapPRG32(0); MapCHR4(0, 0); MapCHR4(1, 0); Mirroring = TRIC_MIRROR_VERTICAL; break;
    case 71: MapPRG16(0, 0); MapPRG16(1, -1); MapCHR8(0); break;
    case 75: MapPRG32(0); MapPRG8(3, -1); MapCHR4(0, 0); MapCHR4(1, 0); Mirroring = TRIC_MIRROR_VERTICAL; break;
    case 206: Reg[7] = 1; MMC3Apply(); break;
    case 76: MapPRG16(0, 0); MapPRG16(1, -1); Reg[7] = 1; for (uint j = 0; j < 4; ++j) MapCHR2(j, 0); break;
    case 94: MapPRG16(0, 0); MapPRG16(1, -1); MapCHR8(0); break;
    case 180: MapPRG16(0, 0); MapPRG16(1, 0); MapCHR8(0); break;
    case 184: MapPRG32(0); MapCHR4(0, 0); MapCHR4(1, 4); break;
    case 232: MapPRG16(0, 0); MapPRG16(1, 3); MapCHR8(0); break;
    case 1:
        Ctrl = 0x0C;   // PRG mode 3, the state a bit-7 write leaves
        MMC1Apply();
        break;
    case 2:
        MapPRG16(0, 0);
        MapPRG16(1, -1);
        MapCHR8(0);
        break;
    case 4:
        Reg[7] = 1;
        MMC3Apply();
        Mirroring = TRIC_MIRROR_VERTICAL;
        break;
    case 7:
        MapPRG32(0);
        MapCHR8(0);
        Mirroring = TRIC_MIRROR_SCREEN_A;
        break;
    default:       // NROM, CNROM, GxROM
        MapPRG32(0);
        MapCHR8(0);
        break;
    }
}

// ---- connector ----

inline void TricMapper::FetchCPU() {
    CPU_Drove = false;
    if (VRC24Board() && CPU_AddressIn >= 0x6000 && CPU_AddressIn < 0x8000) {
        if (VRC2Board() && !HasWRAM) {
            if (CPU_AddressIn < 0x7000) {
                CPU_NonTiltData = (CPU_NonTiltData & 0xfe) | Latch;
                CPU_Drove = true;
            }
            return;
        }
        if (!VRC2Board() && WRAM_Length == 2048 && CPU_AddressIn >= 0x7000) return;
    }
    if (!SeventyTwoPinConnector[49]) {    // A15 high: PRG ROM
        CPU_NonTiltData = Cart.PRGROM[PRG_Off[(CPU_AddressIn >> 12) & 7] | (CPU_AddressIn & 0xFFF)];
        CPU_Drove = true;
    } else if (CPU_AddressIn >= 0x6000 && HasWRAM && WRAM_Readable) {
        CPU_NonTiltData = WRAM[(WRAM_Bank + (CPU_AddressIn & 0x1FFF)) % WRAM_Length];
        CPU_Drove = true;
    }
}

inline byte TricMapper::SnoopCPU(ushort Address) {
    if (VRC24Board() && Address >= 0x6000 && Address < 0x8000) {
        if (VRC2Board() && !HasWRAM) return Address < 0x7000 ? (dataBus & 0xfe) | Latch : dataBus;
        if (!VRC2Board() && WRAM_Length == 2048 && Address >= 0x7000) return dataBus;
    }
    if (Address >= 0x8000) return Cart.PRGROM[PRG_Off[(Address >> 12) & 7] | (Address & 0xFFF)];
    if (Address >= 0x6000 && HasWRAM && WRAM_Readable) return WRAM[(WRAM_Bank + (Address & 0x1FFF)) % WRAM_Length];
    return dataBus;
}

inline void TricMapper::StoreCPU(ushort Address, byte Input) {
    if (VRC24Board() && Address >= 0x6000 && Address < 0x8000) {
        if (VRC2Board() && !HasWRAM) {
            if (Address < 0x7000) Latch = Input%2;
            return;
        }
        if (!VRC2Board() && WRAM_Length == 2048 && Address >= 0x7000) return;
    }
    if (Cart.Mapper == 31 && Address / 4096 == 5) {
        Reg[Address % 8] = Input;
        MapPRG4(Address % 8, Input);
        return;
    }
    if (Cart.Mapper == 34) {
        if (!nes_cart_nina(&Cart.Info) && Address >= 0x8000) {
            Latch = (byte)(Input & SnoopCPU(Address));
            MapPRG32(Latch);
        } else if (nes_cart_nina(&Cart.Info)) {
            switch (Address) {
            case 0x7ffd: MapPRG32(Input % 2); break;
            case 0x7ffe: MapCHR4(0, Input % 16); break;
            case 0x7fff: MapCHR4(1, Input % 16); break;
            }
        }
    }
    if (Cart.Mapper == 79 && (Address & 0xe100) == 0x4100) {
        Latch = Input;
        MapPRG32((Input / 8) % 2);
        MapCHR8(Input % 8);
        return;
    }
    if (Cart.Mapper == 87 && Address >= 0x6000 && Address < 0x8000) {
        MapCHR8((Input % 2) * 2 + (Input / 2) % 2);
        return;
    }
    if (Cart.Mapper == 113 && (Address & 0xe100) == 0x4100) {
        Latch = Input;
        MapPRG32((Input / 8) % 8);
        MapCHR8(Input % 8 + ((Input & 64) ? 8 : 0));
        Mirroring = (Input & 128) ? TRIC_MIRROR_VERTICAL : TRIC_MIRROR_HORIZONTAL;
        return;
    }
    if (Cart.Mapper == 140 && (Address & 0xe000) == 0x6000) {
        MapPRG32((Input / 16) % 4);
        MapCHR8(Input % 16);
        return;
    }
    if (Cart.Mapper == 184 && Address / 0x2000 == 3) {
        MapCHR4(0, Input % 8);
        MapCHR4(1, 4 + (Input / 16) % 4);
        return;
    }
    if (Address >= 0x6000 && Address < 0x8000) {
        if (HasWRAM && WRAM_Writable) WRAM[(WRAM_Bank + (Address & 0x1FFF)) % WRAM_Length] = Input;
        return;
    }
    if (Address < 0x8000) return;
    if ((Cart.Mapper == 2 || Cart.Mapper == 3 || Cart.Mapper == 7) && Cart.Info.submapper == 2)
        Input &= SnoopCPU(Address);
    switch (Cart.Mapper) {
    case 24: case 26: VRC6Write(Address, Input); break;
    case 21: case 22: case 23: case 25: case 73: VRCWrite(Address, Input); break;
    case 9: case 10:
        switch (Address / 4096) {
        case 10:
            PrgReg = Input % 16;
            if (Cart.Mapper == 9) MapPRG8(0, PrgReg); else MapPRG16(0, PrgReg);
            break;
        case 11: case 12: case 13: case 14:
            Reg[Address / 4096 - 11] = Input % 32;
            MapCHR4(0, Reg[Chr0]); MapCHR4(1, Reg[2 + Chr1]);
            break;
        case 15: Mirroring = (Input & 1) ? TRIC_MIRROR_HORIZONTAL : TRIC_MIRROR_VERTICAL; break;
        }
        break;
    case 11:
        Input = (byte)(Input & SnoopCPU(Address));
        Latch = Input;
        MapPRG32(Input % 4);
        MapCHR8(Input / 16);
        break;
    case 13:
        Latch = (byte)(Input & SnoopCPU(Address));
        MapCHR4(1, Latch % 4);
        break;
    case 71:
        if (Address >= 0xc000) MapPRG16(0, Input % 16);
        else if ((!Cart.Info.nes2 && (Address >> 12) == 9) ||
                 (Cart.Info.nes2 && Cart.Info.submapper == 1 && Address < 0xa000))
            Mirroring = (Input & 16) ? TRIC_MIRROR_SCREEN_B : TRIC_MIRROR_SCREEN_A;
        break;
    case 75:
        switch (Address >> 12) {
        case 8: MapPRG8(0, Input % 16); break;
        case 10: MapPRG8(1, Input % 16); break;
        case 12: MapPRG8(2, Input % 16); break;
        case 9:
            Ctrl = Input;
            Mirroring = (Input & 1) ? TRIC_MIRROR_HORIZONTAL : TRIC_MIRROR_VERTICAL;
            break;
        case 14: Chr0 = Input % 16; break;
        case 15: Chr1 = Input % 16; break;
        }
        MapCHR4(0, Chr0 + ((Ctrl & 2) ? 16 : 0));
        MapCHR4(1, Chr1 + ((Ctrl & 4) ? 16 : 0));
        break;
    case 206:
        if ((Address & 0xe000) == 0x8000) {
            if (Address % 2 == 0) BankSelect = Input % 8;
            else Reg[BankSelect] = Input % (BankSelect < 6 ? 64 : 16);
            MMC3Apply();
        }
        break;
    case 76:
        if (Address >= 0x8000 && Address <= 0x9fff) {
            if (Address & 1) Reg[BankSelect] = Input % 64;
            else BankSelect = Input % 8;
            MapPRG8(0, Reg[6] % 16); MapPRG8(1, Reg[7] % 16);
            MapCHR2(0, Reg[2]); MapCHR2(1, Reg[3]);
            MapCHR2(2, Reg[4]); MapCHR2(3, Reg[5]);
        }
        break;
    case 94:
        Latch = (byte)(Input & SnoopCPU(Address));
        MapPRG16(0, (Latch / 4) % 8);
        break;
    case 180:
        Latch = (byte)(Input & SnoopCPU(Address));
        MapPRG16(1, Latch % 8);
        break;
    case 232:
        if (Address >= 0xc000) PrgReg = Input % 4;
        else Ctrl = Cart.Info.submapper == 1 ? ((Input / 8) % 2) * 2 + (Input / 16) % 2 : (Input / 8) % 4;
        MapPRG16(0, Ctrl * 4 + PrgReg);
        MapPRG16(1, Ctrl * 4 + 3);
        break;
    case 1: MMC1Write(Address, Input); break;
    case 2: Latch = Input; MapPRG16(0, Input); break;
    case 3: Latch = Input; MapCHR8(Input); break;
    case 4: MMC3Write(Address, Input); break;
    case 7:
        Latch = Input;
        MapPRG32(Input & 7);
        Mirroring = (Input & 0x10) ? TRIC_MIRROR_SCREEN_B : TRIC_MIRROR_SCREEN_A;
        break;
    case 66:
        Latch = Input;
        MapPRG32((Input >> 4) & 3);
        MapCHR8(Input & 3);
        break;
    default: break;
    }
}

inline int TricMapper::FetchPatternAddress(ushort Address) {
    uint index = CHR_Off[(Address >> 10) & 7] | (Address & 0x3FF);
    return Cart.UsingCHRRAM ? index % Cart.CHRROM_Length : index;
}

inline void TricMapper::PatternReadLatch(ushort Address) {
    if (Cart.Mapper != 9 && Cart.Mapper != 10) return;
    if (!PatternPending) { PatternPending = true; PatternAddress = Address; }
}

inline void TricMapper::PatternReadEnd(bool Reading) {
    if (Reading || !PatternPending) return;
    PatternPending = false;
    ushort Address = PatternAddress;
    unsigned half = Address >> 12;
    unsigned offset = Address & 4095;
    bool range = Cart.Mapper == 10 || half == 1;
    if (offset >= 0xfd8 && offset <= (range ? 0xfdf : 0xfd8)) {
        if (half) Chr1 = 0; else Chr0 = 0;
    } else if (offset >= 0xfe8 && offset <= (range ? 0xfef : 0xfe8)) {
        if (half) Chr1 = 1; else Chr0 = 1;
    } else return;
    MapCHR4(half, Reg[half * 2 + (half ? Chr1 : Chr0)]);
}

inline byte TricMapper::SnoopPPU(ushort Address) {
    if (Address < 0x2000) return Cart.CHRROM[FetchPatternAddress(Address)];
    if (VRC6Board()) {
        unsigned bank=VRC6Nametable((Address>>10)&3);
        if (Ctrl&16) return Cart.CHRROM[(bank*1024+(Address&1023))%(Cart.UsingCHRRAM ? Cart.CHRROM_Length : Cart.CHRPages*1024)];
        return VRAM[(bank%2)*1024+(Address&1023)];
    }
    if (Cart.AlternativeNametableArrangement) return VRAM[Address & 0xfff];
    ushort Addr = (ushort)(Address & 0x3FF);
    if (Mirroring == TRIC_MIRROR_HORIZONTAL) Addr |= (ushort)((Address & 0x800) ? 0x400 : 0);
    else if (Mirroring == TRIC_MIRROR_VERTICAL) Addr |= (ushort)((Address & 0x400) ? 0x400 : 0);
    else if (Mirroring == TRIC_MIRROR_SCREEN_B) Addr |= 0x400;
    return VRAM[Addr];
}

inline void TricMapper::AccessPPU() {
    ushort Address = PPU_AddressIn;
    if (VRC6Board() && SeventyTwoPinConnector[64] && (Ctrl & 16)) {
        unsigned index=(VRC6Nametable((Address>>10)&3)*1024+(Address&1023))%(Cart.UsingCHRRAM ? Cart.CHRROM_Length : Cart.CHRPages*1024);
        PPU_DataIn=PPU_NonTiltData;
        if (!SeventyTwoPinConnector[20]) PPU_NonTiltData=PPU_DataOut=Cart.CHRROM[index];
        if (!SeventyTwoPinConnector[55] && Cart.UsingCHRRAM) Cart.CHRROM[index]=PPU_DataIn;
    }
    if (!SeventyTwoPinConnector[64]) {
        int CHR_Address = FetchPatternAddress(Address);
        PPU_DataIn = PPU_NonTiltData;
        if (!SeventyTwoPinConnector[20]) {
            PPU_DataOut = Cart.CHRROM[CHR_Address];
            PPU_NonTiltData = PPU_DataOut;
            PatternReadLatch(Address);
        }
        if (!SeventyTwoPinConnector[55] && Cart.UsingCHRRAM) {
            Cart.CHRROM[CHR_Address] = PPU_DataIn;
        }
    }
}

inline void TricMapper::Connector_CheckCIRAM() {
    SeventyTwoPinConnector[56] = (PPU_AddressBus & 0x2000) == 0;
    if (VRC6Board()) {
        if (Ctrl & 16) SeventyTwoPinConnector[56]=true;
        SeventyTwoPinConnector[21]=(VRC6Nametable((PPU_AddressBus>>10)&3)&1)!=0;
        return;
    }
    bool a10;
    if (Mirroring == TRIC_MIRROR_HORIZONTAL)    a10 = (PPU_AddressBus & 0x800) != 0;
    else if (Mirroring == TRIC_MIRROR_VERTICAL) a10 = (PPU_AddressBus & 0x400) != 0;
    else                                        a10 = Mirroring == TRIC_MIRROR_SCREEN_B;
    SeventyTwoPinConnector[21] = a10;
}
