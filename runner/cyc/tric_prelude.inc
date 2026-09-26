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

typedef uint8_t  byte;
typedef int8_t   sbyte;
typedef uint16_t ushort;
typedef uint32_t uint;
typedef uint64_t ulong;

#define LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

extern bool SeventyTwoPinConnector[72];
extern bool ConnectorPinFloating[72];
extern byte VRAM[0x800];
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

    // ---- bank tables: byte offsets, PRG in 8KB slots, CHR in 1KB pages ----
    uint PRG_Off[4];
    uint CHR_Off[8];

    // ---- work RAM at $6000-$7FFF ----
    byte WRAM[0x2000];
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

    void Reset();
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

    void FetchCPU();
    byte SnoopCPU(ushort Address);
    byte SnoopPPU(ushort Address);
    void StoreCPU(ushort Address, byte Input);
    int  FetchPatternAddress(ushort Address);
    void AccessPPU();
    void PPUClock();
    void CPUClock() {}
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
    byte *PRGROM;
    int   PRGROM_Length;     // the image's own length
    int   PRGSlots;          // 8KB slots in the padded buffer, a power of two
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

inline void TricMapper::MapPRG8(uint Slot, int Bank) {
    uint n = (uint)Cart.PRGSlots;
    PRG_Off[Slot & 3] = ((uint)Bank & (n - 1)) * 0x2000u;
}

inline void TricMapper::MapCHR1(uint Page, int Bank) {
    uint n = (uint)Cart.CHRPages;
    CHR_Off[Page & 7] = ((uint)Bank & (n - 1)) * 0x400u;
}

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
    for (int i = 0; i < 4; i++) PRG_Off[i] = 0;
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
    CPU_Drove = false;
    memset(WRAM, 0, sizeof(WRAM));

    Mirroring = Cart.NametableHorizontalMirroring ? TRIC_MIRROR_HORIZONTAL : TRIC_MIRROR_VERTICAL;
    HasWRAM = Cart.Mapper == 1 || Cart.Mapper == 4 || (Cart.Mapper == 34 && Cart.CHRPages > 8);
    WRAM_Readable = WRAM_Writable = HasWRAM;

    switch (Cart.Mapper) {
    case 13: MapPRG32(0); MapCHR4(0, 0); MapCHR4(1, 0); Mirroring = TRIC_MIRROR_VERTICAL; break;
    case 71: MapPRG16(0, 0); MapPRG16(1, -1); MapCHR8(0); break;
    case 75: MapPRG32(0); MapPRG8(3, -1); MapCHR4(0, 0); MapCHR4(1, 0); Mirroring = TRIC_MIRROR_VERTICAL; break;
    case 206: Reg[7] = 1; MMC3Apply(); break;
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
    if (!SeventyTwoPinConnector[49]) {    // A15 high: PRG ROM
        CPU_NonTiltData = Cart.PRGROM[PRG_Off[(CPU_AddressIn >> 13) & 3] | (CPU_AddressIn & 0x1FFF)];
        CPU_Drove = true;
    } else if (CPU_AddressIn >= 0x6000 && HasWRAM && WRAM_Readable) {
        CPU_NonTiltData = WRAM[CPU_AddressIn & 0x1FFF];
        CPU_Drove = true;
    }
}

inline byte TricMapper::SnoopCPU(ushort Address) {
    if (Address >= 0x8000) return Cart.PRGROM[PRG_Off[(Address >> 13) & 3] | (Address & 0x1FFF)];
    if (Address >= 0x6000 && HasWRAM && WRAM_Readable) return WRAM[Address & 0x1FFF];
    return dataBus;
}

inline void TricMapper::StoreCPU(ushort Address, byte Input) {
    if (Cart.Mapper == 34) {
        if (Cart.CHRPages <= 8 && Address >= 0x8000) {
            Latch = (byte)(Input & SnoopCPU(Address));
            MapPRG32(Latch);
        } else if (Cart.CHRPages > 8) {
            switch (Address) {
            case 0x7ffd: MapPRG32(Input % 2); break;
            case 0x7ffe: MapCHR4(0, Input % 16); break;
            case 0x7fff: MapCHR4(1, Input % 16); break;
            }
        }
    }
    if (Address >= 0x6000 && Address < 0x8000) {
        if (HasWRAM && WRAM_Writable) WRAM[Address & 0x1FFF] = Input;
        return;
    }
    if (Address < 0x8000) return;
    switch (Cart.Mapper) {
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
        else if ((Address >> 12) == 9)
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
    return (int)(CHR_Off[(Address >> 10) & 7] | (Address & 0x3FF));
}

inline byte TricMapper::SnoopPPU(ushort Address) {
    if (Address < 0x2000) return Cart.CHRROM[FetchPatternAddress(Address)];
    ushort Addr = (ushort)(Address & 0x3FF);
    if (Mirroring == TRIC_MIRROR_HORIZONTAL) Addr |= (ushort)((Address & 0x800) ? 0x400 : 0);
    else if (Mirroring == TRIC_MIRROR_VERTICAL) Addr |= (ushort)((Address & 0x400) ? 0x400 : 0);
    else if (Mirroring == TRIC_MIRROR_SCREEN_B) Addr |= 0x400;
    return VRAM[Addr];
}

inline void TricMapper::AccessPPU() {
    ushort Address = PPU_AddressIn;
    if (!SeventyTwoPinConnector[64]) {
        int CHR_Address = FetchPatternAddress(Address);
        PPU_DataIn = PPU_NonTiltData;
        if (!SeventyTwoPinConnector[20]) {
            PPU_DataOut = Cart.CHRROM[CHR_Address];
            PPU_NonTiltData = PPU_DataOut;
        }
        if (!SeventyTwoPinConnector[55] && Cart.UsingCHRRAM) {
            Cart.CHRROM[CHR_Address] = PPU_DataIn;
        }
    }
}

inline void TricMapper::Connector_CheckCIRAM() {
    SeventyTwoPinConnector[56] = (PPU_AddressBus & 0x2000) == 0;
    bool a10;
    if (Mirroring == TRIC_MIRROR_HORIZONTAL)    a10 = (PPU_AddressBus & 0x800) != 0;
    else if (Mirroring == TRIC_MIRROR_VERTICAL) a10 = (PPU_AddressBus & 0x400) != 0;
    else                                        a10 = Mirroring == TRIC_MIRROR_SCREEN_B;
    SeventyTwoPinConnector[21] = a10;
}
