// tric_core.cpp - cycle-accurate NES hardware core for NESRecomp's accuracy mode.
//
// Derived from TriCNES (https://github.com/100thCoin/TriCNES), the reference
// emulator for the AccuracyCoin test ROM.
//   MIT License. Copyright (c) 2025 Chris Siebert. See runner/cyc/LICENSE.TriCNES.
//
// The C# Emulator class was machine-translated with tools/cyc/port_tricnes.py and
// then edited by hand. Field and method names deliberately match the original so
// behavior can be compared line-by-line against upstream.
//
// It is built only as the oracle (cyc_oracle): TriCNES's complete machine,
// including its 6502 interpreter _6502(), producing the reference trace
// (cyc_trace.h) and memory hashes that NESRecomp's own CPU and hardware
// (cpu6502.c, hw_machine.c, hw_ppu.c, hw_apu.c) are compared with. Nothing in
// a NESRecomp runtime build uses this file.
//
// Oracle-only corrections (ORACLE FIX comments) change TriCNES where it departs
// from documented 6502 behavior; each cites its reference.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#endif

extern "C" {

#include "cyc_core.h"
#include "cyc_trace.h"
#include "hw.h"
#include "tric_prelude.inc"

bool SeventyTwoPinConnector[72];
bool ConnectorPinFloating[72];
TricCartridge Cart;

// NESRecomp: what CPU RAM holds at power-on (cyc_core.h), defined by each
// hardware implementation rather than by a host, so that every host - the
// cosimulation harnesses in tools/cyc included - links without it.
CycRamInit cyc_ram_init = CYC_RAM_PATTERN;
static uint32_t cyc_framebuffer[256 * 240];
static uint16_t cyc_frame_index_buffer[256 * 240]; // NESRecomp: color | emphasis << 6

// TriCNES's end-of-frame flag is the hw.h one.
#define FrameAdvance_ReachedVBlank hw_frame_done

// ---- fields ----
byte PPUClock;
byte CPUClock;
byte MasterClock;
byte CICClock;
bool ResetMode;
int ResetModeCounter;
/* unsized array dropped: bool[] SeventyTwoPinConnector; */
/* unsized array dropped: bool[] ConnectorPinFloating; */
byte APUAlignment;
bool APU_PutCycle = false;
byte OAM[0x100];
byte OAM2[32];
byte SecondaryOAMSize = 0;
byte OAM2Address = 0;
byte SpriteEvaluationTick = 0;
bool OAMAddressOverflowedDuringSpriteEvaluation = false;
byte RAM[0x800];
byte VRAM[0x800];
byte PaletteRAM[0x20];
ushort programCounter = 0;
byte opCode = 0;
uint64_t totalCycles; // NESRecomp: int upstream
byte stackPointer = 0x00;
bool flag_Carry;
bool flag_Zero;
bool flag_Interrupt;
bool flag_Decimal;
bool flag_Overflow;
bool flag_Negative;
byte status = 0;
byte A = 0;
byte X = 0;
byte Y = 0;
byte H = 0;
bool IgnoreH;
byte dataBus = 0;
byte internalBus = 0;
ushort addressBus = 0;
byte specialBus = 0;
byte dl = 0;
byte operationCycle = 0;
bool CPU_SYNC = false;
ushort temporaryAddress;
static const uint NesPalInts[] = {
            
            
            
            0xFF656565, 0xFF002A84, 0xFF1513A2, 0xFF3A019E, 0xFF59007A, 0xFF6A003E, 0xFF680800, 0xFF531D00, 0xFF323400, 0xFF0D4600, 0xFF004F00, 0xFF004C09, 0xFF003F4B, 0xFF000000, 0xFF000000, 0xFF000000,
            0xFFAEAEAE, 0xFF175FD6, 0xFF4341FF, 0xFF7529FA, 0xFF9E1DCA, 0xFFB4207B, 0xFFB13322, 0xFF964E00, 0xFF6A6C00, 0xFF398400, 0xFF0F9000, 0xFF008D33, 0xFF007B8C, 0xFF000000, 0xFF000000, 0xFF000000,
            0xFFFEFFFF, 0xFF66AFFF, 0xFF9390FF, 0xFFC578FF, 0xFFEE6CFF, 0xFFFF6FCA, 0xFFFF8271, 0xFFE69E25, 0xFFBABC00, 0xFF88D501, 0xFF5EE132, 0xFF47DD82, 0xFF4ACBDC, 0xFF4E4E4E, 0xFF000000, 0xFF000000,
            0xFFFEFFFF, 0xFFC0DEFF, 0xFFD2D1FF, 0xFFE7C7FF, 0xFFF8C2FF, 0xFFFFC3E9, 0xFFFFCBC4, 0xFFF5D7A5, 0xFFE2E394, 0xFFCEED96, 0xFFBCF2AA, 0xFFB3F1CB, 0xFFB4E9F0, 0xFFB6B6B6, 0xFF000000, 0xFF000000,
            
            0xFF66423E, 0xFF000D58, 0xFF150075, 0xFF380075, 0xFF560058, 0xFF670027, 0xFF680000, 0xFF530D00, 0xFF341E00, 0xFF102B00, 0xFF003000, 0xFF002B00, 0xFF001C24, 0xFF000000, 0xFF000000, 0xFF000000,
            0xFFAF7E78, 0xFF19379A, 0xFF4320C1, 0xFF720FC1, 0xFF9A089A, 0xFFB10F59, 0xFFB2220F, 0xFF963700, 0xFF6C4D00, 0xFF3D5F00, 0xFF166500, 0xFF005F0C, 0xFF004B55, 0xFF000000, 0xFF000000, 0xFF000000,
            0xFFFFC0B8, 0xFF6878DB, 0xFF9361FF, 0xFFC24FFF, 0xFFEA49DB, 0xFFFF4F99, 0xFFFF634E, 0xFFE77808, 0xFFBC8F00, 0xFF8DA000, 0xFF65A708, 0xFF4DA04A, 0xFF4C8D95, 0xFF4F2F2B, 0xFF000000, 0xFF000000,
            0xFFFFC0B8, 0xFFC1A2C6, 0xFFD399D6, 0xFFE792D6, 0xFFF78FC6, 0xFFFF92AB, 0xFFFF9A8C, 0xFFF6A26F, 0xFFE4AC5F, 0xFFD1B35F, 0xFFC0B66F, 0xFFB7B38B, 0xFFB6ABA9, 0xFFB7857E, 0xFF000000, 0xFF000000,
            
            0xFF395D2C, 0xFF002452, 0xFF000D6A, 0xFF140064, 0xFF2D0041, 0xFF3E0010, 0xFF3F0300, 0xFF301800, 0xFF162F00, 0xFF004200, 0xFF004C00, 0xFF004700, 0xFF003924, 0xFF000000, 0xFF000000, 0xFF000000,
            0xFF71A360, 0xFF005691, 0xFF1939B1, 0xFF4020A9, 0xFF61127B, 0xFF78183A, 0xFF792C00, 0xFF654800, 0xFF426600, 0xFF1B7E00, 0xFF008D00, 0xFF00860A, 0xFF007254, 0xFF000000, 0xFF000000, 0xFF000000,
            0xFFAEF099, 0xFF32A3CB, 0xFF5684EB, 0xFF7E6BE3, 0xFF9E5DB5, 0xFFB66472, 0xFFB77728, 0xFFA39400, 0xFF7FB200, 0xFF57CB00, 0xFF37D900, 0xFF1FD342, 0xFF1EBF8D, 0xFF27471C, 0xFF000000, 0xFF000000,
            0xFFAEF099, 0xFF7BD0AD, 0xFF8AC3BA, 0xFF9AB9B7, 0xFFA8B3A4, 0xFFB1B689, 0xFFB2BE6A, 0xFFAACA50, 0xFF9BD643, 0xFF8BE146, 0xFF7DE65A, 0xFF74E475, 0xFF73DC94, 0xFF77AA65, 0xFF000000, 0xFF000000,
            
            0xFF3F3F25, 0xFF000B46, 0xFF00005D, 0xFF18005A, 0xFF2F003F, 0xFF40000E, 0xFF410000, 0xFF320A00, 0xFF191A00, 0xFF002800, 0xFF002F00, 0xFF002A00, 0xFF001B1C, 0xFF000000, 0xFF000000, 0xFF000000,
            0xFF797A55, 0xFF003581, 0xFF201F9F, 0xFF450D9C, 0xFF640478, 0xFF7B0A36, 0xFF7C1E00, 0xFF683200, 0xFF474900, 0xFF225B00, 0xFF036400, 0xFF005D00, 0xFF004A4A, 0xFF000000, 0xFF000000, 0xFF000000,
            0xFFBABB8B, 0xFF3E75B7, 0xFF605ED6, 0xFF854CD2, 0xFFA443AE, 0xFFBB4A6C, 0xFFBD5D21, 0xFFA87200, 0xFF878900, 0xFF619B00, 0xFF42A400, 0xFF2B9D34, 0xFF2A8A7F, 0xFF2C2D15, 0xFF000000, 0xFF000000,
            0xFFBABB8B, 0xFF879E9D, 0xFF9595AA, 0xFFA48DA8, 0xFFB18999, 0xFFBB8C7E, 0xFFBB945F, 0xFFB39D48, 0xFFA5A63B, 0xFF96AE3D, 0xFF89B14C, 0xFF7FAF67, 0xFF7FA686, 0xFF80805A, 0xFF000000, 0xFF000000,
            
            0xFF47477C, 0xFF001A8C, 0xFF0B0AA9, 0xFF2900A3, 0xFF410081, 0xFF4D004A, 0xFF49000D, 0xFF340400, 0xFF141500, 0xFF002800, 0xFF003300, 0xFF00331B, 0xFF002A58, 0xFF000000, 0xFF00000A, 0xFF00000A,
            0xFF8584CD, 0xFF0B49E2, 0xFF3533FF, 0xFF5D1AFF, 0xFF7D0CD4, 0xFF8D0B8B, 0xFF86173A, 0xFF6B2C00, 0xFF414200, 0xFF195B00, 0xFF006904, 0xFF006A4C, 0xFF005E9E, 0xFF00000A, 0xFF00000A, 0xFF00000A,
            0xFFC9C8FF, 0xFF4E8CFF, 0xFF7876FF, 0xFFA05CFF, 0xFFC14EFF, 0xFFD14DE4, 0xFFCB5A92, 0xFFAF6E4C, 0xFF848525, 0xFF5C9E2D, 0xFF3BAD5B, 0xFF2BADA5, 0xFF32A1F7, 0xFF343362, 0xFF00000A, 0xFF00000A,
            0xFFC9C8FF, 0xFF96AFFF, 0xFFA8A6FF, 0xFFB89BFF, 0xFFC696FF, 0xFFCC95FF, 0xFFCA9AEA, 0xFFBEA3CD, 0xFFACACBD, 0xFF9CB7C0, 0xFF8FBDD3, 0xFF88BDF2, 0xFF8BB8FF, 0xFF8B8AD6, 0xFF00000A, 0xFF00000A,
            
            0xFF46344C, 0xFF00085C, 0xFF0B007A, 0xFF260077, 0xFF3D005C, 0xFF4A0030, 0xFF480000, 0xFF340000, 0xFF140F00, 0xFF001D00, 0xFF002400, 0xFF002200, 0xFF001829, 0xFF000000, 0xFF000000, 0xFF000000,
            0xFF846B8C, 0xFF0A30A1, 0xFF3419C8, 0xFF5907C5, 0xFF7800A1, 0xFF880166, 0xFF860E23, 0xFF6B2300, 0xFF403900, 0xFF1C4C00, 0xFF005400, 0xFF00521A, 0xFF00445C, 0xFF000000, 0xFF000000, 0xFF000000,
            0xFFC7A7D2, 0xFF4C6BE8, 0xFF7754FF, 0xFF9C42FF, 0xFFBB39E7, 0xFFCC3CAB, 0xFFCA4968, 0xFFAE5E23, 0xFF837500, 0xFF5E8700, 0xFF3F9023, 0xFF2E8E5F, 0xFF3080A2, 0xFF332338, 0xFF000000, 0xFF000000,
            0xFFC7A7D2, 0xFF948EDB, 0xFFA685EB, 0xFFB57DEA, 0xFFC27ADB, 0xFFC97BC2, 0xFFC880A7, 0xFFBD898A, 0xFFAB927A, 0xFF9C9A7B, 0xFF8F9D8A, 0xFF889CA3, 0xFF8997BE, 0xFF8A7093, 0xFF000000, 0xFF000000,
            
            0xFF304144, 0xFF00155A, 0xFF000471, 0xFF11006B, 0xFF2A0049, 0xFF36001C, 0xFF350000, 0xFF250300, 0xFF0C1300, 0xFF002600, 0xFF003100, 0xFF002F00, 0xFF002531, 0xFF000000, 0xFF000000, 0xFF000000,
            0xFF647D80, 0xFF00429E, 0xFF152CBC, 0xFF3C13B4, 0xFF5C0586, 0xFF6D074B, 0xFF6B1509, 0xFF572900, 0xFF364000, 0xFF0E5900, 0xFF006700, 0xFF006424, 0xFF005766, 0xFF000000, 0xFF000000, 0xFF000000,
            0xFF9EBEC3, 0xFF2D83E1, 0xFF4E6CFF, 0xFF7653F8, 0xFF9745C9, 0xFFA7478D, 0xFFA5554A, 0xFF916A12, 0xFF6F8100, 0xFF479A00, 0xFF27A82A, 0xFF16A566, 0xFF1898A9, 0xFF1F2E30, 0xFF000000, 0xFF000000,
            0xFF9EBEC3, 0xFF6FA6CF, 0xFF7D9CDC, 0xFF8E92D8, 0xFF9B8CC5, 0xFFA28DAD, 0xFFA19391, 0xFF999C7A, 0xFF8BA56D, 0xFF7AAF70, 0xFF6DB584, 0xFF66B49C, 0xFF67AEB8, 0xFF6A8386, 0xFF000000, 0xFF000000,
            
            0xFF343434, 0xFF00084B, 0xFF000061, 0xFF14005F, 0xFF2B0044, 0xFF380017, 0xFF360000, 0xFF270000, 0xFF0E0F00, 0xFF001D00, 0xFF002400, 0xFF002200, 0xFF001721, 0xFF000000, 0xFF000000, 0xFF000000,
            0xFF6A6A6A, 0xFF003088, 0xFF1B19A7, 0xFF4007A3, 0xFF5F007F, 0xFF6F0144, 0xFF6D0E02, 0xFF592300, 0xFF383900, 0xFF134B00, 0xFF005400, 0xFF00520F, 0xFF004451, 0xFF000000, 0xFF000000, 0xFF000000,
            0xFFA6A6A6, 0xFF356BC5, 0xFF5654E3, 0xFF7B42E0, 0xFF9B39BB, 0xFFAB3C80, 0xFFA9493D, 0xFF955E04, 0xFF737500, 0xFF4E8700, 0xFF2F900E, 0xFF1E8E4A, 0xFF20808D, 0xFF232323, 0xFF000000, 0xFF000000,
            0xFFA6A6A6, 0xFF788EB3, 0xFF8585C0, 0xFF957DBE, 0xFFA279AF, 0xFFA87A96, 0xFFA8807B, 0xFF9F8964, 0xFF919257, 0xFF829A59, 0xFF759D68, 0xFF6E9C80, 0xFF6F979C, 0xFF707070, 0xFF000000, 0xFF000000,
            
            0xFF010900
        };
int chosenColor;
bool Logging;
bool LoggingPPU;
bool PPU_RESET;
bool CPU_Read;
bool DoBRK;
bool DoNMI;
bool DoIRQ;
bool DoReset;
bool DoOAMDMA;
bool FirstCycleOfOAMDMA;
bool DoDMCDMA;
byte DMCDMADelay;
byte CannotRunDMCDMARightNow = 0;
byte DMAPage;
byte DMAAddress;
bool FrameAdvance_ReachedVBlank;
bool APU_ControllerPortsStrobing;
bool APU_ControllerPortsStrobed;
byte ControllerPort1;
byte ControllerPort2;
byte ControllerShiftRegister1;
byte ControllerShiftRegister2;
byte Controller1ShiftCounter;
byte Controller2ShiftCounter;
bool LagFrame;
bool TASTimelineClockFiltering;
int CycleCountForCycleTAS = 0;
bool APU_Status_DMCInterrupt;
bool APU_Status_FrameInterrupt;
bool APU_Status_DMC;
bool APU_Status_DelayedDMC;
bool APU_Status_Noise;
bool APU_Status_Triangle;
bool APU_Status_Pulse2;
bool APU_Status_Pulse1;
bool Clearing_APU_FrameInterrupt;
byte APU_DelayedDMC4015;
bool APU_ImplicitAbortDMC4015;
bool APU_SetImplicitAbortDMC4015;
byte APU_Register[0x10];
bool APU_FrameCounterMode;
bool APU_FrameCounterInhibitIRQ;
byte APU_FrameCounterReset = 0xFF;
ushort APU_Framecounter = 0;
bool APU_QuarterFrameClock = false;
bool APU_HalfFrameClock = false;
bool APU_Envelope_StartFlag = false;
bool APU_Envelope_DividerClock = false;
byte APU_Envelope_DecayLevel = 0;
byte APU_LengthCounter_Pulse1 = 0;
byte APU_LengthCounter_Pulse2 = 0;
byte APU_LengthCounter_Triangle = 0;
byte APU_LengthCounter_Noise = 0;
static const byte APU_LengthCounterLUT[] = { 10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14, 12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30 };
bool APU_LengthCounter_HaltPulse1 = false;
bool APU_LengthCounter_HaltPulse2 = false;
bool APU_LengthCounter_HaltTriangle = false;
bool APU_LengthCounter_HaltNoise = false;
bool APU_LengthCounter_ReloadPulse1 = false;
bool APU_LengthCounter_ReloadPulse2 = false;
bool APU_LengthCounter_ReloadTriangle = false;
bool APU_LengthCounter_ReloadNoise = false;
byte APU_LengthCounter_ReloadValuePulse1 = 0;
byte APU_LengthCounter_ReloadValuePulse2 = 0;
byte APU_LengthCounter_ReloadValueTriangle = 0;
byte APU_LengthCounter_ReloadValueNoise = 0;
ushort APU_ChannelTimer_Pulse1 = 0;
ushort APU_ChannelTimer_Pulse2 = 0;
ushort APU_ChannelTimer_Triangle = 0;
ushort APU_ChannelTimer_Noise = 0;
ushort APU_ChannelTimer_DMC = 0;
bool APU_DMC_EnableIRQ = false;
bool APU_DMC_Loop = false;
ushort APU_DMC_Rate = 428;
static const ushort APU_DMCRateLUT[] = { 428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54 };
byte APU_DMC_Output;
ushort APU_DMC_SampleAddress = 0xC000;
ushort APU_DMC_SampleLength = 0;
ushort APU_DMC_BytesRemaining = 0;
byte APU_DMC_Buffer = 0;
ushort APU_DMC_AddressCounter = 0xC000;
byte APU_DMC_Shifter = 0;
byte APU_DMC_ShifterBitsRemaining = 8;
bool DPCM_Up;
bool APU_Silent = true;
byte PPUBus;
int PPUBusDecay[8];
const int PPUBusDecayConstant = 1786830;
byte PPUOAMAddress;
bool PPUStatus_VBlank;
bool PPUStatus_PendingSpriteZeroHit;
bool PPUStatus_PendingSpriteZeroHit2;
bool PPUStatus_SpriteZeroHit;
bool PPUStatus_SpriteZeroHit_Delayed;
bool PPUStatus_SpriteOverflow;
bool PPUStatus_SpriteOverflow_Delayed;
bool PPU_VSET;
bool PPU_VSET_Latch1;
bool PPU_VSET_Latch2;
bool PPU_Read2002;
bool PPU_Spritex16;
ushort PPU_Scanline;
ushort PPU_Dot;
bool PPU_VRegisterChangedOutOfVBlank;
bool PPU_OAMCorruptionRenderingDisabledOutOfVBlank;
bool PPU_PendingOAMCorruption;
byte PPU_OAMCorruptionIndex;
bool PPU_OAMCorruptionRenderingDisabledOutOfVBlank_Instant;
bool PPU_OAMCorruptionRenderingEnabledOutOfVBlank;
bool PPU_OAMEvaluationCorruptionOddCycle;
bool PPU_OAMEvaluationObjectInRange;
bool PPU_OAMEvaluationObjectInXRange;
bool PPU_PaletteCorruptionRenderingDisabledOutOfVBlank;
byte PPU_AttributeLatchRegister;
ushort PPU_BackgroundAttributeShiftRegisterL;
ushort PPU_BackgroundAttributeShiftRegisterH;
ushort PPU_BackgroundPatternShiftRegisterL;
ushort PPU_BackgroundPatternShiftRegisterH;
byte PPU_FineXScroll;
byte PPU_SpriteShiftRegisterL[8];
byte PPU_SpriteShiftRegisterH[8];
byte PPU_SpriteAttribute[8];
byte PPU_SpriteAttr;
byte PPU_SpritePattern;
byte PPU_SpriteXposition[8];
byte PPU_SpriteShifterCounter[8];
bool PPU_NextScanlineContainsSpriteZero;
bool PPU_CurrentScanlineContainsSpriteZero;
byte PPU_SpritePatternL;
byte PPU_SpritePatternH;
bool PPU_Mask_Greyscale;
bool PPU_Mask_8PxShowBackground;
bool PPU_Mask_8PxShowSprites;
bool PPU_Mask_ShowBackground;
bool PPU_Mask_ShowSprites;
bool PPU_Mask_EmphasizeRed;
bool PPU_Mask_EmphasizeGreen;
bool PPU_Mask_EmphasizeBlue;
bool PPU_Mask_ShowBackground_Delayed;
bool PPU_Mask_ShowSprites_Delayed;
bool PPU_Mask_ShowBackground_Instant;
bool PPU_Mask_ShowSprites_Instant;
byte PPU_RenderingCounter;
byte PPU_LowBitPlane;
byte PPU_HighBitPlane;
byte PPU_Attribute;
ushort PPU_PatternAddressRegister_CHR;
ushort PPU_PatternAddressRegister_NT;
ushort PPU_PatternAddressRegister_AT;
ushort PPU_PAR_MUX;
bool PPU_CanDetectSpriteZeroHit;
bool PPU_OddFrame;
byte DotColor;
byte PrevDotColor;
byte PrevPrevDotColor;
byte PrevPrevPrevDotColor;
int PrevPrevPrevPrevDotColor;
byte PaletteRAMAddress;
bool ThisDotReadFromPaletteRAM;
bool NMI_PinsSignal;
bool NMI_PreviousPinsSignal;
bool IRQ_LevelDetector;
bool NMILine;
bool IRQLine;
bool CopyV = false;
bool SkippedPreRenderDot341 = false;
bool PPUActiveForShiftRegisterUpdate;
bool PPU_2007_Read;
bool PPU_2007_Read_SR;
bool PPU_2007_Read_Latches[5];
bool PPU_2007_PD_RB;
bool PPU_2007_ReadALE;
bool PPU_2007_Read_H0_Latch;
bool PPU_2007_Read_XRB;
bool PPU_READ;
bool PPU_2007_Write;
bool PPU_2007_Write_SR;
bool PPU_2007_Write_Latches[5];
bool PPU_2007_DB_PAR;
bool PPU_2007_WriteALE;
bool PPU_2007_TStep_Latch;
bool PPU_2007_TStep;
bool PPU_2007_BLNK_Latch;
bool PPU_2007_PaletteRAMEnable;
byte PPU_2007_WriteData;
bool PPU_WRITE;
bool PPU_DecodeSignal;
bool PPU_ShowScreenBorders;
bool PPU_ShowRawNTSCSignal;
bool OamCorruptedOnOddCycle;
byte PPU_OAMBuffer_In;
byte PPU_OAMBuffer;
byte PPU_OAMLatch;
ushort PPU_OAM_VerticalOffset;
bool NineObjectsOnThisScanline;
bool OAM2Overflowed;
byte OAM2ResetSignal;
byte PPU_RenderTemp;
bool PPU_Commit_NametableFetch;
bool PPU_Commit_AttributeFetch;
bool PPU_Commit_PatternLowFetch;
bool PPU_Commit_PatternHighFetch;
byte DecayBitmask[] = { 0xFE, 0xFD, 0xFB, 0xF7, 0xEF, 0xDF, 0xBF, 0x7F };
bool OAMDMA_Aligned = false;
bool OAMDMA_Halt = false;
bool DMCDMA_Halt = false;
byte OAM_InternalBus;
ushort OAMAddressBus;
ushort PPU_VRAM_MysteryAddress;
ushort PPU_AddressBus;
bool PPU_ALE;
byte PPU_OctalLatch;
ushort PPU_v = 0;
ushort PPU_t = 0;
byte PPU_Update2006Delay;
byte PPU_Update2005Delay;
byte PPU_Update2005Value;
byte PPU_Update2001Value;
ushort PPU_Update2006Value;
ushort PPU_Update2006Value_Temp;
byte PPU_Update2001Delay;
byte PPU_Update2001OAMCorruptionDelay;
byte PPU_Update2001EmphasisBitsDelay;
bool PPU_WasRenderingBefore2001Write;
byte PPU_ReadBuffer = 0;
bool PPUAddrLatch = false;
bool PPUControlIncrementMode32;
bool PPUControl_NMIEnabled;
bool PPU_PatternSelect_Sprites;
bool PPU_PatternSelect_Background;
bool PPU_EXT_Enable;
bool PPU_PendingVBlank;
bool dataPinsAreNotFloating = false;
bool TAS_ReadingTAS;
int TAS_InputSequenceIndex;
/* unsized array dropped: ushort[] TAS_InputLog; */
/* unsized array dropped: bool[] TAS_ResetLog; */
bool ClockFiltering = false;
bool SyncFM2;
bool FixHighByte = false;
ushort DebugRange_Low = 0x0000;
ushort DebugRange_High = 0xFFFF;
bool OnlyDebugInRange = false;

// ---- prototypes ----
void Emulator_Init();
void Reset();
void _EmulatorCore();
void CoreTickAfterCPU();
void EmulateUntilEndOfRead();
void EmulateNMasterClockCycles(int n);
void _EmulateAPU();
void _EmulatePPU();
byte FetchVideoMemory();
void WriteVideoMemory(byte input);
void _EmulateHalfPPU();
void PPU_DATA_StateMachine();
void PPU_DATA_StateMachine2();
void PPU_DATA_StateMachine_Half();
void DrawToScreen();
void CorruptOAM();
void IncrementOAM2Address();
void PPU_Render_SpriteEvaluation();
void PPU_Render_CalculatePixel(bool borders);
void CorruptPalettes(byte Color, byte Palette);
void PPU_Render_ShiftRegistersAndBitPlanes();
void PPU_Render_CommitShiftRegistersAndBitPlanes();
void PPU_Render_ShiftRegistersAndBitPlanes_DummyNT();
void PPU_CheckPAR();
byte Flip(byte b);
void PPU_UpdateBackgroundShiftRegisters();
void UpdateSpriteShiftRegisters();
void PPU_LoadShiftRegisters();
void PPU_IncrementScrollX();
void PPU_IncrementScrollY();
void PPU_ResetXScroll();
void PPU_ResetYScroll();
void DecayPPUDataBus();
void OAMDMA_Get();
void OAMDMA_Halted();
void OAMDMA_Put();
void DMCDMA_Get();
void DMCDMA_Halted();
void DMCDMA_Put();
void PollInterrupts();
void PollInterrupts_CantDisableIRQ();
void CompleteOperation();
bool DMAWantsCycle();
void RunDMACycle();
void EndOfCPUCycle();
void _6502();
void ResetReadPush();
void Push(byte A);
byte Observe(ushort Address);
byte Fetch(ushort Address);
byte ObservePPU(ushort Address);
byte MapperObserve(ushort Address);
void MapperFetchPRG(ushort Address);
byte ReadOAM();
void Store(byte Input, ushort Address);
void StorePPURegisters(ushort Addr, byte In);
void StartDMCSample();
void GetImmediate();
void GetAddressAbsolute();
void GetAddressZeroPage();
void GetAddressIndOffX();
void GetAddressIndOffY(bool TakeExtraCycleOnlyIfPageBoundaryCrossed);
void GetAddressZPOffX();
void GetAddressZPOffY();
void GetAddressAbsOffX(bool TakeExtraCycleIfPageBoundaryCrossed);
void GetAddressAbsOffY(bool TakeExtraCycleIfPageBoundaryCrossed);
void Op_ORA(byte Input);
void Op_ASL(byte Input, ushort Address);
void Op_ASL_A();
void Op_SLO(byte Input, ushort Address);
void Op_AND(byte Input);
void Op_ROL(byte Input, ushort Address);
void Op_ROL_A();
void Op_RLA(byte Input, ushort Address);
void Op_EOR(byte Input);
void Op_LSR(byte Input, ushort Address);
void Op_LSR_A();
void Op_SRE(byte Input, ushort Address);
void Op_ADC(byte Input);
void Op_ROR(byte Input, ushort Address);
void Op_ROR_A();
void Op_RRA(byte Input, ushort Address);
void Op_CMP(byte Input);
void Op_CPY(byte Input);
void Op_CPX(byte Input);
void Op_SBC(byte Input);
void Op_INC(ushort Address);
void Op_DEC(ushort Address);

// ---- methods ----
void Emulator_Init()
{
            memset(RAM, 0, sizeof(RAM));
            A = 0;  // The A, X, and Y registers are all initialized with 0 when the console boots up.
            X = 0;
            Y = 0;
            memset(VRAM, 0, sizeof(VRAM));
            memset(OAM, 0, sizeof(OAM));
            for (int oam2_init = 0; oam2_init < LEN(OAM2); oam2_init++)
            {
                OAM2[oam2_init] = 0xFF;
            }

            memset(SeventyTwoPinConnector, 0, sizeof(SeventyTwoPinConnector));
            memset(ConnectorPinFloating, 0, sizeof(ConnectorPinFloating));
            // set up RAM and PPU RAM Pattern
            int i = 0;
            while (i < 0x800)
            {
                int j = i & 0x2;
                bool swap = (i & 0x1F) >= 0x10;
                if (j < 0x2 == !swap)
                {
                    VRAM[i] = 0xF0;
                    RAM[i] = 0xF0;
                }
                else
                {
                    VRAM[i] = 0x0F;
                    RAM[i] = 0x0F;
                }
                /* --ram-init: power CPU RAM up the way another emulator does,
                 * so a program that reads uninitialized RAM can be compared
                 * against it (cyc_core.h). VRAM keeps the measured pattern. */
                if (cyc_ram_init != CYC_RAM_PATTERN)
                {
                    RAM[i] = cyc_ram_init == CYC_RAM_ONES ? 0xFF : 0x00;
                }
                i++;
            }

            bool BlarggPalette = false; // There's a PPU test cartridge that expects a very specific palette when you power on the console.
            if (BlarggPalette)
            {
                //use the palette that Blargg's NES uses
                PaletteRAM[0x00] = 0x09;
                PaletteRAM[0x01] = 0x01;
                PaletteRAM[0x02] = 0x00;
                PaletteRAM[0x03] = 0x01;
                PaletteRAM[0x04] = 0x00;
                PaletteRAM[0x05] = 0x02;
                PaletteRAM[0x06] = 0x02;
                PaletteRAM[0x07] = 0x0D;
                PaletteRAM[0x08] = 0x08;
                PaletteRAM[0x09] = 0x10;
                PaletteRAM[0x0A] = 0x08;
                PaletteRAM[0x0B] = 0x24;
                PaletteRAM[0x0C] = 0x00;
                PaletteRAM[0x0D] = 0x00;
                PaletteRAM[0x0E] = 0x04;
                PaletteRAM[0x0F] = 0x2C;
                PaletteRAM[0x10] = 0x09;
                PaletteRAM[0x11] = 0x01;
                PaletteRAM[0x12] = 0x34;
                PaletteRAM[0x13] = 0x03;
                PaletteRAM[0x14] = 0x00;
                PaletteRAM[0x15] = 0x04;
                PaletteRAM[0x16] = 0x00;
                PaletteRAM[0x17] = 0x14;
                PaletteRAM[0x18] = 0x08;
                PaletteRAM[0x19] = 0x3A;
                PaletteRAM[0x1A] = 0x00;
                PaletteRAM[0x1B] = 0x02;
                PaletteRAM[0x1C] = 0x00;
                PaletteRAM[0x1D] = 0x20;
                PaletteRAM[0x1E] = 0x2C;
                PaletteRAM[0x1F] = 0x08;
            }
            else // Except my actual console has a different palette than Blargg, so I use this palette instead.
            {
                // use the palette that my NES uses
                PaletteRAM[0x00] = 0x00;
                PaletteRAM[0x01] = 0x00;
                PaletteRAM[0x02] = 0x28;
                PaletteRAM[0x03] = 0x00;
                PaletteRAM[0x04] = 0x00;
                PaletteRAM[0x05] = 0x08;
                PaletteRAM[0x06] = 0x00;
                PaletteRAM[0x07] = 0x00;
                PaletteRAM[0x08] = 0x00;
                PaletteRAM[0x09] = 0x01;
                PaletteRAM[0x0A] = 0x01;
                PaletteRAM[0x0B] = 0x20;
                PaletteRAM[0x0C] = 0x00;
                PaletteRAM[0x0D] = 0x08;
                PaletteRAM[0x0E] = 0x00;
                PaletteRAM[0x0F] = 0x02;
                PaletteRAM[0x10] = 0x00;
                PaletteRAM[0x11] = 0x00;
                PaletteRAM[0x12] = 0x00;
                PaletteRAM[0x13] = 0x00;
                PaletteRAM[0x14] = 0x00;
                PaletteRAM[0x15] = 0x02;
                PaletteRAM[0x16] = 0x21;
                PaletteRAM[0x17] = 0x00;
                PaletteRAM[0x18] = 0x00;
                PaletteRAM[0x19] = 0x00;
                PaletteRAM[0x1A] = 0x00;
                PaletteRAM[0x1B] = 0x00;
                PaletteRAM[0x1C] = 0x00;
                PaletteRAM[0x1D] = 0x10;
                PaletteRAM[0x1E] = 0x00;
                PaletteRAM[0x1F] = 0x00;
            }

            programCounter = 0xFFFF; // Technically, this value is nondeterministic. It also doesn't matter where it is, as it will be initialized in the RESET instruction.
            PPU_Scanline = 0;        // The PPU begins on dot 0 of scanline 0
            PPU_Dot = 0;

            PPU_OddFrame = true;    // And this is technically considered an "odd" frame when it comes to even/odd frame timing.

            APU_DMC_SampleAddress = 0xC000;
            APU_DMC_AddressCounter = 0xC000;

            APU_DMC_SampleLength = 1;
            APU_DMC_ShifterBitsRemaining = 8;

            switch (APUAlignment & 4)
            {
                default:
                case 0:
                    {
                        APU_ChannelTimer_DMC = 1022;
                        APU_PutCycle = true;
                    }
                    break;
                case 1:
                    {
                        APU_ChannelTimer_DMC = 1022;
                        APU_PutCycle = false;
                    }
                    break;
                case 2:
                    {
                        APU_ChannelTimer_DMC = 1020;
                        APU_PutCycle = true;
                    }
                    break;
                case 3:
                    {
                        APU_ChannelTimer_DMC = 1020;
                        APU_PutCycle = false;
                    }
                    break;
            }

            DoReset = true; // This is used to force the first instruction at power on to be the RESET instruction.
            PPU_RESET = false; // I'm not even 100% certain my console has this behavior. I'll set it to false for now.
        }

void Reset()
{
            // The A, X, and Y registers are unchanged through reset.
            // most flags go unchanged as well, but the I flag is set to 1
            flag_Interrupt = true;
            // Triangle phase gets reset, though I'm not yet emulating audio.
            APU_DMC_Output &= 1;
            // All the bits of $4015 are cleared
            APU_Status_DMCInterrupt = false;
            APU_Status_FrameInterrupt = false;
            APU_Status_DelayedDMC = false;
            APU_Status_DMC = false;
            APU_Status_Noise = false;
            APU_Status_Triangle = false;
            APU_Status_Pulse2 = false;
            APU_Status_Pulse1 = false;
            APU_DMC_BytesRemaining = 0;
            APU_LengthCounter_Noise = 0;
            APU_LengthCounter_Triangle = 0;
            APU_LengthCounter_Pulse2 = 0;
            APU_LengthCounter_Pulse1 = 0;
            APU_Framecounter = 0; // reset the frame counter

            // PPU registers
            PPUControl_NMIEnabled = false;
            PPUControlIncrementMode32 = false;
            PPU_Spritex16 = false;
            PPU_PatternSelect_Sprites = false;
            PPU_PatternSelect_Background = false;
            PPU_t = 0;

            PPU_Mask_Greyscale = false;
            PPU_Mask_EmphasizeRed = false;
            PPU_Mask_EmphasizeGreen = false;
            PPU_Mask_EmphasizeBlue = false;
            PPU_Mask_8PxShowBackground = false;
            PPU_Mask_8PxShowSprites = false;
            PPU_Mask_ShowBackground = false;
            PPU_Mask_ShowSprites = false;

            PPU_Update2005Delay = 0;
            PPU_FineXScroll = 0;

            //$2006 is unchanged

            PPU_ReadBuffer = 0;
            PPU_OddFrame = false;

            PPU_Dot = 0;
            PPU_Scanline = 0;

            DoDMCDMA = false;
            DoOAMDMA = false;
            operationCycle = 0;

            switch (APUAlignment & 4)
            {
                default:
                case 0:
                    {
                        APU_ChannelTimer_DMC = 1022;
                        APU_PutCycle = true;
                    }
                    break;
                case 1:
                    {
                        APU_ChannelTimer_DMC = 1022;
                        APU_PutCycle = false;
                    }
                    break;
                case 2:
                    {
                        APU_ChannelTimer_DMC = 1020;
                        APU_PutCycle = true;
                    }
                    break;
                case 3:
                    {
                        APU_ChannelTimer_DMC = 1020;
                        APU_PutCycle = false;
                    }
                    break;
            }

            DoReset = true;
            PPU_RESET = false; // I'm not even 100% certain my console has this behavior. I'll set it to false for now.
            // in theory, the CPU/PPU clock would be given random values. Let's just assume no changes.
        }

void _EmulatorCore()
{
            // counters count down to 0, run the appropriate chip's logic, and the counter is reset.
            // If multiple counters read 0 at the same time, there's an order of events.
            // The order of events:
            // CPU
            // PPU
            // APU

            if (CPUClock == 12)
            {
                CPUClock = 0; // there is 1 CPU cycle for every 12 master clock cycles

                if(ResetMode)
                {
                    ResetModeCounter++;
                    if(ResetModeCounter == 1477840) // Approximately how long it takes for the CIC chip to reset the console. Keep in mind, this value lowers as the temperature increases.
                    {
                        ResetModeCounter = 0;
                        Reset(); // Reset via the CIC, not the reset button.
                    }
                }

                _6502(); // This is where I run the CPU
                totalCycles++;         // for debugging mostly
                Cart.MapperChip.CPUClock(); // If the mapper chip does every cpu cycle... (see FME-7)
            }
            CoreTickAfterCPU();
}

// ORACLE FIX (IRQ). The 2A03's IRQ output is the frame interrupt flag (unless
// inhibited) OR the DMC interrupt flag, and acknowledging one source leaves the
// other asserted (nesdev wiki, APU: reading $4015 clears the frame flag "but not
// the DMC interrupt flag"; NES_MiSTer rtl/apu.sv `IRQ = frame_irq || DmcIrq`).
// TriCNES keeps a single IRQ_LevelDetector that both sources set and every
// acknowledgment clears, so a $4010 or $4015 write dips the line for one sample
// under a pending frame IRQ, and a $4015 read or an inhibiting $4017 write drops
// a pending DMC IRQ from the line while $4015 bit 7 still reads 1 (an IRQ
// handler that checks $4015 before acknowledging the DMC is not re-entered).
// Here IRQ_LevelDetector carries the frame interrupt alone, with its measured
// timing, and the DMC flag is ORed in where the CPU samples the line.
// CYC_ORACLE_UNFIXED=IRQ restores the original behavior.
static int oracle_irq_fix = -1;
static bool OracleIRQFix()
{
            if (oracle_irq_fix < 0) {
                const char *unfixed = getenv("CYC_ORACLE_UNFIXED");
                oracle_irq_fix = !(unfixed && strstr(unfixed, "IRQ"));
            }
            return oracle_irq_fix != 0;
}

// NESRecomp: the remainder of a master clock tick, after the CPU has (or has not)
// acted on it. Split out of _EmulatorCore so recompiled code can perform the CPU's
// work itself and then let the rest of the machine advance in the same order.
void CoreTickAfterCPU()
{
            if (CPUClock == 4)
            {
                NMILine |= PPUControl_NMIEnabled && PPUStatus_VBlank;
                if (operationCycle == 0 && !(PPUStatus_VBlank && PPUControl_NMIEnabled))
                {
                    NMILine = false;
                }
            }
            if (CPUClock == 7) //M2 going low.
            {
                // NESRecomp: ORACLE FIX (IRQ), and /IRQ is shared with the
                // cartridge (connector pin 50, open drain), so a mapper's
                // interrupt holds the line alongside the 2A03's.
                IRQLine = IRQ_LevelDetector || (OracleIRQFix() && APU_Status_DMCInterrupt) ||
                          Cart.MapperChip.IRQ();
                if (APU_Status_FrameInterrupt && !APU_FrameCounterInhibitIRQ)
                {
                    IRQ_LevelDetector = true; // if the APU frame counter flag is never cleared, you will get another IRQ when the I flag is cleared.
                }
                Cart.MapperChip.CPUClockRise(); // If the mapper chip does something when M2 rises... (see MMC3)
            }
            if (PPUClock == 4)
            {
                PPUClock = 0; // there is 1 PPU cycle for every 12 master clock cycles

                _EmulatePPU();
                if (PPUBus != 0)
                {
                    DecayPPUDataBus();
                }
            }
            if (PPUClock == 2)
            {
                _EmulateHalfPPU();
            }
            

            if (CPUClock == 0)
            {

                _EmulateAPU();
                APU_PutCycle = !APU_PutCycle;

                // the APU is actually clocked every 24 master clock cycles.
                // yet there's a lot of timing that happens every cpu cycle anyway??
                // If the timing needs to be exactly n and a half APU cycles, then I'll just multiply the numbers by 2 and clock this twice as fast.
            }

            if(CICClock == 5) // The CIC clock is NOT tied to the master clock. It would actually clock approximately every 5.369318 master clock cycles, but I'm not going to worry about that for now.
            {
                CICClock = 0;
                if (!Cart.MapperChip.CheckCIC())
                {
                    ResetMode = true;
                }
            }

            // Increment the clocks.
            PPUClock++;
            CPUClock++;
            CICClock++;
        }

void EmulateUntilEndOfRead()
{
            // this is used during reads from some ppu registers.
            // run 1.75 ppu cycles. (the actual duty cycle here would result in 1 and 7/8 ppu cycles, but my emulator doesn't worry about half-master-clock-cycles.
            for (int i = 0; i < 7; i++)
            {
                _EmulatorCore();
            }
        }

void EmulateNMasterClockCycles(int n)
{
            // This does run the risk of recursion, so don't use a value of 12 or more with this.
            for (int i = 0; i < n; i++)
            {
                _EmulatorCore();
            }
        }

void _EmulateAPU()
{
            // This runs every 12 master clock cycles, though has different logic for even/odd CPU cycles.
            if (!APU_ControllerPortsStrobing)
            {
                if (Controller1ShiftCounter > 0)
                {
                    Controller1ShiftCounter--;
                    if (Controller1ShiftCounter == 0)
                    {
                        ControllerShiftRegister1 <<= 1;
                        ControllerShiftRegister1 |= 1;
                    }
                }
                if (Controller2ShiftCounter > 0)
                {
                    Controller2ShiftCounter--;
                    if (Controller2ShiftCounter == 0)
                    {
                        ControllerShiftRegister2 <<= 1;
                        ControllerShiftRegister2 |= 1;
                    }
                }
            }
            else
            {
                Controller1ShiftCounter = 0;
                Controller2ShiftCounter = 0;
            }

            if (!APU_PutCycle)
            {
                // If this is a get cycle, transitioning to a put cycle.

                // controller reading is handled here in the APU chip.

                // If a 1 was written to $4016, we are strobing the controller.
                if (APU_ControllerPortsStrobing)
                {
                    if (!APU_ControllerPortsStrobed)
                    {
                        LagFrame = false;
                        APU_ControllerPortsStrobed = true;
                        // this will be reset to false if:
                        // 1.) the controllers are un-strobed. Ready for the next strobe.
                        // 2.) the controller ports are read, while still strobed. This allows data to be streamed in through the A button.
                        // (NESRecomp: TriCNES's TAS input log playback is omitted; hosts write ControllerPort1/2.)

                        // this sets up the shift registers with the value of the controller ports.
                        // If not set by the TAS, these are probably set outside this script in the script for the form.
                        ControllerShiftRegister1 = ControllerPort1;
                        ControllerShiftRegister2 = ControllerPort2;
                    }
                }
                else
                {
                    APU_ControllerPortsStrobed = false;
                }

                // clock timers
                APU_ChannelTimer_Pulse1--; // every APU GET cycle.
                APU_ChannelTimer_Pulse2--;
                APU_ChannelTimer_Noise--;


                //this happens whether a sample is playing or not
                APU_ChannelTimer_DMC--;
                APU_ChannelTimer_DMC--; // the table is in CPU cycles, but the count is in APU cycles
                if (APU_ChannelTimer_DMC == 0)
                {
                    APU_ChannelTimer_DMC = APU_DMC_Rate;
                    DPCM_Up = (APU_DMC_Shifter & 1) == 1;
                    if (DPCM_Up)
                    {
                        if (APU_DMC_Output <= 125) // this is 7 bit, and cannot go above 127
                        {
                            APU_DMC_Output += 2;
                        }
                    }
                    else
                    {
                        if (APU_DMC_Output >= 2) // this is 7 bit, and cannot go below 0
                        {
                            APU_DMC_Output -= 2;
                        }
                    }
                    APU_DMC_Shifter >>= 1; // shift the bits in the shift register
                    APU_DMC_ShifterBitsRemaining--; // and decrement the "bits remaining" counter.
                    if (APU_DMC_ShifterBitsRemaining == 0) // If there are no bits left,
                    {
                        APU_DMC_ShifterBitsRemaining = 8; // it's time for a DMC DMA!

                        if (APU_DMC_BytesRemaining > 0 || APU_SetImplicitAbortDMC4015)
                        {
                            if (!DoDMCDMA && CannotRunDMCDMARightNow != 2)
                            {
                                // if playing a sample:
                                DoDMCDMA = true;
                                DMCDMA_Halt = true;
                            }
                            if (APU_SetImplicitAbortDMC4015)
                            {
                                APU_ImplicitAbortDMC4015 = true; // check for weird DMA abort behavior
                                APU_SetImplicitAbortDMC4015 = false;
                            }
                            APU_DMC_Shifter = APU_DMC_Buffer; // and set up the shifter with the new values.
                            APU_Silent = false; // The APU is not silent.

                        }
                        else
                        {
                            APU_Silent = true;
                        }
                    }
                }
                if (CannotRunDMCDMARightNow > 0)
                {
                    CannotRunDMCDMARightNow -= 2;
                }
            }
            else
            {
                // If this is a put cycle, transitioning to a get cycle.

                if (Clearing_APU_FrameInterrupt)
                {
                    Clearing_APU_FrameInterrupt = false;
                    APU_Status_FrameInterrupt = false;
                    IRQ_LevelDetector = false;
                }
                // DMC load from 4015
                if (DMCDMADelay > 0)
                {
                    DMCDMADelay--; // there's a small delay beetween the write occurring and the DMA beginning
                    if (DMCDMADelay == 0 && !DoDMCDMA) // if the DMA is already happening because of the timer
                    {
                        DoDMCDMA = true;
                        DMCDMA_Halt = true;
                        APU_DMC_Shifter = APU_DMC_Buffer;
                        APU_Silent = false;
                    }
                }
            }
            if (APU_DelayedDMC4015 > 0)
            {
                APU_DelayedDMC4015--;
                if (APU_DelayedDMC4015 == 0)
                {
                    APU_Status_DMC = APU_Status_DelayedDMC;
                    if (!APU_Status_DMC)
                    {
                        APU_DMC_BytesRemaining = 0;
                    }
                }
            }

            APU_ChannelTimer_Triangle--; // every CPU cycle.

            // clock sequencer
            if ((APU_FrameCounterReset & 0x80) == 0)
            {
                APU_FrameCounterReset--;
                if ((APU_FrameCounterReset & 0x80) != 0)
                {
                    APU_Framecounter = 0;
                }
            }

            APU_Framecounter++;

            // We're clocking the APU twice as fast in order to get the frame counter timing to allow the 'half APU cycle' timing.
            // these numbers are just multiplied by 2.

            if (APU_FrameCounterMode)
            {
                // 5 step
                switch (APU_Framecounter)
                {
                    default: break;
                    case 7457:
                        APU_QuarterFrameClock = true;
                        break;
                    case 14913:
                        APU_QuarterFrameClock = true;
                        APU_HalfFrameClock = true;
                        break;
                    case 22371:
                        APU_QuarterFrameClock = true;
                        break;
                    case 29829:
                        break;
                    case 37281:
                        APU_QuarterFrameClock = true;
                        APU_HalfFrameClock = true;
                        break;
                    case 37282:
                        APU_Framecounter = 0;
                        break;
                }
            }
            else
            {
                // 4 step
                switch (APU_Framecounter)
                {
                    default: break;
                    case 7457:
                        APU_QuarterFrameClock = true;
                        break;
                    case 14913:
                        APU_QuarterFrameClock = true;
                        APU_HalfFrameClock = true;
                        break;
                    case 22371:
                        APU_QuarterFrameClock = true;
                        break;
                    case 29828:
                        APU_Status_FrameInterrupt = true;
                        break;
                    case 29829:
                        APU_QuarterFrameClock = true;
                        APU_Status_FrameInterrupt = true;
                        IRQ_LevelDetector |= !APU_FrameCounterInhibitIRQ;
                        APU_HalfFrameClock = true;
                        break;
                    case 29830:
                        APU_Status_FrameInterrupt = !APU_FrameCounterInhibitIRQ;
                        IRQ_LevelDetector |= !APU_FrameCounterInhibitIRQ;

                        APU_Framecounter = 0;

                        break;
                }

            }





            // perform quarter frame / half frame stuff

            if (APU_QuarterFrameClock)
            {
                APU_QuarterFrameClock = false;
                if (APU_Envelope_StartFlag)
                {
                    APU_Envelope_StartFlag = false;
                    APU_Envelope_DecayLevel = 15;

                }
                else
                {
                    APU_Envelope_DividerClock = true;
                }
            }

            if (APU_HalfFrameClock)
            {
                if (APU_LengthCounter_ReloadPulse1 && APU_LengthCounter_Pulse1 == 0) { APU_LengthCounter_Pulse1 = APU_LengthCounter_ReloadValuePulse1; } else { APU_LengthCounter_ReloadPulse1 = false; }
                if (APU_LengthCounter_ReloadPulse2 && APU_LengthCounter_Pulse2 == 0) { APU_LengthCounter_Pulse2 = APU_LengthCounter_ReloadValuePulse2; } else { APU_LengthCounter_ReloadPulse2 = false; }
                if (APU_LengthCounter_ReloadTriangle && APU_LengthCounter_Triangle == 0) { APU_LengthCounter_Triangle = APU_LengthCounter_ReloadValueTriangle; } else { APU_LengthCounter_ReloadTriangle = false; }
                if (APU_LengthCounter_ReloadNoise && APU_LengthCounter_Noise == 0) { APU_LengthCounter_Noise = APU_LengthCounter_ReloadValueNoise; } else { APU_LengthCounter_ReloadNoise = false; }
                APU_HalfFrameClock = false;
                // length counters and sweep
                if (!APU_Status_Pulse1) { APU_LengthCounter_Pulse1 = 0; }
                if (!APU_Status_Pulse2) { APU_LengthCounter_Pulse2 = 0; }
                if (!APU_Status_Triangle) { APU_LengthCounter_Triangle = 0; }
                if (!APU_Status_Noise) { APU_LengthCounter_Noise = 0; }

                if (APU_LengthCounter_Pulse1 != 0 && !APU_LengthCounter_HaltPulse1 && !APU_LengthCounter_ReloadPulse1)
                {
                    APU_LengthCounter_Pulse1--;
                }
                if (APU_LengthCounter_Pulse2 != 0 && !APU_LengthCounter_HaltPulse2 && !APU_LengthCounter_ReloadPulse2)
                {
                    APU_LengthCounter_Pulse2--;
                }
                if (APU_LengthCounter_Triangle != 0 && !APU_LengthCounter_HaltTriangle && !APU_LengthCounter_ReloadTriangle)
                {
                    APU_LengthCounter_Triangle--;
                }
                if (APU_LengthCounter_Noise != 0 && !APU_LengthCounter_HaltNoise && !APU_LengthCounter_ReloadNoise)
                {
                    APU_LengthCounter_Noise--;
                }
            }
            else
            {
                if (APU_LengthCounter_ReloadPulse1) { APU_LengthCounter_Pulse1 = APU_LengthCounter_ReloadValuePulse1; }
                if (APU_LengthCounter_ReloadPulse2) { APU_LengthCounter_Pulse2 = APU_LengthCounter_ReloadValuePulse2; }
                if (APU_LengthCounter_ReloadTriangle) { APU_LengthCounter_Triangle = APU_LengthCounter_ReloadValueTriangle; }
                if (APU_LengthCounter_ReloadNoise) { APU_LengthCounter_Noise = APU_LengthCounter_ReloadValueNoise; }
                APU_LengthCounter_ReloadPulse1 = false;
                APU_LengthCounter_ReloadPulse2 = false;
                APU_LengthCounter_ReloadTriangle = false;
                APU_LengthCounter_ReloadNoise = false;
            }

            APU_LengthCounter_HaltPulse1 = ((APU_Register[0] & 0x20) != 0);
            APU_LengthCounter_HaltPulse2 = ((APU_Register[4] & 0x20) != 0);
            APU_LengthCounter_HaltTriangle = ((APU_Register[8] & 0x80) != 0);
            APU_LengthCounter_HaltNoise = ((APU_Register[0xC] & 0x20) != 0);



        }

void _EmulatePPU()
{

            // When writing to ppu registers, there's a slight delay before resulting action is taken.
            // This delay can vary depending on the CPU/PPU alignment.

            // after writing to $2005, there is either a 1 or 2 cycle delay.
            if (PPU_Update2005Delay > 0)
            {
                PPU_Update2005Delay--;
                if (PPU_Update2005Delay == 0)
                {
                    if (!PPUAddrLatch)
                    {
                        // if this is the first write to $2005
                        PPU_FineXScroll = (byte)(PPU_Update2005Value & 7); // This updates the fine X scroll
                        PPU_t = (ushort)((PPU_t & 0x7FE0) | (PPU_Update2005Value >> 3)); // as well as changing the 't' register.
                    }
                    else
                    {
                        // if this is the second write to $2005
                        PPU_t = (ushort)((PPU_t & 0xC1F) | (((PPU_Update2005Value & 0xF8) << 2) | ((PPU_Update2005Value & 7) << 12))); // this also writes to 't'
                    }
                    PPUAddrLatch = !PPUAddrLatch; // flip the latch
                }
            }

            // Updating the scroll registers during screen rendering
            if (PPU_Scanline < 240 || PPU_Scanline == 261)// if this is the pre-render line, or any line before vblank
            {
                if ((PPU_Mask_ShowBackground || PPU_Mask_ShowSprites) && PPU_RenderingCounter >= 1)
                {
                    if (PPU_Dot == 256) //The Y scroll is incremented on dot 256.
                    {
                        PPU_IncrementScrollY();
                    }
                    else if (PPU_Dot == 257) //The X scroll is reset on dot 257.
                    {
                        PPU_ResetXScroll();
                    }
                    if (PPU_Dot >= 280 && PPU_Dot <= 304 && PPU_Scanline == 261) //numbers from the nesdev wiki
                    {
                        PPU_ResetYScroll(); //The Y scroll is reset on every dot from 280 through 304 on the pre-render scanline.
                    }
                }
            }

            // Increment the PPU dot
            PPU_Dot++;
            if (PPU_Dot > 340) // There are only 341 dots per scanline
            {
                PPU_Dot = 0;  // reset the dot back to 0
                PPU_Scanline++;     // and increment the scanline
                // Sprite zero hits rely on the previous scanline's sprite evaluation.

                if (PPU_Scanline > 261) // There are 262 scanlines in a frame.
                {
                    PPU_Scanline = 0;   // reset to scanline 0.
                }
            }

            if (PPU_Scanline == 241) // If this is the first scanline of VBLank
            {
                if (PPU_Dot == 0)
                {
                    // If Address $2002 is read during the next ppu cycle, the PPU Status flags aren't set.
                    // These variables are used to check if Address $2002 is read during the next ppu cycle.
                    // I usually refer to this as the $2002 race condition.
                    // The more proper term would be "Vblank/NMI flag suppression".

                    // oh- and also if we're running a fm2 TAS file, due to FCEUX's incorrect timing of the first frame, I need to prevent this from being set just a few cycles after power on.
                    if (!SyncFM2)
                    {
                        PPU_PendingVBlank = true;
                    }
                    else
                    {
                        SyncFM2 = false;
                    }
                }
                if (PPU_Dot == 1)
                {
                    PPU_RESET = false;

                    // else, address $2002 was read on this ppu cycle. no VBlank flag.
                    if (!PPU_ShowScreenBorders)
                    {
                        FrameAdvance_ReachedVBlank = true; // Emulator specific stuff. Used for frame advancing to detect the frame has ended, and nothing else.
                    }
                }

            }
            else if (PPU_Scanline == 242 && PPU_Dot == 1)
            {
                if (PPU_ShowScreenBorders && !(PPU_DecodeSignal || PPU_ShowRawNTSCSignal)) // if we're showing the boarders, we need to wait for 2 more scanlines to render.
                {
                    FrameAdvance_ReachedVBlank = true; // Emulator specific stuff. Used for frame advancing to detect the frame has ended, and nothing else.
                }
            }
            else if (PPU_Scanline == 260 && PPU_Dot == 340)
            {
                PPU_OddFrame = !PPU_OddFrame; // I guess this could happen on pretty much any cycle?

            }
            else if (PPU_Scanline == 261 && PPU_Dot == 1)
            {
                // On dot 1 of the pre-render scanline, all of these flags are cleared.
                // You might be looking at the results of my "$2002 Flag Clear Timing" test from the AccuracyCoin test ROM and thinking, "Hold on. That can't be right!"
                // Well, it is. You see, PPUStatus_VBlank is read at the beginning of the read, while PPUStatus_SpriteZeroHit and PPUStatus_SpriteOverflow are read at the end of the read.
                // This means about 1 and 7/8 ppu cycles pass between the start of the read and the end, so thes values are seemingly cleared on different cycles, but they are in-fact cleared at the same time.
                PPUStatus_VBlank = false;
                PPU_CanDetectSpriteZeroHit = true;
                PPUStatus_SpriteZeroHit = false;
                PPUStatus_SpriteOverflow = false;
                PPUStatus_SpriteZeroHit_Delayed = false;
            }

            else if (PPU_Scanline == 261 && PPU_Dot == 0)
            {
                if (PPU_ShowScreenBorders && (PPU_DecodeSignal || PPU_ShowRawNTSCSignal)) // if we're showing the boarders, we need to wait for scanline 0.
                {
                    FrameAdvance_ReachedVBlank = true; // Emulator specific stuff. Used for frame advancing to detect the frame has ended, and nothing else.
                }
            }

            PPU_VSET_Latch1 = !PPU_VSET; //  VSET_Latch1 is latched with /VSET on the first half of a PPU cycle.
            if (PPU_VSET && !PPU_VSET_Latch2)
            {
                PPUStatus_VBlank = true;
            }
            if (PPU_Read2002)
            {
                PPU_Read2002 = false;
                PPUStatus_VBlank = false;
            }

            PPUStatus_SpriteOverflow_Delayed = PPUStatus_SpriteOverflow;

            // NESRecomp: Cart.MapperChip.PPUClock() used to be called here, but
            // this point is before the dot's PPU_AddressBus is driven (below,
            // and in the fetch cases), so a mapper watching A12 would see the
            // previous dot's address. It is called at the end of _EmulatePPU
            // instead, where the pins hold what this dot put out.

            if (PPU_OddFrame && (PPU_Mask_ShowBackground || PPU_Mask_ShowSprites))
            {
                if (PPU_Scanline == 0 && PPU_Dot == 0)
                {
                    // On every other frame, dot 0 of scanline 0 is skipped.
                    // this cycle is technically (0,0), but this still makes the Nametable fetch during the last cycle of the pre-render line
                    PPU_Dot++;
                    SkippedPreRenderDot341 = true;
                }
            }


            if (PPU_OddFrame && (PPU_Mask_ShowBackground || PPU_Mask_ShowSprites) && PPU_Scanline == 0 && PPU_Dot == 2)
            {
                SkippedPreRenderDot341 = false; // This variable is used for some esoteric business on dot 1 of scanline 0.
            }
            // Okay, now that we're updated all those flags, let's render stuff to the screen!

            // let's establish the order of operations.
            // Sprite evaluation
            // then calculate the color for the next dot.

            //but to complicate things, the delay after writing to $2001 happens between those 2 steps, and also on a specific alignment, this delay is 1 cycle longer for sprite evaluation.

            // If this is NOT phase 1
            if ((CPUClock & 3) != 3)
            {
                // sprite evaluation has a 1 ppu cycle delay before recognizing these flags were set or cleared.
                PPU_Mask_ShowBackground_Delayed = PPU_Mask_ShowBackground;
                PPU_Mask_ShowSprites_Delayed = PPU_Mask_ShowSprites;
            }

            PPU_DATA_StateMachine();
            PPU_OAMLatch = PPU_OAMBuffer;

            // TODO: Does this use a state machine like $2007?
            CopyV = false;
            if (PPU_Update2006Delay > 0)
            {
                PPU_Update2006Delay--; // this counts down,
                if (PPU_Update2006Delay == 0) // and when it reaches zero
                {
                    ushort temp_Prev_V = PPU_v;
                    CopyV = true;
                    PPU_v = PPU_t; // the PPU_ReadWriteAddress is updated!
                    PPU_AddressBus = PPU_v; // This value is the same thing.
                    if ((temp_Prev_V & 0x3FFF) >= 0x3F00 && (PPU_AddressBus & 0x3FFF) < 0x3F00) // Palette corruption check. Are we leaving Palette ram?
                    {
                        if ((PPU_Scanline < 240) && PPU_Dot <= 256) // if this dot is visible
                        {
                            if ((temp_Prev_V & 0xF) != 0)  // also, Palette corruption only happens if the previous address did not end in a 0
                            {
                                PPU_VRegisterChangedOutOfVBlank = true;
                            }
                        }
                    }
                }
            }

            if(OAM2ResetSignal > 0)
            {
                OAM2ResetSignal--; // This will never reach zero in this routine. See _EmulateHalfPPU()
            }

            if ((PPU_Scanline < 240 || PPU_Scanline == 261))// if this is the pre-render line, or any line before vblank
            {
                // Sprite evaluation
                if (PPU_Scanline < 241 || PPU_Scanline == 261)
                {
                    PPU_Render_SpriteEvaluation(); // fill in secondary OAM, and set up various arrays of sprite properties.

                    if (PPU_Mask_ShowBackground_Delayed || PPU_Mask_ShowSprites_Delayed)
                    {
                        if (PPU_Dot == 63 || PPU_Dot == 255 || PPU_Dot == 339)
                        {
                            OAM2ResetSignal = 3; // 3 half-dots.
                        }
                    }
                }
            }

            if ((CPUClock & 3) == 3)
            {
                // on phase 1,
                // sprite evaluation has a 2 ppu cycle delay before recognizing these flags were set or cleared.
                PPU_Mask_ShowBackground_Delayed = PPU_Mask_ShowBackground;
                PPU_Mask_ShowSprites_Delayed = PPU_Mask_ShowSprites;
            }

            if (!PPU_Mask_ShowBackground && !PPU_Mask_ShowSprites)
            {
                PPU_RenderingCounter = 0;

                PPU_AddressBus = PPU_v; // the address bus is always v when rendering is disabled.
                // TODO: Is this occuring one ppu cycle too late???
                // I specifically moved this here (outside of the following if statements) because it broke nes_reset_state_detect-letters.nes on alignment 1.
            }
            else if(PPU_RenderingCounter < 5)
            {
                PPU_RenderingCounter++;
            }

            // after sprite evaluation, but before screen rendering...
            if (PPU_Update2001Delay > 0) // if we wrote to 2001 recently
            {
                PPU_Update2001Delay--;
                if (PPU_Update2001Delay == 0) // if we've waited enough cycles, apply the changes
                {
                    PPU_Mask_8PxShowBackground = (PPU_Update2001Value & 0x02) != 0;
                    PPU_Mask_8PxShowSprites = (PPU_Update2001Value & 0x04) != 0;
                    PPU_Mask_ShowBackground = (PPU_Update2001Value & 0x08) != 0;
                    PPU_Mask_ShowSprites = (PPU_Update2001Value & 0x10) != 0;

                    PPU_Mask_ShowBackground_Instant = PPU_Mask_ShowBackground; // now that the PPU has updated, OAM evaluation will also recognize the change
                    PPU_Mask_ShowSprites_Instant = PPU_Mask_ShowSprites;
                }
            }
            if (PPU_Update2001OAMCorruptionDelay > 0) // if we wrote to 2001 recently
            {
                PPU_Update2001OAMCorruptionDelay--;
                if (PPU_Update2001OAMCorruptionDelay == 0) // if we've waited enough cycles, apply the changes
                {
                    if (PPU_WasRenderingBefore2001Write && (PPU_Update2001Value & 0x08) == 0 && (PPU_Update2001Value & 0x10) == 0)
                    {
                        if ((PPU_Scanline < 240 || PPU_Scanline == 261)) // if this is the pre-render line, or any line before vblank
                        {
                            if (!PPU_PendingOAMCorruption) // due to OAM corruption occurring inside OAM evaluation before this even occurs, make sure OAM isn't already corrupt
                            {
                                PPU_OAMCorruptionRenderingDisabledOutOfVBlank = true;
                            }
                        }
                    }
                }
            }
            if (PPU_Update2001EmphasisBitsDelay > 0)
            {
                PPU_Update2001EmphasisBitsDelay--;
                if (PPU_Update2001EmphasisBitsDelay == 0)
                {
                    PPU_Mask_Greyscale = (PPU_Update2001Value & 0x01) != 0;
                    PPU_Mask_EmphasizeRed = (PPU_Update2001Value & 0x20) != 0;
                    PPU_Mask_EmphasizeGreen = (PPU_Update2001Value & 0x40) != 0;
                    PPU_Mask_EmphasizeBlue = (PPU_Update2001Value & 0x80) != 0;
                }
            }

            PrevPrevPrevDotColor = PrevPrevDotColor; // Drawing a color to the screen has a 3(?) ppu cycle delay between deciding the color, and drawing it.
            PrevPrevDotColor = PrevDotColor;
            PrevDotColor = DotColor; // These variables here just record the color, and swap them through these variables so it can be used 3 cycles after it was chosen.
            if ((PPU_Scanline < 240 || PPU_Scanline == 261) || (PPU_Scanline == 240 && PPU_Dot == 0))// if this is the pre-render line, or any line before vblank, or dot 0 of scanline 240
            {
                if ((PPU_Dot >= 1 && PPU_Dot <= 256) || (PPU_Dot >= 321 && PPU_Dot <= 336)) // if this is a visible pixel, or preparing the start of next scanline
                {
                    if ((PPU_Mask_ShowBackground_Delayed || PPU_Mask_ShowSprites_Delayed)) // if rendering background or sprites
                    {
                        PPU_Render_ShiftRegistersAndBitPlanes(); // update shift registers for the background.
                    }
                }
                else if (PPU_Dot >= 337 || PPU_Dot == 0)
                {
                    if ((PPU_Mask_ShowBackground_Delayed || PPU_Mask_ShowSprites_Delayed)) // if rendering background or sprites
                    {
                        PPU_Render_ShiftRegistersAndBitPlanes_DummyNT();
                    }
                }

                if ((PPU_Dot > 0 && PPU_Dot <= 256)) // if this is a visible pixel, or preparing the start of next scanline
                {
                    if (PPU_Scanline < 241)
                    {
                        PPU_Render_CalculatePixel(false); // this determines the color of the pixel being drawn.
                    }
                    UpdateSpriteShiftRegisters(); // update shift registers for the sprites.
                }

                // (NESRecomp: TriCNES's bordered-screen and NTSC signal decode outputs are omitted.)
                DrawToScreen();
            }
            ThisDotReadFromPaletteRAM = false;

            PPU_DATA_StateMachine2();

            // NESRecomp: the mapper's per-PPU-clock hook (see MMC3). It belongs
            // at the end of _EmulatePPU, once per dot, where PPU_AddressBus holds
            // the address this dot drove - not in the half-dot handler, and not
            // before the fetch cases assign it (both of which put the mapper's
            // view of A12 a fraction of a dot away from the runtime's, which only
            // shows up at the CPU/PPU alignments where a dot falls next to the
            // cycle's interrupt sample). A12 and A13 are direct pins, so a mapper
            // sees every address the PPU puts out, fetch or not.
            Cart.MapperChip.PPUClock();
        }

byte FetchVideoMemory()
{
            Cart.MapperChip.Connector_SetUpPPUAddressPins();
            Cart.MapperChip.Connector_SetUpPPUDataPins((byte)PPU_AddressBus); // If AccessPPU() doesn't update the pins, then we get back the address bus.
            Cart.MapperChip.Connector_CheckCIRAM();
            bool CE = !SeventyTwoPinConnector[56] && !ConnectorPinFloating[56] && !ConnectorPinFloating[57];

            // Always attempt to read from the cartridge. The data pins would not be updated if reading from the nametable.
            Cart.MapperChip.Connector_PPU_RW(PPU_READ, PPU_WRITE);
            Cart.MapperChip.AccessPPU();
            byte t = Cart.MapperChip.Connector_ReadPPUDataPins((byte)PPU_AddressBus);

            if (CE)
            {
                // We're reading from on-console VRAM, not the cartridge.
                // NOTE: In theory you could trigger bus conflicts with this. I'm currently just assuming the bus is free.
                ushort Address = (ushort)((PPU_AddressBus & 0x300) | PPU_OctalLatch);
                Address |= (ushort)(SeventyTwoPinConnector[21] ? 0x400 : 0);
                t = VRAM[Address];
            }

            PPU_AddressBus &= 0xFF00;
            PPU_AddressBus |= t;

            return t;
        }

void WriteVideoMemory(byte input)
{
            Cart.MapperChip.Connector_SetUpPPUAddressPins();
            Cart.MapperChip.Connector_CheckCIRAM();
            bool CE = !SeventyTwoPinConnector[56] && !ConnectorPinFloating[56] && !ConnectorPinFloating[57];

            if ((PPU_AddressBus & 0x3FFF) >= 0x3F00)
            {
                PaletteRAM[PPU_AddressBus & (((PPU_AddressBus & 0x3) == 0) ? 0x0F : 0x1F)] = input;
            }
            else if (CE)
            {
                // We're writing to on-console VRAM, not the cartridge.
                // NOTE: In theory you could trigger bus conflicts with this. I'm currently just assuming the bus is free.
                ushort Address = (ushort)((PPU_AddressBus & 0x300) | PPU_OctalLatch);
                Address |= (ushort)(SeventyTwoPinConnector[21] ? 0x400 : 0);
                VRAM[Address] = input;
            }
            else
            {
                Cart.MapperChip.Connector_SetUpPPUDataPins(input);
                Cart.MapperChip.Connector_PPU_RW(PPU_READ, PPU_WRITE);
                Cart.MapperChip.AccessPPU();
            }
        }

void _EmulateHalfPPU()
{
            // Oh boy, it's time for half PPU cycles.            
            if ((PPU_Scanline < 240 || PPU_Scanline == 261))// if this is the pre-render line, or any line before vblank
            {
                if ((PPU_Dot > 0 && PPU_Dot <= 257) || (PPU_Dot > 320 && PPU_Dot <= 336)) // if this is a visible pixel, or preparing the start of next scanline
                {
                    if ((PPU_Mask_ShowBackground || PPU_Mask_ShowSprites)) // if rendering background or sprites
                    {
                        PPU_UpdateBackgroundShiftRegisters(); // shift all the shift registers 1 bit
                    }
                }
            }

            if(OAM2ResetSignal > 0)
            {
                OAM2ResetSignal--;
                if(OAM2ResetSignal == 0)
                {
                    OAM2Address = 0;
                    OAM2Overflowed = false;
                }
            }

            PPU_Render_CommitShiftRegistersAndBitPlanes();
            if ((PPU_Mask_ShowBackground_Delayed || PPU_Mask_ShowSprites_Delayed) || (PPU_Mask_ShowBackground || PPU_Mask_ShowSprites)) // if rendering background or sprites
            {
                PPU_OAMBuffer = PPU_OAMBuffer_In; // Update the OAM Buffer.
            }
            PPU_VSET = false;
            if (PPU_PendingVBlank)
            {
                PPU_PendingVBlank = false;
                PPU_VSET = true;
            }
            // PPU_VSET_Latch1 gets inverted, and that becomes the state of PPU_VSET_Latch2
            PPU_VSET_Latch2 = !PPU_VSET_Latch1;

            PPUStatus_SpriteZeroHit_Delayed = PPUStatus_SpriteZeroHit;
            if (PPUStatus_PendingSpriteZeroHit2)
            {
                PPUStatus_PendingSpriteZeroHit2 = false;
                PPUStatus_SpriteZeroHit = true;
            }
            if (PPUStatus_PendingSpriteZeroHit)
            {
                PPUStatus_PendingSpriteZeroHit = false;
                PPUStatus_PendingSpriteZeroHit2 = true;
            }

            PPU_DATA_StateMachine_Half();

        }

void PPU_DATA_StateMachine()
{
            bool BLNK = (!PPU_Mask_ShowBackground && !PPU_Mask_ShowSprites) || (PPU_Scanline >= 240 && PPU_Scanline < 261);
            PPU_2007_BLNK_Latch = BLNK;
            bool H0_DASH = (PPU_Dot - 1 & 1) != 0;

            PPU_2007_PaletteRAMEnable = ((PPU_AddressBus & 0x3F00) == 0x3F00) && PPU_2007_BLNK_Latch;
            PPU_2007_Read_XRB = PPU_2007_Read && PPU_2007_PaletteRAMEnable;
                     
            PPU_2007_Read_Latches[0] = PPU_2007_Read_SR;
            if(PPU_2007_Read)
            {
                PPU_2007_Read = false; // I put this in an if statement for easier debugging / breakpoint placement.
            }
            PPU_2007_Read_Latches[2] = !PPU_2007_Read_Latches[1];
            PPU_2007_Read_Latches[4] = !PPU_2007_Read_Latches[3];
            PPU_2007_PD_RB = PPU_2007_Read_Latches[4] && !PPU_2007_Read_Latches[2];
            PPU_2007_ReadALE = !PPU_2007_Read_Latches[4] && PPU_2007_Read_Latches[2];
            PPU_2007_Read_H0_Latch = (PPU_Dot - 1 & 1) != 0;


            PPU_READ = (PPU_2007_PD_RB || (!BLNK && PPU_2007_Read_H0_Latch)); // even ppu cycles outside of blanking always read. Also read if we are reading $2007.

            PPU_2007_Write_Latches[0] = PPU_2007_Write_SR;
            if (PPU_2007_Write)
            {
                PPU_2007_Write = false; // I put this in an if statement for easier debugging / breakpoint placement.
            }
            PPU_2007_Write_Latches[2] = !PPU_2007_Write_Latches[1];
            PPU_2007_Write_Latches[4] = !PPU_2007_Write_Latches[3];
            PPU_2007_WriteALE = !PPU_2007_Write_Latches[4] && PPU_2007_Write_Latches[2];

            PPU_2007_TStep_Latch = PPU_2007_DB_PAR;
           
            bool b = (!BLNK && !H0_DASH); // If you are on an even dot out of a blanking period
            PPU_ALE = (PPU_2007_ReadALE || PPU_2007_WriteALE || b);

            if ((PPU_2007_ReadALE || PPU_2007_WriteALE))
            {
                if (!PPU_READ) // TODO: this if statement doesn't seem to change the results of the 2007 stress test in any way.
                {
                    PPU_AddressBus = PPU_v;
                    PPU_OctalLatch = (byte)PPU_AddressBus;
                }
            }
        }

void PPU_DATA_StateMachine2()
{
            
            if (PPU_2007_PD_RB)
            {
                PPU_ReadBuffer = FetchVideoMemory();
                if (PPU_ALE)
                {
                    PPU_OctalLatch = (byte)PPU_AddressBus;
                }
            }           
        }

void PPU_DATA_StateMachine_Half()
{
            PPU_2007_TStep = (PPU_2007_TStep_Latch || PPU_2007_PD_RB);
            if (PPU_2007_TStep) // If this occurs inside PPU_DATA_StateMachine() instead, the timing is wrong, and this breaks SMB1's title screen.
            {
                if (!PPU_2007_BLNK_Latch)
                {
                    PPU_IncrementScrollY();
                }
                else
                {
                    PPU_v += (ushort)(PPUControlIncrementMode32 ? 32 : 1);
                    PPU_v &= 0x7FFF;
                }
            }

            PPU_ALE = (PPU_2007_ReadALE || PPU_2007_WriteALE);
            if (PPU_2007_PD_RB)
            {
                PPU_ReadBuffer = FetchVideoMemory();
                if (PPU_ALE)
                {
                    // pretty sure this can never happen, but keep it just in case.
                    PPU_OctalLatch = (byte)PPU_AddressBus;
                }
            }
            PPU_2007_Read_Latches[1] = !PPU_2007_Read_Latches[0];
            PPU_2007_Read_Latches[3] = !PPU_2007_Read_Latches[2];
            if (!PPU_2007_Read_Latches[3])
            {
                PPU_2007_Read_SR = false;
            }

            PPU_2007_Write_Latches[1] = !PPU_2007_Write_Latches[0];
            PPU_2007_Write_Latches[3] = !PPU_2007_Write_Latches[2];
            if (!PPU_2007_Write_Latches[3])
            {
                PPU_2007_Write_SR = false;
            }
            PPU_2007_DB_PAR = PPU_2007_Write_Latches[1] && !PPU_2007_Write_Latches[3];
            PPU_WRITE = !PPU_2007_PaletteRAMEnable && PPU_2007_DB_PAR;
            if (PPU_2007_DB_PAR) // Using PAR instead of PPU_WRITE, since I re-use WriteVideoMemory() for writes to palette RAM.
            {
                WriteVideoMemory(PPU_2007_WriteData);
            }

        }

// CYC_ORACLE_UNFIXED=BACKDROP restores TriCNES's odd-frame backdrop pixel.
static int oracle_backdrop_fix = -1;
static bool OracleBackdropFix()
{
            if (oracle_backdrop_fix < 0) {
                const char *unfixed = getenv("CYC_ORACLE_UNFIXED");
                oracle_backdrop_fix = !(unfixed && strstr(unfixed, "BACKDROP"));
            }
            return oracle_backdrop_fix != 0;
}

void DrawToScreen()
{
            if (PPU_Dot > 3 && PPU_Dot <= 259 && PPU_Scanline < 241) // the process of drawing a dot to the screen actually has a 2 ppu cycle delay, which the emphasis bits happen after
            {
                // in other words, the geryscale/emphasis bits can affect the color that was decided 2 ppu cycles ago.
                chosenColor = PrevPrevPrevDotColor;
                if (PPU_Mask_Greyscale) // if the ppu greyscale mode is active,
                {
                    chosenColor &= 0x30; //To force greyscale, bitwise AND this color with 0x30
                }
                // emphasis bits
                int emphasis = 0;
                if (PPU_Mask_EmphasizeRed) { emphasis |= 0x40; } // if emhpasizing r, add 0x40 to the index into the palette LUT.
                if (PPU_Mask_EmphasizeGreen) { emphasis |= 0x80; } // if emhpasizing g, add 0x80 to the index into the palette LUT.
                if (PPU_Mask_EmphasizeBlue) { emphasis |= 0x100; } // if emhpasizing b, add 0x100 to the index into the palette LUT.
                int scanline0OddFrameOffset = 0;
                // NESRecomp: ORACLE FIX (BACKDROP PIXEL) - see below. The shift itself is
                // the fix: what an odd frame skips is the last tick of the pre-render
                // scanline, so scanline 0 still emits all 256 of its pixels.
                if (!OracleBackdropFix() && PPU_Scanline == 0 && PPU_OddFrame &&
                    (PPU_Mask_ShowBackground || PPU_Mask_ShowSprites))
                {
                    scanline0OddFrameOffset = 1;
                }
                if (scanline0OddFrameOffset == 1 && PPU_Dot == 4)
                {
                    // do nothing. This would be off screen.
                }
                else
                {
                    cyc_framebuffer[PPU_Scanline * 256 + (PPU_Dot - 4 - scanline0OddFrameOffset)] = NesPalInts[chosenColor | emphasis]; // this sets the pixel on screen to the chosen color.
                    cyc_frame_index_buffer[PPU_Scanline * 256 + (PPU_Dot - 4 - scanline0OddFrameOffset)] = (ushort)(chosenColor | emphasis); // NESRecomp
                }
            }
            // NESRecomp: ORACLE FIX (BACKDROP PIXEL). This pixel only exists because
            // TriCNES shifts scanline 0 one pixel left on odd frames, leaving x=255
            // with nothing in it. An odd frame skips the last tick of the *pre-render*
            // scanline (nesdev wiki, PPU frame timing: the PPU "jumps directly from
            // (339, 261) to (0, 0)"; NES_MiSTer's ppu.sv skips "the *last* cycle of odd
            // frames", armed on the pre-render line), so scanline 0 runs its own dots
            // and emits all 256 pixels, and Mesen's picture agrees. With the fix the
            // shift above is gone and there is no gap to fill; CYC_ORACLE_UNFIXED=BACKDROP
            // restores both, along with TriCNES's raw 8-bit palette byte here (palette RAM
            // holds 6 bits and greyscale applies to every color the PPU outputs, so a
            // $3F00 write with bit 6 or 7 set turned this pixel into another emphasis
            // row's color).
            if (!OracleBackdropFix() && PPU_Scanline == 0 && PPU_OddFrame &&
                (PPU_Mask_ShowBackground || PPU_Mask_ShowSprites) && PPU_Dot == 259)
            {
                // draw the backdrop.
                chosenColor = PaletteRAM[0];
                // emphasis bits
                int emphasis = 0;
                if (PPU_Mask_EmphasizeRed) { emphasis |= 0x40; } // if emhpasizing r, add 0x40 to the index into the palette LUT.
                if (PPU_Mask_EmphasizeGreen) { emphasis |= 0x80; } // if emhpasizing g, add 0x80 to the index into the palette LUT.
                if (PPU_Mask_EmphasizeBlue) { emphasis |= 0x100; } // if emhpasizing b, add 0x100 to the index into the palette LUT.
                cyc_framebuffer[PPU_Scanline * 256 + 255] = NesPalInts[chosenColor | emphasis]; // this sets the pixel on screen to the chosen color.
                cyc_frame_index_buffer[PPU_Scanline * 256 + 255] = (ushort)(chosenColor | emphasis); // NESRecomp
            }
        }

void CorruptOAM()
{
            // basically 8 entries of OAM are getting replaced (this is considered a single "row" of OAM) 
            // PPU_OAMCorruptionIndex is the row that gets corrupted.
            if (PPU_OAMCorruptionIndex == 0x20)
            {
                PPU_OAMCorruptionIndex = 0;
            }
            int i = 0;
            while (i < 8) // 8 entries in a row
            {
                OAM[PPU_OAMCorruptionIndex * 8 + i] = OAM[i]; // The corrupted row is replaced with the values from row 0
                i++;
            }
            OAM2[PPU_OAMCorruptionIndex] = OAM2[0]; // Also corrupt this byte.
            // this all happens in a single cycle.
        }

void IncrementOAM2Address()
{
            if(!OAM2Overflowed)
            {
                OAM2Address++;
                if(OAM2Address == 0x20)
                {
                    OAM2Overflowed = true;
                    OAM2Address = 0;
                }
            }
        }

void PPU_Render_SpriteEvaluation()
{
            bool SpriteEval_ReadOnly_PreRenderLine = false;
            if (PPU_Scanline == 261)
            {
                SpriteEval_ReadOnly_PreRenderLine = true;
            }
            if ((PPU_Mask_ShowBackground_Instant || PPU_Mask_ShowSprites_Instant))
            {
                if (PPU_PendingOAMCorruption) // OAM corruption occurs on the visible dot after rendering was enabled. It also can happen on the pre-render line.
                {
                    PPU_PendingOAMCorruption = false;
                    if (!PPU_OAMCorruptionRenderingEnabledOutOfVBlank)
                    {
                        CorruptOAM();
                    }
                    PPU_OAMCorruptionRenderingEnabledOutOfVBlank = false;
                }
            }

            if ((PPU_Dot >= 0 && PPU_Dot <= 64)) // Dots 1 through 64, not on the pre-render line. (and also dot 0 for OAM corruption purposes)
            {

                // this step is clearing secondary OAM, and writing FF to each byte in the array.
                if ((PPU_Dot & 1) == 1)
                { //odd cycles
                    if ((PPU_Mask_ShowBackground_Delayed || PPU_Mask_ShowSprites_Delayed))
                    {
                        if (SpriteEval_ReadOnly_PreRenderLine)
                        {
                            PPU_OAMBuffer_In = OAM2[OAM2Address];
                        }
                        else
                        {
                            PPU_OAMBuffer_In = 0xFF;
                        }
                        if (PPU_Dot == 1)
                        {
                            OAM2Address = 0; // if this is dot 1, reset the secondary OAM address                                                     // in preparation for the next section, let's clear these flags too
                            SpriteEvaluationTick = 0;
                            OAMAddressOverflowedDuringSpriteEvaluation = false;
                        }
                        if (PPU_OAMCorruptionRenderingDisabledOutOfVBlank || PPU_OAMCorruptionRenderingDisabledOutOfVBlank_Instant)
                        {
                            PPU_OAMCorruptionRenderingDisabledOutOfVBlank = false;
                            PPU_OAMCorruptionRenderingDisabledOutOfVBlank_Instant = false;
                            PPU_PendingOAMCorruption = true;
                            PPU_OAMCorruptionIndex = OAM2Address; // this value will be used when rendering is re-enabled and the corruption occurs
                        }
                    }
                }
                else
                { //even cycles
                    if (PPU_Dot > 0)
                    {
                        if ((PPU_Mask_ShowBackground_Delayed || PPU_Mask_ShowSprites_Delayed))
                        {
                            if (!SpriteEval_ReadOnly_PreRenderLine)
                            {
                                OAM2[OAM2Address] = PPU_OAMBuffer; // store FF in secondary OAM
                            }
                            if (PPU_OAMCorruptionRenderingDisabledOutOfVBlank)
                            {
                                PPU_OAMCorruptionRenderingDisabledOutOfVBlank = false;
                                PPU_OAMCorruptionRenderingDisabledOutOfVBlank_Instant = false;
                                PPU_PendingOAMCorruption = true;
                                PPU_OAMCorruptionIndex = OAM2Address; // this value will be used when rendering is re-enabled and the corruption occurs
                            }

                            IncrementOAM2Address();

                            if (PPU_OAMCorruptionRenderingDisabledOutOfVBlank_Instant && PPU_Dot == 64)
                            {
                                PPU_OAMCorruptionRenderingDisabledOutOfVBlank = false;
                                PPU_OAMCorruptionRenderingDisabledOutOfVBlank_Instant = false;
                                PPU_PendingOAMCorruption = true;
                            }
                        }
                        else
                        {
                            if (PPU_OAMCorruptionRenderingDisabledOutOfVBlank || PPU_OAMCorruptionRenderingDisabledOutOfVBlank_Instant)
                            {
                                PPU_OAMCorruptionRenderingDisabledOutOfVBlank = false;
                                PPU_OAMCorruptionRenderingDisabledOutOfVBlank_Instant = false;
                                PPU_PendingOAMCorruption = true;
                                PPU_OAMCorruptionIndex = OAM2Address; // this value will be used when rendering is re-enabled and the corruption occurs
                            }
                        }
                    }
                    else
                    {
                        if ((PPU_Mask_ShowBackground_Delayed || PPU_Mask_ShowSprites_Delayed))
                        {
                            IncrementOAM2Address();
                        }
                    }
                }
            }
            else if ((PPU_Dot >= 65 && PPU_Dot <= 256)) // Dots 65 through 256, not on the pre-render line
            {
                if (PPU_Dot == 65)
                {
                    if ((PPU_Mask_ShowBackground_Delayed || PPU_Mask_ShowSprites_Delayed))
                    {
                        NineObjectsOnThisScanline = false;
                        OamCorruptedOnOddCycle = false;
                        OAMAddressOverflowedDuringSpriteEvaluation = false;
                    }
                }
                if (PPU_Mask_ShowBackground_Instant || PPU_Mask_ShowSprites_Instant || PPU_OAMCorruptionRenderingDisabledOutOfVBlank_Instant) // if rendering is enabled, or was *just* disabled mid evaluation
                {
                    if ((PPU_Dot & 1) == 1)
                    { //odd cycles
                        byte PrevSpriteEvalTemp = PPU_OAMBuffer;
                        PPU_OAMBuffer_In = OAM[PPUOAMAddress]; // read from OAM

                        // If rendering was disabled *this* cycle (the odd cycle) then the even cycle will run normally, and the *next odd cycle* will have the OAM address increment. Presumably, that's when we record secondOAMAddr.
                        if (PPU_OAMCorruptionRenderingDisabledOutOfVBlank_Instant)
                        {
                            PPU_OAMEvaluationCorruptionOddCycle = false;
                            PPU_OAMCorruptionRenderingDisabledOutOfVBlank = false;
                            if (!SpriteEval_ReadOnly_PreRenderLine)
                            {
                                PPUOAMAddress++;
                            }
                            OamCorruptedOnOddCycle = true;

                        }
                    }
                    else
                    { //even cycles                       
                        if (!OAMAddressOverflowedDuringSpriteEvaluation)
                        {
                            byte PreIncVal = PPUOAMAddress; // for checking if PPUOAMAddress overflows
                            if (!OAM2Overflowed && !SpriteEval_ReadOnly_PreRenderLine) // If secondary OAM is not yet full,
                            {
                                OAM2[OAM2Address] = PPU_OAMBuffer; // store this value at the secondary oam address.
                            }
                            byte OAM2READ = OAM2[OAM2Address];
                            if (SpriteEvaluationTick == 0) // tick 0: check if this object's y position is in range for this scanline
                            {
                                PPU_OAMEvaluationObjectInXRange = false;
                                PPU_OAM_VerticalOffset = (ushort)((PPU_Scanline & 0xFF) - PPU_OAMBuffer);
                                if (!NineObjectsOnThisScanline && !SpriteEval_ReadOnly_PreRenderLine && PPU_OAM_VerticalOffset < (PPU_Spritex16 ? 16 : 8))
                                {
                                    PPU_OAMEvaluationObjectInRange = true;
                                    // if this sprite is within range.
                                    if (!OAM2Overflowed)
                                    {
                                        if (!OamCorruptedOnOddCycle)
                                        {
                                            if (!SpriteEval_ReadOnly_PreRenderLine)
                                            {
                                                PPUOAMAddress++; // +1
                                            }
                                            IncrementOAM2Address();
                                        }
                                        // Sprite zero hits actually have nothing to do with reading the object at OAM index 0. Rather, if an object is within range of the scanline on dot 66.
                                        // typically, the object processed on dot 66 is OAM[0], though it's possible using precisely timed writes to $2003 to have PPUOAMAddress start processing here from a different value.
                                        if (PPU_Dot == 66)
                                        {
                                            PPU_NextScanlineContainsSpriteZero = true; // this value will be transferred to PPU_PreviousScanlineContainsSpriteZero at the end of the scanline, and that variable is used in sp 0 hit detection.
                                        }
                                    }
                                    else
                                    {
                                        NineObjectsOnThisScanline = true;
                                        PPUOAMAddress++;
                                        if (!PPUStatus_SpriteOverflow)// if secondary OAM is full, yet another object is on this scanline
                                        {
                                            PPUStatus_SpriteOverflow = true; // set the sprite overflow flag
                                        }
                                    }
                                    if (!SpriteEval_ReadOnly_PreRenderLine)
                                    {
                                        SpriteEvaluationTick++; // increment the tick for next even ppu cycle.
                                    }
                                }
                                else
                                {
                                    if (PPU_Dot == 66)
                                    {
                                        PPU_NextScanlineContainsSpriteZero = false; // this value will be transferred to PPU_PreviousScanlineContainsSpriteZero at the end of the scanline, and that variable is used in sp 0 hit detection.
                                    }
                                    PPU_OAMEvaluationObjectInRange = false;
                                    if (!OamCorruptedOnOddCycle && !SpriteEval_ReadOnly_PreRenderLine)
                                    {
                                        if (OAM2Overflowed && !NineObjectsOnThisScanline)// this behavior stops after finding the ninth object.
                                        {
                                            if ((PPUOAMAddress & 0x3) == 3)
                                            {
                                                PPUOAMAddress++; // A real hardware bug.
                                            }
                                            else
                                            {
                                                PPUOAMAddress += 4; // +4
                                                PPUOAMAddress++; // A real hardware bug.
                                            }
                                        }
                                        else
                                        {
                                            PPUOAMAddress += 4; // +4
                                            PPUOAMAddress &= 0xFC; // also mask away the lower 2 bits
                                        }
                                    }
                                }
                            }
                            else // ticks 1, 2, or 3
                            {
                                if (SpriteEvaluationTick == 3) // tick 3: X position.
                                {
                                    PPU_OAMEvaluationObjectInRange = false;
                                    // OAM X coordinate.
                                    // This also runs the "vertical in range check", though typically the result doesn't matter.
                                    if (PPU_Scanline - PPU_OAMBuffer >= 0 && PPU_Scanline - PPU_OAMBuffer < (PPU_Spritex16 ? 16 : 8))
                                    {
                                        // if this sprite is within range.
                                        PPU_OAMEvaluationObjectInXRange = true;
                                        if (!OAM2Overflowed)
                                        {
                                            if (!OamCorruptedOnOddCycle && !SpriteEval_ReadOnly_PreRenderLine)
                                            {
                                                PPUOAMAddress++; // +1
                                            }
                                        }
                                        else
                                        {
                                            if (!OamCorruptedOnOddCycle && !SpriteEval_ReadOnly_PreRenderLine)
                                            {
                                                PPUOAMAddress += 4; // +1 (In theory, this should be +4, though my experiments only reflect my consoles behavior if this is +1?)
                                            }
                                        }
                                    }
                                    else
                                    {
                                        PPU_OAMEvaluationObjectInXRange = false;
                                        if (!OAM2Overflowed)
                                        {
                                            if (!OamCorruptedOnOddCycle && !SpriteEval_ReadOnly_PreRenderLine)
                                            {
                                                PPUOAMAddress += 1; // +1 (In theory, this should be +4, though my experiments only reflect my consoles behavior if this is +1?)
                                                PPUOAMAddress &= 0xFC; // also mask away the lower 2 bits
                                            }
                                        }
                                        else
                                        {
                                            PPUOAMAddress += 1; // +1 (In theory, this should be +4, though my experiments only reflect my consoles behavior if this is +1?)
                                            PPUOAMAddress &= 0xFC; // also mask away the lower 2 bits
                                        }
                                    }
                                }
                                else // ticks 1 and 2 don't make any checks. Only increment the OAM address.
                                {
                                    if (!OamCorruptedOnOddCycle && !SpriteEval_ReadOnly_PreRenderLine)
                                    {
                                        PPUOAMAddress++; // +1
                                    }
                                }
                                SpriteEvaluationTick++; // increment the tick for next even ppu cycle.
                                SpriteEvaluationTick &= 3; // and reset the tick to 0 if it reaches 4.
                                if (!OAM2Overflowed && !SpriteEval_ReadOnly_PreRenderLine) // if secondary OAM is not full
                                {
                                    IncrementOAM2Address();
                                }
                            }
                            OamCorruptedOnOddCycle = false;

                            if (PPUOAMAddress < PreIncVal && PPUOAMAddress < 4) // If an overflow occured
                            {
                                OAMAddressOverflowedDuringSpriteEvaluation = true; // set this flag.
                            }
                            PPU_OAMBuffer_In = OAM2READ; // When overflowed, the ppu reads instead of writing to OAM2. (Run this regardless of if OAM2 is full or not.)
                        }
                        else
                        {   // OAM Address Overflowed During Sprite Evaluation
                            // fail to write to SecondaryOAM
                            // boo womp.

                            // also update the PPUOAMAddress.
                            if (!OamCorruptedOnOddCycle && !SpriteEval_ReadOnly_PreRenderLine)
                            {
                                PPUOAMAddress += 4; // +4
                                PPUOAMAddress &= 0xFC; // also mask away the lower 2 bits
                            }
                            PPU_OAMBuffer_In = OAM2[OAM2Address]; // When overflowed, the ppu reads instead of writing to OAM2.
                        }
                        if (PPU_OAMCorruptionRenderingDisabledOutOfVBlank_Instant && !PPU_OAMEvaluationCorruptionOddCycle) // if we just disabled rendering mid OAM evaluation, the address is incremented yet again.
                        {
                            PPU_OAMCorruptionRenderingDisabledOutOfVBlank = false;
                            PPU_OAMCorruptionRenderingDisabledOutOfVBlank_Instant = false;
                            PPU_PendingOAMCorruption = true;

                            if ((OAM2Address & 3) != 0 && !OAMAddressOverflowedDuringSpriteEvaluation && !SpriteEval_ReadOnly_PreRenderLine)
                            {
                                OAM2Address &= 0xFC;
                                OAM2Address += 4; // ??? Why is this using OAM2???
                                // NESRecomp PORT FIX: from $1D-$1F this reaches $20, one past
                                // OAM2, and the next OAM2 access is out of bounds (upstream
                                // C# throws; the port overwrote the following globals).
                                // Overflow the counter the way IncrementOAM2Address() does.
                                if (OAM2Address == 0x20)
                                {
                                    OAM2Overflowed = true;
                                    OAM2Address = 0;
                                }
                            }
                            if (PPUClock == 0 || PPUClock == 3)
                            {
                                PPU_OAMCorruptionIndex = (byte)(OAM2Address); // this value will be used when rendering is re-enabled and the corruption occurs
                            }
                            if (PPUClock == 1 || PPUClock == 2)
                            {
                                PPU_OAMCorruptionIndex = (byte)(OAM2Address); // this value will be used when rendering is re-enabled and the corruption occurs
                            }
                            if (PPU_Dot == 256)
                            {
                                PPU_OAMCorruptionIndex = OamCorruptedOnOddCycle ? (byte)0 : (byte)1; //I have no idea.
                            }

                        }
                        PPU_OAMCorruptionRenderingDisabledOutOfVBlank_Instant = false;
                    }
                }

            }
            else if (PPU_Dot >= 257 && PPU_Dot <= 320) // this also happens on the pre-render line.
            {
                PPU_CurrentScanlineContainsSpriteZero = PPU_NextScanlineContainsSpriteZero;

                if ((PPU_Mask_ShowBackground_Delayed || PPU_Mask_ShowSprites_Delayed))
                {
                    PPUOAMAddress = 0; // this is reset during every one of these cycles, 257 through 320
                }

                if (PPU_OAMCorruptionRenderingDisabledOutOfVBlank && (PPUClock == 0 || PPUClock == 3))
                {
                    PPU_OAMCorruptionRenderingDisabledOutOfVBlank = false;
                    PPU_OAMCorruptionRenderingDisabledOutOfVBlank_Instant = false;
                    PPU_PendingOAMCorruption = true;
                    PPU_OAMCorruptionIndex = OAM2Address; // this value will be used when rendering is re-enabled and the corruption occurs
                }

                if (PPU_READ)
                {
                    PPU_OctalLatch = (byte)PPU_AddressBus;
                }

                int selectedShifter = ((PPU_Dot - 1) & 0x38) >> 3;

                if (PPU_Mask_ShowBackground_Delayed || PPU_Mask_ShowSprites_Delayed)
                {
                    PPU_OAMBuffer_In = OAM2[OAM2Address];
                }
                else
                {
                    PPU_OAMBuffer_In = OAM[PPUOAMAddress]; // The OAM buffer is updated with Primary OAM when rendering is disabled.
                }
                switch ((PPU_Dot - 1) & 7)
                    {
                        // So each scanline can only have up to 8 sprites.
                        // Each sprite has a Y position, Pattern, Attributes, and X position.
                        // So there's an 8-index-long array for each of those.
                        // Each index in the array is for a different sprite.

                        // Sprites also have 2 "bit plane" shift registers.
                        // These are the 8 pixels to draw for the object on this scanline.
                        // Again, there are 8 objects, so there are 2 8-index-long arrays of bit planes.

                        // each case is a different ppu cycle.
                        // case 0.
                        // next cycle, case 1.
                        // next cycle, case 2, and so on.
                        // case 7 then leads back to case 0.

                        case 0: // Y position         dot 257, (+8), (+16) ...
                            if (PPU_Mask_ShowBackground_Delayed || PPU_Mask_ShowSprites_Delayed) // if rendering has been enabled for at least one cycle
                            {
                                // set this object's Y position in the array
                                PPU_PatternAddressRegister_NT = (ushort)(0x2000 + (PPU_v & 0x0FFF));
                                PPU_PAR_MUX = PPU_PatternAddressRegister_NT;
                                PPU_AddressBus = PPU_PAR_MUX;
                                IncrementOAM2Address();
                            }
                            break;
                        case 1: // Pattern            dot 258, (+8), (+16) ...
                            if (PPU_Mask_ShowBackground_Delayed || PPU_Mask_ShowSprites_Delayed) // if rendering has been enabled for at least one cycle
                            {
                                PPU_OAM_VerticalOffset = (ushort)((PPU_Scanline & 0xFF) - PPU_OAMBuffer);

                                // set this object's pattern in the array
                                PPU_Render_ShiftRegistersAndBitPlanes(); // Dummy Nametable Fetch
                                IncrementOAM2Address();
                            }
                            break;
                        case 2: // Attribute          dot 259, (+8), (+16) ...
                            if (PPU_Mask_ShowBackground_Delayed || PPU_Mask_ShowSprites_Delayed) // if rendering has been enabled for at least one cycle
                            {
                                PPU_SpritePattern = PPU_OAMBuffer;

                                // set this object's attribute in the array

                                PPU_PatternAddressRegister_NT = (ushort)(0x2000 + (PPU_v & 0x0FFF));
                                PPU_PAR_MUX = PPU_PatternAddressRegister_NT;
                                PPU_AddressBus = PPU_PAR_MUX;
                                IncrementOAM2Address();
                            }
                            break;
                        case 3: // X position         dot 260, (+8), (+16) ...
                            if (PPU_Mask_ShowBackground_Delayed || PPU_Mask_ShowSprites_Delayed) // if rendering has been enabled for at least one cycle
                            {
                                PPU_SpriteAttribute[selectedShifter] = PPU_OAMBuffer;
                                PPU_SpriteAttr = PPU_OAMBuffer;
                                // set this object's X position in the array
                                PPU_Render_ShiftRegistersAndBitPlanes(); // Dummy Nametable Fetch                            
                            }
                            // notably, the secondary OAM address does not get incremented until case 7
                            break;
                        case 4: // X position (again) dot 261, (+8), (+16) ...
                            if (PPU_Mask_ShowBackground_Delayed || PPU_Mask_ShowSprites_Delayed) // if rendering has been enabled for at least one cycle
                            {
                                // set this object's X position in the array... again.
                                PPU_SpriteShifterCounter[selectedShifter] = PPU_OAMBuffer;

                                PPU_SpriteXposition[selectedShifter] = PPU_OAMBuffer;
                                // But also: Find the PPU address of this sprite's graphical data inside the Pattern Tables.
                                PPU_CheckPAR();
                                PPU_PatternAddressRegister_CHR &= 0x1FF7;
                                PPU_PAR_MUX = PPU_PatternAddressRegister_CHR;
                                PPU_AddressBus = PPU_PAR_MUX;
                            }
                            break;
                        case 5: // X position (again)  dot 262, (+8), (+16) ...
                            if (PPU_Mask_ShowBackground_Delayed || PPU_Mask_ShowSprites_Delayed) // if rendering has been enabled for at least one cycle
                            {
                                // set this object's X position in the array... again.
                                PPU_SpriteXposition[selectedShifter] = PPU_OAMBuffer;
                                // but also: set up the bit plane shift register.

                                PPU_CheckPAR();
                                PPU_PatternAddressRegister_CHR &= 0x1FF7;
                                PPU_AddressBus = (ushort)((PPU_PatternAddressRegister_CHR & 0xFF00) | PPU_OctalLatch);

                                PPU_SpritePatternL = FetchVideoMemory();
                                if (((PPU_SpriteAttribute[selectedShifter] >> 6) & 1) == 1) // Attributes are set up to flip X
                                {
                                    PPU_SpritePatternL = Flip(PPU_SpritePatternL);
                                }
                                PPU_SpriteShiftRegisterL[selectedShifter] = PPU_SpritePatternL;

                                // in-range check. (The pre-render line ends up checking scanline 5 due to the `& 0xFF`.
                                if (!(PPU_OAM_VerticalOffset < (PPU_Spritex16 ? 16 : 8)))
                                {
                                    PPU_SpriteShiftRegisterL[selectedShifter] = 0; // clear the value in this shift register if this object isn't in range.
                                }
                            }
                            break;
                        case 6: // X position (again)  dot 263, (+8), (+16) ...
                            if (PPU_Mask_ShowBackground_Delayed || PPU_Mask_ShowSprites_Delayed) // if rendering has been enabled for at least one cycle
                            {
                                // set this object's X position in the array... again.
                                PPU_SpriteXposition[selectedShifter] = PPU_OAMBuffer;
                                // but also: add 8 to the PPU address. The other bit plane is 8 addresses away.
                                PPU_AddressBus |= 8;
                                PPU_CheckPAR();
                                PPU_PatternAddressRegister_CHR |= 8;
                                PPU_PAR_MUX = PPU_PatternAddressRegister_CHR;
                                PPU_AddressBus = PPU_PAR_MUX;
                            }
                            break;
                        case 7: // X position (again)  dot 264, (+8), (+16) ...
                            if (PPU_Mask_ShowBackground_Delayed || PPU_Mask_ShowSprites_Delayed) // if rendering has been enabled for at least one cycle
                            {
                                // set this object's X position in the array... again.
                                PPU_SpriteXposition[selectedShifter] = PPU_OAMBuffer; // read X pos again
                                                                                      // but also: set up the second bit plane

                                PPU_CheckPAR();
                                PPU_PatternAddressRegister_CHR |= 8;
                                PPU_AddressBus = (ushort)((PPU_PatternAddressRegister_CHR & 0xFF00) | PPU_OctalLatch);

                                PPU_SpritePatternH = FetchVideoMemory();
                                if (((PPU_SpriteAttribute[selectedShifter] >> 6) & 1) == 1) // Attributes are set up to flip X
                                {
                                    PPU_SpritePatternH = Flip(PPU_SpritePatternH);
                                }
                                PPU_SpriteShiftRegisterH[selectedShifter] = PPU_SpritePatternH;

                                // in-range check. (The pre-render line ends up checking scanline 5 due to the `& 0xFF`.
                                if (!(PPU_OAM_VerticalOffset < (PPU_Spritex16 ? 16 : 8)))
                                {
                                    PPU_SpriteShiftRegisterH[selectedShifter] = 0; // clear the value in this shift register if this object isn't in range.
                                }
                                IncrementOAM2Address();
                            }
                            break;
                    }
                if (PPU_ALE && !PPU_READ)
                {
                    PPU_OctalLatch = (byte)PPU_AddressBus;
                }

                if (PPU_OAMCorruptionRenderingDisabledOutOfVBlank && (PPUClock == 1 || PPUClock == 2))
                {
                    PPU_OAMCorruptionRenderingDisabledOutOfVBlank = false;
                    PPU_OAMCorruptionRenderingDisabledOutOfVBlank_Instant = false;
                    PPU_PendingOAMCorruption = true;
                    PPU_OAMCorruptionIndex = OAM2Address; // this value will be used when rendering is re-enabled and the corruption occurs
                }

            }
            else
            {
                // cycles 320 to 340
                if (PPU_OAMCorruptionRenderingDisabledOutOfVBlank || PPU_OAMCorruptionRenderingDisabledOutOfVBlank_Instant)
                {
                    PPU_OAMCorruptionRenderingDisabledOutOfVBlank = false;
                    PPU_OAMCorruptionRenderingDisabledOutOfVBlank_Instant = false;
                    PPU_PendingOAMCorruption = true;
                    PPU_OAMCorruptionIndex = OAM2Address; // this value will be used when rendering is re-enabled and the corruption occurs
                }
                if ((PPU_Mask_ShowSprites || PPU_Mask_ShowBackground))
                {
                    PPU_OAMBuffer_In = OAM2[OAM2Address];
                }
                else
                {
                    PPU_OAMBuffer_In = OAM[PPUOAMAddress]; // The OAM buffer is updated with Primary OAM when rendering is disabled.
                }
                if (PPU_Dot == 339)
                {
                    for (int i = 0; i < 8; i++)
                    {
                        if ((PPU_Mask_ShowSprites || PPU_Mask_ShowBackground))
                        {
                        }
                        else
                        {
                            PPU_SpriteShifterCounter[i] = 0;
                        }
                    }
                }
            }
            // and that's all for sprite evaluation!
        }

void PPU_Render_CalculatePixel(bool borders)
{
            // dots 1 through 256
            if (PPU_Dot > 256)
            {
                borders = true;
            }
            if (PPU_Dot <= 256 || borders)
            {
                // there are 8 palettes in the PPU
                // 4 are for the background, and the other 4 are for sprites.
                byte Palette = 0;
                // each of these palettes have 4 colors
                byte Color = 0;
                if (!borders)
                {
                    if (PPU_Mask_ShowBackground && (PPU_Dot > 8 || PPU_Mask_8PxShowBackground)) // if rendering is enables for this pixel
                    {
                        byte col0 = (byte)(((PPU_BackgroundPatternShiftRegisterL >> (15 - PPU_FineXScroll))) & 1); // take the bit from the shift register for the pattern low bit plane
                        byte col1 = (byte)(((PPU_BackgroundPatternShiftRegisterH >> (15 - PPU_FineXScroll))) & 1); // take the bit from the shift register for the pattern high bit plane
                        Color = (byte)((col1 << 1) | col0);

                        byte pal0 = (byte)(((PPU_BackgroundAttributeShiftRegisterL) >> (7 - PPU_FineXScroll)) & 1); // take the bit from the shift register for the attribute low bit plane
                        byte pal1 = (byte)(((PPU_BackgroundAttributeShiftRegisterH) >> (7 - PPU_FineXScroll)) & 1); // take the bit from the shift register for the attribute high bit plane
                        Palette = (byte)((pal1 << 1) | pal0);

                        if (Color == 0 && Palette != 0) // color 0 of all palettes are mirrors of color 0 of palette 0
                        {
                            Palette = 0;
                        }
                    }
                }
                // pretty much the same thing, but for sprites instead of background
                byte SpritePalette = 0;
                byte SpriteColor = 0;
                bool SpritePriority = false; // if set, this sprite will be in front of background tiles. Otherwise, it will only take priority if the background is using color 0.
                if (!borders)
                {
                    if (PPU_Mask_ShowSprites && (PPU_Dot > 8 || PPU_Mask_8PxShowSprites))
                    {
                        int i = 0;

                        // check all 8 objects in secondary OAM
                        while (i < 8)
                        {
                            if (PPU_SpriteShifterCounter[i] == 0 || SkippedPreRenderDot341) // if the shifter counter == 0 (the shifter counter is decremented each ppu cycle)
                            {
                                bool SpixelL = ((PPU_SpriteShiftRegisterL[i]) & 0x80) != 0; // take the bit from the shift register for the pattern low bit plane
                                bool SpixelH = ((PPU_SpriteShiftRegisterH[i]) & 0x80) != 0; // take the bit from the shift register for the pattern high bit plane
                                SpriteColor = 0;
                                if (SpixelL) { SpriteColor = 1; }
                                if (SpixelH) { SpriteColor |= 2; }

                                SpritePalette = (byte)((PPU_SpriteAttribute[i] & 0x03) | 0x04); // read the palette from secondary OAM attributes.
                                SpritePriority = ((PPU_SpriteAttribute[i] >> 5) & 1) == 0;      // read the priority from secondary OAM attributes.

                            }
                            else // if no objects are in range of this pixel...
                            {
                                i++; // try the next one
                                continue;
                            }

                            if (SpriteColor != 0) // if we found an object, exit the loop. This means, objects earlier in secondary OAM hive higher priority over sprites later in secondary OAM
                            {
                                break;
                            }

                            i++; // This pixel wasn't a part of the previous object. Try the next slot in secondary oam.
                        }

                        // if we hit sprite zero and both rendering background and sprites are enabled...
                        if (PPU_CanDetectSpriteZeroHit && i == 0 && PPU_CurrentScanlineContainsSpriteZero && PPU_Mask_ShowBackground && PPU_Mask_ShowSprites)
                        {
                            if (Color != 0 && SpriteColor != 0) // if both the background and sprites are visible on this pixel
                            {
                                if ((PPU_Mask_8PxShowSprites || PPU_Dot > 8) && PPU_Dot < 256) // and if this isn't on pixel 256, or in the first 8 pixels being masked away from the nametable, if that setting is enabled...
                                {
                                    PPUStatus_PendingSpriteZeroHit = true; // we did it! sprite zero hit achieved... the flag is set on teh next half-ppu-cycle.
                                    PPU_CanDetectSpriteZeroHit = false; // another sprite zero hit cannot occur until the end of next vblank.
                                }
                            }
                        }

                        // which do we draw, the background or the sprite?
                        if (Color == 0 && SpriteColor != 0) // Well, if the background was using color 0, and the sprite wasn't,  always draw the sprite.
                        {
                            Color = SpriteColor; // I'm just reusing this background color variable.
                            Palette = SpritePalette;       // I'm also just reusing the background palette variable.
                        }
                        else if (SpriteColor != 0) // the background color isn't zero...
                        {
                            if (SpritePriority) // if the sprite has priority, always draw the sprite.
                            {
                                Color = SpriteColor; // I'm just reusing this cackground color variable.
                                Palette = SpritePalette; // I'm also just reusing the background palette variable.
                            }
                        }
                    }
                }
                if ((PPU_Mask_ShowBackground || PPU_Mask_ShowSprites) && PPU_Scanline < 240) // if rendering is enabled...
                {
                    PaletteRAMAddress = (byte)(Palette << 2 | Color); // the Palette RAM address is determined by the palette and color we found.
                }
                else
                {
                    // rendering is disabled...
                    if ((PPU_v & 0x3F1F) >= 0x3F00) // if v points to palette ram:
                    {
                        PaletteRAMAddress = (byte)(PPU_v & 0x1F); // The palette RAM address is simply wherever the v register is. (bitwise and with $1F due to palette RAM mirroring)
                        if ((PaletteRAMAddress & 3) == 0)
                        {
                            PaletteRAMAddress &= 0x0F; // the transparent colors for sprites and backgrounds are shared.
                        }
                    }
                    else
                    {
                        // EXT Pins
                        PaletteRAMAddress = 0; // I'm not really emulating the EXT pins, and as far as I'm aware they aren't used in any games, official or homebrew.
                        // This is typically why the background color is using Palette[0] when rendering is disabled.
                    }
                }

                if (PPU_PaletteCorruptionRenderingDisabledOutOfVBlank || PPU_VRegisterChangedOutOfVBlank)
                {
                    PPU_VRegisterChangedOutOfVBlank = false;
                    PPU_PaletteCorruptionRenderingDisabledOutOfVBlank = false;
                    // PPU palette corruption!

                    CorruptPalettes(Color, Palette);
                    // This corruption also results in a single discolored pixel, and this occurs on all alignments.
                    // I'm not entirely sure how this works, and I think it's the *next* pixel that gets corrupt? More research needed.

                }

                DotColor = (byte)((PaletteRAM[0x00 | PaletteRAMAddress]) & 0x3F); // Get the color by reading from Palette RAM

                // though this is actually drawn to the screen 2 ppu cycles from now.
            }
        }

void CorruptPalettes(byte Color, byte Palette)
{
            // Depending on the index into a color palette being used to select a color being drawn when rendering was disabled during a nametable fetch on a visible pixel with the PPU V Register (bitwise AND with $3FFF) being >= $3C00...
            // Palettes get "corrupted" with a specific pattern.
            // This pattern is determined by:
            // The lowest nybble of the PPU's V register,
            // The color index into the palette,
            // and if this is using a sprite palette. (TODO: emulate this part)

            // All of this was determined by observations with a custom test cart.
            // It is entirely possible that the logic defined in this functions is incorrect, or possibly there are more factors at play.
            // As far as I can tell though, this is "good enough" emulation of palette corruption.

            if ((CPUClock & 3) != 2)
            {
                // this behavior occurs on other alignments, but seems consistent on alignment 2, and very hit or miss on other alignments.
                // Currently, I'm only emulating this on alignment 2, but I'll probably change this in the future.
                return;
            }


            byte CorruptedPalette[LEN(PaletteRAM)];
            for (int i = 0; i < LEN(CorruptedPalette); i++)
            {
                CorruptedPalette[i] = PaletteRAM[i];
            }

            switch (Color)
            {
                case 0:
                    // simply take the low nybble from the V register. that's the color to corrupt.
                    CorruptedPalette[PPU_v & 0xF] = (byte)((PaletteRAM[0] & PaletteRAM[PPU_v & 0xC]) | (PaletteRAM[0] & PaletteRAM[PPU_v & 0xF]) | (PaletteRAM[PPU_v & 0xC] & PaletteRAM[PPU_v & 0xF]));
                    // TODO: Nybble 7 can corrupt color F. It's inconsistent though, so I'll need to circle back to this.

                    break;
                case 1:

                    // To be honest, I'm not sure what's going on, so forgive the lack of comments.
                    // There's almost a pattern, but again- unsure on why this is how it behaves.
                    // and also it's likely this isn't entirely accurate, either due to mistyping something, or not enough research.

                    switch (PPU_v & 0xF)
                    {
                        case 0:
                            CorruptedPalette[0x0] = (byte)((PaletteRAM[0x1] & PaletteRAM[0xD]) | PaletteRAM[0x0]);
                            CorruptedPalette[0x4] = PaletteRAM[0x5];
                            CorruptedPalette[0x8] = PaletteRAM[0x9];
                            CorruptedPalette[0xC] = PaletteRAM[0xD];
                            break;
                        case 1:
                            break;
                        case 2:
                            CorruptedPalette[0x2] = (byte)((PaletteRAM[0x2] | PaletteRAM[0xD]) & PaletteRAM[0x3]);
                            CorruptedPalette[0x3] = (byte)((PaletteRAM[0x1] | PaletteRAM[0x2]) & PaletteRAM[0x3]);
                            CorruptedPalette[0x6] = (byte)((PaletteRAM[0x6] | PaletteRAM[0x5]) & PaletteRAM[0x7]);
                            CorruptedPalette[0xA] = (byte)((PaletteRAM[0xA] | PaletteRAM[0x9]) & PaletteRAM[0xB]);
                            CorruptedPalette[0xE] = PaletteRAM[0xD];
                            CorruptedPalette[0xF] = PaletteRAM[0xD];
                            break;
                        case 3:
                            CorruptedPalette[0x3] &= (byte)(PaletteRAM[0x1] | PaletteRAM[0xD]);
                            CorruptedPalette[0xF] = PaletteRAM[0xD];
                            break;
                        case 4:
                            CorruptedPalette[0x0] = PaletteRAM[0x1];
                            CorruptedPalette[0x4] = (byte)((PaletteRAM[0x5] & PaletteRAM[0xD]) | PaletteRAM[0x4]);
                            CorruptedPalette[0x8] = PaletteRAM[0x9];
                            CorruptedPalette[0xC] = PaletteRAM[0xD];
                            break;
                        case 5:
                            break;
                        case 6:
                            CorruptedPalette[0x2] = (byte)((PaletteRAM[0x2] | PaletteRAM[0x1]) & PaletteRAM[0x3]);
                            CorruptedPalette[0x6] = (byte)((PaletteRAM[0x6] | PaletteRAM[0x7]) & PaletteRAM[0xD]);
                            CorruptedPalette[0x7] = (byte)((PaletteRAM[0x7] | PaletteRAM[0x6]) & PaletteRAM[0x5]);
                            CorruptedPalette[0xA] = (byte)((PaletteRAM[0xA] | PaletteRAM[0x9]) & PaletteRAM[0xB]);
                            CorruptedPalette[0xE] = PaletteRAM[0xD];
                            CorruptedPalette[0xF] = PaletteRAM[0xD];
                            break;
                        case 7:
                            CorruptedPalette[0x7] &= (byte)(PaletteRAM[0x5] | PaletteRAM[0xD]);
                            CorruptedPalette[0xF] = PaletteRAM[0xD];
                            break;
                        case 8:
                            CorruptedPalette[0x0] = PaletteRAM[0x1];
                            CorruptedPalette[0x4] = PaletteRAM[0x5];
                            CorruptedPalette[0x8] = (byte)((PaletteRAM[0x9] & PaletteRAM[0xD]) | PaletteRAM[0x8]);
                            CorruptedPalette[0xC] = PaletteRAM[0xD];
                            break;
                        case 9:
                            break;
                        case 0xA:
                            CorruptedPalette[0x2] = (byte)((PaletteRAM[0x2] | PaletteRAM[0x1]) & PaletteRAM[0x3]);
                            CorruptedPalette[0x6] = (byte)((PaletteRAM[0x6] | PaletteRAM[0xD]) & PaletteRAM[0x7]);
                            CorruptedPalette[0xA] = (byte)((PaletteRAM[0xB] | PaletteRAM[0xD]) & PaletteRAM[0xA]);
                            CorruptedPalette[0xB] = (byte)((PaletteRAM[0x9] | PaletteRAM[0xA]) & PaletteRAM[0xB]);
                            CorruptedPalette[0xE] = PaletteRAM[0xD];
                            CorruptedPalette[0xF] = PaletteRAM[0xD];
                            break;
                        case 0xB:
                            CorruptedPalette[0xB] &= (byte)(PaletteRAM[0x9] | PaletteRAM[0xD]);
                            CorruptedPalette[0xF] = PaletteRAM[0xD];
                            break;
                        case 0xC:
                            CorruptedPalette[0x0] = PaletteRAM[0x1];
                            CorruptedPalette[0x4] = PaletteRAM[0x5];
                            CorruptedPalette[0x8] = PaletteRAM[0x9];
                            CorruptedPalette[0xC] = PaletteRAM[0xD];
                            break;
                        case 0xD:
                            break;
                        case 0xE:
                            CorruptedPalette[0x2] = (byte)((PaletteRAM[0x2] | PaletteRAM[0x1]) & PaletteRAM[0x3]);
                            CorruptedPalette[0x6] = (byte)((PaletteRAM[0x6] | PaletteRAM[0xD]) & PaletteRAM[0x7]);
                            CorruptedPalette[0xA] = (byte)((PaletteRAM[0xA] | PaletteRAM[0x9]) & PaletteRAM[0xB]);
                            CorruptedPalette[0xE] = PaletteRAM[0xD];
                            CorruptedPalette[0xF] = PaletteRAM[0xD];
                            break;
                        case 0xF:
                            CorruptedPalette[0xF] = PaletteRAM[0xD];
                            break;
                    }


                    // In some tests with case A, bit 3 ($08) of color 3 can remove bit 2 ($04) from the value of color 0 for the purposes of the bitwise AND. It's inconsistent though.


                    break;
                case 2:

                    // To be honest, I'm not sure what's going on, so forgive the lack of comments.
                    // There's almost a pattern, but again- unsure on why this is how it behaves.
                    // and also it's likely this isn't entirely accurate, either due to mistyping something, or not enough research.

                    switch (PPU_v & 0xF)
                    {
                        case 0:
                            CorruptedPalette[0x0] = (byte)(PaletteRAM[0x0] | (PaletteRAM[0x2] & PaletteRAM[0xE]));
                            CorruptedPalette[0x4] = PaletteRAM[0x6];
                            CorruptedPalette[0x8] = PaletteRAM[0xA];
                            CorruptedPalette[0xC] = PaletteRAM[0xE];
                            break;
                        case 1:
                            CorruptedPalette[0x1] = (byte)((PaletteRAM[0x2] | PaletteRAM[0x1] | PaletteRAM[0xE]) & (PaletteRAM[0x3] | PaletteRAM[0xE]));
                            CorruptedPalette[0x3] = (byte)((PaletteRAM[0x2] | PaletteRAM[0xE] | 0x3C) & PaletteRAM[0x3]);
                            CorruptedPalette[0x5] = (byte)((PaletteRAM[0x6] | PaletteRAM[0x7]) & PaletteRAM[0x5]);
                            CorruptedPalette[0x9] = (byte)((PaletteRAM[0xA] | PaletteRAM[0xB]) & PaletteRAM[0x9]);
                            CorruptedPalette[0xD] = PaletteRAM[0xE];
                            CorruptedPalette[0xF] = PaletteRAM[0xE];
                            break;
                        case 2:
                            break;
                        case 3:
                            CorruptedPalette[0x3] &= (byte)(PaletteRAM[0x2] | PaletteRAM[0xE]);
                            CorruptedPalette[0xF] = PaletteRAM[0xE];
                            break;
                        case 4:
                            CorruptedPalette[0x0] = PaletteRAM[0x2];
                            CorruptedPalette[0x4] = (byte)(PaletteRAM[0x4] | (PaletteRAM[0x6] & PaletteRAM[0xE]));
                            CorruptedPalette[0x8] = PaletteRAM[0xA];
                            CorruptedPalette[0xC] = PaletteRAM[0xE];
                            break;
                        case 5:
                            CorruptedPalette[0x1] = (byte)((PaletteRAM[0x2] | PaletteRAM[0x1]) & PaletteRAM[0x3]);
                            CorruptedPalette[0x5] = (byte)((PaletteRAM[0xE] | PaletteRAM[0x6]) & PaletteRAM[0x5]);
                            CorruptedPalette[0x7] = (byte)((PaletteRAM[0xE] | PaletteRAM[0x6]) & PaletteRAM[0x7]);
                            CorruptedPalette[0xD] = PaletteRAM[0xE];
                            CorruptedPalette[0xF] = PaletteRAM[0xE];
                            break;
                        case 6:
                            break;
                        case 7:
                            CorruptedPalette[0x7] &= (byte)(PaletteRAM[0x6] | PaletteRAM[0xE]);
                            //CorruptedPalette[0xF] = PaletteRAM[0xE];
                            break;
                        case 8:
                            CorruptedPalette[0x0] = PaletteRAM[0x2];
                            CorruptedPalette[0x4] = PaletteRAM[0x6];
                            CorruptedPalette[0x8] = (byte)(PaletteRAM[0x8] | (PaletteRAM[0xA] & PaletteRAM[0xE]));
                            CorruptedPalette[0xC] = PaletteRAM[0xE];
                            break;
                        case 9:
                            CorruptedPalette[0x1] = (byte)((PaletteRAM[0x2] | PaletteRAM[0x1]) & PaletteRAM[0x3]);
                            CorruptedPalette[0x5] = (byte)((PaletteRAM[0x6] | PaletteRAM[0x5]) & PaletteRAM[0x7]);
                            CorruptedPalette[0x9] = (byte)((PaletteRAM[0xE] | PaletteRAM[0xA] | 0x01) & PaletteRAM[0x9]);
                            CorruptedPalette[0xB] = (byte)((PaletteRAM[0xE] | PaletteRAM[0xA] | 0x31) & PaletteRAM[0xB]);
                            CorruptedPalette[0xD] = PaletteRAM[0xE];
                            CorruptedPalette[0xF] = PaletteRAM[0xE];
                            break;
                        case 0xA:
                            break;
                        case 0xB:
                            CorruptedPalette[0xB] &= (byte)(PaletteRAM[0xA] | PaletteRAM[0xE]);
                            CorruptedPalette[0xF] = PaletteRAM[0xE];
                            break;
                        case 0xC:
                            CorruptedPalette[0x0] = PaletteRAM[0x2];
                            CorruptedPalette[0x4] = PaletteRAM[0x6];
                            CorruptedPalette[0x8] = PaletteRAM[0xA];
                            CorruptedPalette[0xC] = PaletteRAM[0xE];
                            break;
                        case 0xD:
                            CorruptedPalette[0x1] = (byte)((PaletteRAM[0x2] | PaletteRAM[0x1]) & PaletteRAM[0x3]);
                            CorruptedPalette[0x5] = (byte)((PaletteRAM[0x6] | PaletteRAM[0x5]) & PaletteRAM[0x7]);
                            CorruptedPalette[0x9] = (byte)((PaletteRAM[0xA] | PaletteRAM[0x9]) & PaletteRAM[0xB]);
                            CorruptedPalette[0xD] = PaletteRAM[0xE];
                            CorruptedPalette[0xF] = PaletteRAM[0xE];
                            break;
                        case 0xE:
                            break;
                        case 0xF:
                            CorruptedPalette[0xF] = PaletteRAM[0xE];
                            break;
                    }


                    break;
                case 3:

                    // To be honest, I'm not sure what's going on, so forgive the lack of comments.
                    // There's almost a pattern, but again- unsure on why this is how it behaves.
                    // and also it's likely this isn't entirely accurate, either due to mistyping something, or not enough research.

                    switch (PPU_v & 0xF)
                    {
                        case 0:
                            CorruptedPalette[0x0] = (byte)((PaletteRAM[0x3] | (PaletteRAM[0xF] & PaletteRAM[0x0])));
                            CorruptedPalette[0x4] &= PaletteRAM[0x7];
                            CorruptedPalette[0x8] &= (byte)(PaletteRAM[0x9] | PaletteRAM[0xA] | PaletteRAM[0xB] | PaletteRAM[0xF] | 0x22); // magic number... Probably a temperature thing? I've seen 02, 22, 2C, or 2E
                            CorruptedPalette[0xC] = PaletteRAM[0xF];
                            break;
                        case 1:
                            CorruptedPalette[0x1] = (byte)((PaletteRAM[0x1] | PaletteRAM[0xF]) & PaletteRAM[0x3]);
                            CorruptedPalette[0x5] = PaletteRAM[0x7];
                            CorruptedPalette[0x9] = PaletteRAM[0xB];
                            CorruptedPalette[0xD] = PaletteRAM[0xF];
                            break;
                        case 2:
                            CorruptedPalette[0x2] = (byte)((PaletteRAM[0x3] | PaletteRAM[0xF]) & PaletteRAM[0x3]);
                            CorruptedPalette[0x6] = PaletteRAM[0x7];
                            CorruptedPalette[0xA] = PaletteRAM[0xB];
                            CorruptedPalette[0xE] = PaletteRAM[0xF];
                            break;
                        case 3:
                            break;
                        case 4:
                            CorruptedPalette[0x0] &= (byte)(((PaletteRAM[0xF] ^ 0xFF)) | PaletteRAM[0x1] | PaletteRAM[0x2] | PaletteRAM[0x3] | 0x7); // magic number... I've only seen it as 07 though.
                            CorruptedPalette[0x4] &= (byte)(PaletteRAM[0x7] | PaletteRAM[0xF]);
                            CorruptedPalette[0x8] &= (byte)(PaletteRAM[0xB] | PaletteRAM[0xF] | (PaletteRAM[0xC] ^ 0xFF));
                            CorruptedPalette[0xC] = (byte)((PaletteRAM[0x7] & PaletteRAM[0xF]) | PaletteRAM[0xC]);
                            break;
                        case 5:
                            CorruptedPalette[0x1] = PaletteRAM[0x3];
                            CorruptedPalette[0x5] = (byte)((PaletteRAM[0x5] | PaletteRAM[0xF]) & PaletteRAM[0x7]);
                            CorruptedPalette[0x9] = PaletteRAM[0xB];
                            CorruptedPalette[0xD] = PaletteRAM[0xF];
                            break;
                        case 6:
                            CorruptedPalette[0x2] = PaletteRAM[0x3];
                            CorruptedPalette[0x6] = (byte)((PaletteRAM[0x6] | PaletteRAM[0xF]) & PaletteRAM[0x7]);
                            CorruptedPalette[0xA] = PaletteRAM[0xB];
                            CorruptedPalette[0xE] = PaletteRAM[0xF];
                            break;
                        case 7:
                            break;
                        case 8:
                            CorruptedPalette[0x0] &= (byte)(((PaletteRAM[0xF] ^ 0xFF)) | PaletteRAM[0x1] | PaletteRAM[0x2] | PaletteRAM[0x3] | 0x23); // magic number... I've only seen it as 23 though.
                            CorruptedPalette[0x4] = (byte)(PaletteRAM[0x7]);
                            CorruptedPalette[0x8] &= (byte)(PaletteRAM[0xB] | PaletteRAM[0xF] | (PaletteRAM[0xC] ^ 0xFF));
                            CorruptedPalette[0xC] = (byte)((PaletteRAM[0xB] & PaletteRAM[0xF]) | PaletteRAM[0xC]);
                            break;
                        case 9:
                            CorruptedPalette[0x1] = PaletteRAM[0x3];
                            CorruptedPalette[0x5] = PaletteRAM[0x7];
                            CorruptedPalette[0x9] = (byte)((PaletteRAM[0x9] | PaletteRAM[0xF]) & PaletteRAM[0xB]);
                            CorruptedPalette[0xD] = PaletteRAM[0xF];
                            break;
                        case 0xA:
                            CorruptedPalette[0x2] = PaletteRAM[0x3];
                            CorruptedPalette[0x6] = PaletteRAM[0x7];
                            CorruptedPalette[0xA] = (byte)((PaletteRAM[0xA] | PaletteRAM[0xF]) & PaletteRAM[0xB]);
                            CorruptedPalette[0xE] = PaletteRAM[0xF];
                            break;
                        case 0xB:
                            break;
                        case 0xC:
                            CorruptedPalette[0x0] &= (byte)(((PaletteRAM[0xF] ^ 0xFF)) | PaletteRAM[0x1] | PaletteRAM[0x2] | PaletteRAM[0x3] | 0x37); // magic number... I've only seen it as 23 though.
                            CorruptedPalette[0x4] = PaletteRAM[0x7];
                            CorruptedPalette[0x8] &= (byte)(PaletteRAM[0xB] | 0x2F); // Magic number. I've seen 2F and 2E
                            CorruptedPalette[0xC] = PaletteRAM[0xF];
                            break;
                        case 0xD:
                            CorruptedPalette[0x1] = PaletteRAM[0x3];
                            CorruptedPalette[0x5] = PaletteRAM[0x7];
                            CorruptedPalette[0x9] = PaletteRAM[0xB];
                            CorruptedPalette[0xD] = PaletteRAM[0xF];
                            break;
                        case 0xE:
                            CorruptedPalette[0x2] = PaletteRAM[0x3];
                            CorruptedPalette[0x6] = PaletteRAM[0x7];
                            CorruptedPalette[0xA] = PaletteRAM[0xB];
                            CorruptedPalette[0xE] = PaletteRAM[0xF];
                            break;
                        case 0xF:
                            break;
                    }

                    break;


            }
            for (int i = 0; i < LEN(CorruptedPalette); i++)
            {
                PaletteRAM[i] = CorruptedPalette[i];
            }


        }

void PPU_Render_ShiftRegistersAndBitPlanes()
{
            byte cycleTick; // for the switch statement below, this checks which case to run on a given ppu cycle.
            cycleTick = (byte)((PPU_Dot+7) & 7);

            if (PPU_Dot >= 257 && PPU_Dot <= 320)
            {
                cycleTick = 1;
            }

            if (PPU_ALE && PPU_READ)
            {
                PPU_OctalLatch = (byte)PPU_AddressBus;
            }

            switch (cycleTick)
            {
                case 0:
                    PPU_PatternAddressRegister_NT = (ushort)(0x2000 + (PPU_v & 0x0FFF));
                    PPU_PAR_MUX = PPU_PatternAddressRegister_NT;
                    PPU_AddressBus = PPU_PAR_MUX;
                    break;
                case 1:
                    // fetch byte from Nametable
                    PPU_PatternAddressRegister_NT = (ushort)(0x2000 + (PPU_v & 0x0FFF)); // this happens again.
                    PPU_AddressBus = (ushort)((PPU_PatternAddressRegister_NT & 0xFF00) | PPU_OctalLatch);
                    PPU_RenderTemp = FetchVideoMemory();
                    PPU_Commit_NametableFetch = true;
                    break;
                case 2:
                    PPU_PatternAddressRegister_AT = (ushort)(0x23C0 | (PPU_v & 0x0C00) | ((PPU_v >> 4) & 0x38) | ((PPU_v >> 2) & 0x07));
                    PPU_PAR_MUX = PPU_PatternAddressRegister_AT;
                    PPU_AddressBus = PPU_PAR_MUX;
                    break;
                case 3:
                    // fetch attribute byte from attribute table
                    PPU_PatternAddressRegister_AT = (ushort)(0x23C0 | (PPU_v & 0x0C00) | ((PPU_v >> 4) & 0x38) | ((PPU_v >> 2) & 0x07)); // this happens again.
                    PPU_AddressBus = (ushort)((PPU_PatternAddressRegister_AT & 0xFF00) | PPU_OctalLatch);
                    PPU_RenderTemp = FetchVideoMemory();
                    PPU_Commit_AttributeFetch = true;
                    // now we only have the 2 bits we're looking for
                    break;
                case 4:
                    PPU_CheckPAR();
                    PPU_PatternAddressRegister_CHR &= 0x1FF7;
                    PPU_PAR_MUX = PPU_PatternAddressRegister_CHR;
                    PPU_AddressBus = PPU_PAR_MUX;
                    break;
                case 5:
                    // fetch pattern bits from value read off the nametable
                    PPU_AddressBus = (ushort)((PPU_PatternAddressRegister_CHR & 0xFF00) | PPU_OctalLatch);
                    PPU_RenderTemp = FetchVideoMemory();
                    PPU_Commit_PatternLowFetch = true;
                    break;
                case 6:
                    PPU_CheckPAR();
                    PPU_PatternAddressRegister_CHR |= 8;
                    PPU_PAR_MUX = PPU_PatternAddressRegister_CHR;
                    PPU_AddressBus = PPU_PAR_MUX;
                    break;
                case 7:
                    // fetch pattern bits with the new address
                    PPU_AddressBus = (ushort)((PPU_PatternAddressRegister_CHR & 0xFF00) | PPU_OctalLatch);
                    PPU_RenderTemp = FetchVideoMemory();
                    PPU_Commit_PatternHighFetch = true;
                    break;
            }

            if (PPU_ALE && !PPU_READ)
            {
                PPU_OctalLatch = (byte)PPU_AddressBus;
            }

        }

void PPU_Render_CommitShiftRegistersAndBitPlanes()
{
            if (PPU_Commit_NametableFetch)
            {
                PPU_Commit_NametableFetch = false;
                PPU_PatternAddressRegister_CHR &= 0x100F;
                if (PPU_Dot < 256 || PPU_Dot > 320)
                {
                    PPU_PatternAddressRegister_CHR |= (ushort)( (byte)(PPU_AddressBus) << 4);
                }
                else
                {
                    PPU_PatternAddressRegister_CHR |= (ushort)(PPU_SpritePattern << 4);
                }
            }
            if (PPU_Commit_AttributeFetch)
            {
                PPU_Commit_AttributeFetch = false;
                PPU_Attribute = PPU_RenderTemp;
                // 1 byte of attribute data is 4 tiles worth. determine which tile this is for.
                if ((PPU_v & 3) >= 2) // If this is on the right tile
                {
                    PPU_Attribute = (byte)(PPU_Attribute >> 2);
                }
                if ((((PPU_v & 0x3E0) >> 5) & 3) >= 2) // If this is on the bottom tile
                {
                    PPU_Attribute = (byte)(PPU_Attribute >> 4);
                }
                PPU_Attribute = (byte)(PPU_Attribute & 3);
            }
            if (PPU_Commit_PatternLowFetch)
            {
                PPU_Commit_PatternLowFetch = false;
                PPU_LowBitPlane = PPU_RenderTemp;
            }
            if (PPU_Commit_PatternHighFetch)
            {
                PPU_Commit_PatternHighFetch = false;
                PPU_HighBitPlane = PPU_RenderTemp;
                PPU_LoadShiftRegisters();
                PPU_IncrementScrollX();
            }
        }

void PPU_Render_ShiftRegistersAndBitPlanes_DummyNT()
{

            if (PPU_READ)
            {
                PPU_OctalLatch = (byte)PPU_AddressBus;
            }

            if (PPU_Dot == 0)
            {
                PPU_CheckPAR();
                PPU_PatternAddressRegister_CHR &= 0x1FF7;
                if (PPU_Scanline != 261) // This would not occur on the pre-render line.
                {
                    PPU_AddressBus = PPU_PatternAddressRegister_CHR;
                }
            }
            else
            {
                byte cycleTick; // for the switch statement below, this checks which case to run on a given ppu cycle.
                cycleTick = (byte)(PPU_Dot - 337);

                switch (cycleTick)
                {
                    case 0:
                        PPU_AddressBus = (ushort)(0x2000 + (PPU_v & 0x0FFF));
                        break;
                    case 1:
                        // fetch byte from Nametable
                        PPU_AddressBus = (ushort)(0x2000 + (PPU_v & 0x0FFF));
                        PPU_RenderTemp = FetchVideoMemory();
                        PPU_Commit_NametableFetch = true;
                        break;
                    case 2:
                        PPU_AddressBus = (ushort)(0x2000 + (PPU_v & 0x0FFF));
                        break;
                    case 3:
                        // fetch attribute byte from attribute table
                        PPU_RenderTemp = FetchVideoMemory();
                        //IGNORED NT FETCH: This actually doesn't update the NT register.
                        break;
                }
            }
            if (PPU_ALE && !PPU_READ)
            {
                PPU_OctalLatch = (byte)PPU_AddressBus;
            }
        }

void PPU_CheckPAR()
{
            // Some bits in PAR change based on context:
            if(PPU_Dot < 256 || PPU_Dot > 320)
            {
                // Which pattern table do we use for nametable fetches?
                PPU_PatternAddressRegister_CHR &= 0xFF8;
                PPU_PatternAddressRegister_CHR |= (ushort)(PPU_PatternSelect_Background ? 0x1000 : 0);
                PPU_PatternAddressRegister_CHR |= (ushort)((PPU_v & 0x7000) >> 12);
            }
            else
            {
                // Which pattern table do we use for sprite fetches?
                if(!PPU_Spritex16)
                {
                    bool flipy = (PPU_SpriteAttr & 0x80) != 0;
                    PPU_PatternAddressRegister_CHR &= 0xFF8;
                    PPU_PatternAddressRegister_CHR |= (ushort)(PPU_PatternSelect_Sprites ? 0x1000 : 0);
                    PPU_PatternAddressRegister_CHR |= (ushort)(flipy ? 7-(PPU_OAM_VerticalOffset & 0x7) : (PPU_OAM_VerticalOffset & 0x7));
                }
                else
                {
                    bool flipy = (PPU_SpriteAttr & 0x80) != 0;
                    PPU_PatternAddressRegister_CHR &= 0xFE8;
                    PPU_PatternAddressRegister_CHR |= (ushort)(((PPU_SpritePattern & 1) != 0) ? 0x1000 : 0); // Bit 0 of the OAM2 Pattern
                    PPU_PatternAddressRegister_CHR |= (ushort)(flipy ? 7 - (PPU_OAM_VerticalOffset & 0x7) : (PPU_OAM_VerticalOffset & 0x7));
                    PPU_PatternAddressRegister_CHR |= (ushort)(((PPU_OAM_VerticalOffset & 0x08) ^ (flipy ? 8 : 0)) <<1);
                }
            }
        }

byte Flip(byte b)
{
            b = (byte)(((b & 0xF0) >> 4) | ((b & 0xF) << 4));
            b = (byte)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
            b = (byte)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
            return b;
        }

void PPU_UpdateBackgroundShiftRegisters()
{
            PPU_BackgroundPatternShiftRegisterL = (ushort)(PPU_BackgroundPatternShiftRegisterL << 1); // shift 1 bit to the left. Bring in a 0.
            PPU_BackgroundPatternShiftRegisterH = (ushort)((PPU_BackgroundPatternShiftRegisterH << 1) | 1); // shift 1 bit to the left. Bring in a 1.
            PPU_BackgroundAttributeShiftRegisterL = (ushort)((PPU_BackgroundAttributeShiftRegisterL << 1) | (PPU_AttributeLatchRegister & 1)); // shift 1 bit to the left. Bring in Attribute low bit.
            PPU_BackgroundAttributeShiftRegisterH = (ushort)((PPU_BackgroundAttributeShiftRegisterH << 1) | ((PPU_AttributeLatchRegister & 10) >> 1)); // shift 1 bit to the left. Bring in Attribute high bit.
        }

void UpdateSpriteShiftRegisters()
{
            if (PPU_Dot <= 256) // the shift registers for sprites are shifted after the rendering process.
            {
                // shift all 8 sprite shift registers.
                int i = 0;
                while (i < 8)
                {
                    if (PPU_SpriteShifterCounter[i] > 0 && !SkippedPreRenderDot341)
                    {
                        PPU_SpriteShifterCounter[i]--; // decrement the X position of all objects in secondary OAM. When this is zero, the ppu can draw it.
                    }
                    else
                    {
                        if ((PPU_Mask_ShowSprites || PPU_Mask_ShowBackground)) // this happens if rendering either sprites or background.
                        {
                            PPU_SpriteShiftRegisterL[i] = (byte)(PPU_SpriteShiftRegisterL[i] << 1); // shift 1 bit to the left.
                            PPU_SpriteShiftRegisterH[i] = (byte)(PPU_SpriteShiftRegisterH[i] << 1); // shift 1 bit to the left.
                        }
                    }
                    i++;
                }
            }
        }

void PPU_LoadShiftRegisters()
{
            // this runs as the first step of PPU_Render_ShiftRegistersAndBitPlanes(), using the values determined by the previous 8 steps of PPU_Render_ShiftRegistersAndBitPlanes().
            PPU_BackgroundPatternShiftRegisterL = (ushort)((PPU_BackgroundPatternShiftRegisterL & 0xFF00) | PPU_LowBitPlane);
            PPU_BackgroundPatternShiftRegisterH = (ushort)((PPU_BackgroundPatternShiftRegisterH & 0xFF00) | PPU_HighBitPlane);
            PPU_AttributeLatchRegister = PPU_Attribute;
        }

void PPU_IncrementScrollX()
{
            // used when setting up shift registers for the background
            // update the v register. Either increment it, or reset the scroll
            if ((PPU_v & 0x001F) == 31)
            {
                PPU_v &= 0xFFE0; // resetting the scroll
                PPU_v ^= 0x0400;
            }
            else
            {
                PPU_v++; // increment
            }
            PPU_v &= 0x7FFF;
        }

void PPU_IncrementScrollY()
{
            if (CopyV)
            {
                PPU_v = (ushort)(PPU_Update2006Value_Temp & PPU_Update2006Value); // This isn't actually accurate. More research needed.
            }
            else
            {
                if ((PPU_v & 0x7000) != 0x7000)
                {
                    PPU_v += 0x1000;
                }
                else
                {
                    PPU_v &= 0x0FFF;
                    int y = (PPU_v & 0x03E0) >> 5;
                    if (y == 29)
                    {
                        y = 0; // reset the Y value and also flip some other bit in the 'v' register
                        PPU_v ^= 0x0800;
                    }
                    else if (y == 31)
                    {
                        y = 0; // reset the Y value
                    }
                    else
                    {
                        y++; // increment the Y value
                    }
                    PPU_v = (ushort)((PPU_v & 0xFC1F) | (y << 5));
                }
            }
            PPU_v &= 0x7FFF;
        }

void PPU_ResetXScroll()
{
            // If a write to $2000 occurs during this ppu cycle, PPU_t will be the incorrect value!
            // The value of PPU_t will be corrected on the next ppu cycle, but it's already too late.
            // This is the "scanline bug" : https://www.nesdev.org/wiki/PPU_glitches#PPUCTRL
            // The bug is only visible if the nametable mirroring is vertical.
            PPU_v = (ushort)((PPU_v & 0x7BE0) | (PPU_t & 0x41F));
        }

void PPU_ResetYScroll()
{
            // The exact same issue from PPU_ResetXScroll() can happen here too, except this corrupts an entire frame.
            // The bug is only visible if the nametable mirroring is horizontal.
            //PPU_t = (ushort)((PPU_t & 0x7C1F) | (0x3C0)); //Uncomment this line to experiment with the "Attirbutes as tiles" bug.
            PPU_v = (ushort)((PPU_v & 0x41F) | (PPU_t & 0x7BE0));
        }

void DecayPPUDataBus()
{
            int i = 0;
            while (i < LEN(PPUBusDecay))
            {
                if (PPUBusDecay[i] > 0)
                {
                    PPUBusDecay[i]--;
                    if (PPUBusDecay[i] == 0)
                    {
                        PPUBus &= DecayBitmask[i];
                    }
                }
                i++;
            }
        }

void OAMDMA_Get()
{
            OAMAddressBus = (ushort)(DMAPage << 8 | DMAAddress);
            OAMDMA_Aligned = true;
            // the fetch happens regardless of halt
            OAM_InternalBus = Fetch(OAMAddressBus);
        }

void OAMDMA_Halted()
{
            if (cyc_trace_enabled) cyc_trace_dma(true); // NESRecomp
            Fetch(addressBus); // if halted, just read from the current address bus.
        }

void OAMDMA_Put()
{

            if (OAMDMA_Aligned) // if the DMA is aligned
            {
                Store(OAM_InternalBus, 0x2004); // write to OAM
                DMAAddress++;
                if (DMAAddress == 0) // if we overflow the DMA address
                {
                    DoOAMDMA = false; // we have completed the DMA.
                    OAMDMA_Aligned = false;
                    return;
                }
            }
            else // if this is an alignment cycle
            {
                if (cyc_trace_enabled) cyc_trace_dma(true); // NESRecomp
                Fetch(addressBus); // just read from the current address bus
            }

        }

void DMCDMA_Get()
{
            // now reload the DMC buffer.
            APU_DMC_Buffer = Fetch(APU_DMC_AddressCounter);

            APU_DMC_AddressCounter++;
            if (APU_DMC_AddressCounter == 0)
            {
                APU_DMC_AddressCounter = 0x8000;
            }
            if (APU_DMC_BytesRemaining > 0)
            {
                // due to writes to $4015 setting the BytesRemaining to 0 if disabled, this could potentially underflow without the if statement.
                APU_DMC_BytesRemaining--;
            }

            if (APU_DMC_BytesRemaining == 0)
            {
                //reset sample

                if (!APU_DMC_Loop)
                {
                    APU_Status_DMC = false;
                    if (APU_DMC_EnableIRQ) // if the DMC should fire an IRQ when it completes...
                    {
                        if (!OracleIRQFix()) IRQ_LevelDetector = true; // NESRecomp: ORACLE FIX (IRQ)
                        APU_Status_DMCInterrupt = true;
                    }
                }
                else
                {
                    StartDMCSample();
                }
            }
            DoDMCDMA = false;
            OAMDMA_Aligned = false;
            CannotRunDMCDMARightNow = 2;

        }

void DMCDMA_Halted()
{
            if (cyc_trace_enabled) cyc_trace_dma(true); // NESRecomp
            Fetch(addressBus);
        }

void DMCDMA_Put()
{
            if (cyc_trace_enabled) cyc_trace_dma(true); // NESRecomp
            Fetch(addressBus);
        }

void PollInterrupts()
{
            NMI_PreviousPinsSignal = NMI_PinsSignal;
            NMI_PinsSignal = NMILine;
            if (NMI_PinsSignal && !NMI_PreviousPinsSignal)
            {
                DoNMI = true;
            }
            DoIRQ = IRQLine && !flag_Interrupt;
        }

void PollInterrupts_CantDisableIRQ()
{
            NMI_PreviousPinsSignal = NMI_PinsSignal;
            NMI_PinsSignal = NMILine;
            if (NMI_PinsSignal && !NMI_PreviousPinsSignal)
            {
                DoNMI = true;
            }
            if (!DoIRQ)
            {
                DoIRQ = IRQLine && !flag_Interrupt;
            }
        }

void CompleteOperation()
{
            CPU_SYNC = true;
            operationCycle = 0xFF; // this will be incremented to 0.
            addressBus = programCounter;
            CPU_Read = true;
            IgnoreH = false;
        }

// NESRecomp: the DMA half of _6502(), split out so NESRecomp's CPU can let a DMA
// take CPU cycles exactly where TriCNES's would.
bool DMAWantsCycle()
{
            return (DoDMCDMA && (APU_Status_DMC || APU_ImplicitAbortDMC4015) && CPU_Read) || (DoOAMDMA && CPU_Read);
}

void RunDMACycle()
{
                if (cyc_trace_enabled) cyc_trace_dma(false); // NESRecomp
                // NESRecomp's CPU handles this itself: hw_dma_stalls on the
                // SH* fix-up cycle (recompiler/src/cyc_codegen.c, emit_sh).
                if (
                    (opCode == 0x93 && operationCycle == 4) ||
                    (opCode == 0x9B && operationCycle == 3) ||
                    (opCode == 0x9C && operationCycle == 3) ||
                    (opCode == 0x9E && operationCycle == 3) ||
                    (opCode == 0x9F && operationCycle == 3)
                    )
                {
                    IgnoreH = true;
                }

                if (DoOAMDMA && FirstCycleOfOAMDMA)
                {
                    FirstCycleOfOAMDMA = false;
                    if (!APU_PutCycle) // if the first cycle of an OAM DMA is a get cycle, it's a halt cycle.
                    {
                        OAMDMA_Halt = true;
                    }
                }

                if (APU_PutCycle)
                {
                    // Put cycle (write)
                    if (DoDMCDMA && DoOAMDMA) // if we're running both a DMC and OAM DMA.
                    {
                        if (DMCDMA_Halt && OAMDMA_Halt) // both halt cycles
                        {
                            OAMDMA_Halted();
                        }
                        else if (!OAMDMA_Halt && DMCDMA_Halt) // only DMC halted
                        {
                            OAMDMA_Put();
                        }
                        else if (OAMDMA_Halt && !DMCDMA_Halt) // only OAM halted
                        {
                            DMCDMA_Put(); // Can this logically ever happen?
                        }
                        else // none halted : OAM DMA has priority
                        {
                            OAMDMA_Put();
                        }
                    }
                    else // only performing a single DMA
                    {
                        if (DoDMCDMA) // only running DMC DMA
                        {
                            if (DMCDMA_Halt)
                            {
                                DMCDMA_Halted();
                            }
                            else
                            {
                                DMCDMA_Put();
                            }
                        }
                        else // only running OAM DMA
                        {
                            if (OAMDMA_Halt)
                            {
                                OAMDMA_Halted();
                            }
                            else
                            {
                                OAMDMA_Put();
                            }
                        }
                    }
                }
                else
                {
                    // Get cycle (read)
                    if (DoDMCDMA && DoOAMDMA) // if we're running both a DMC and OAM DMA.
                    {
                        if (DMCDMA_Halt && OAMDMA_Halt) // both halt cycles
                        {
                            DMCDMA_Halted();
                        }
                        else if (!OAMDMA_Halt && DMCDMA_Halt) // only DMC halted
                        {
                            OAMDMA_Get();
                        }
                        else if (OAMDMA_Halt && !DMCDMA_Halt) // only OAM halted
                        {
                            DMCDMA_Get();
                        }
                        else // none halted : DMC DMA has priority
                        {
                            DMCDMA_Get();
                        }
                    }
                    else
                    {
                        // only performing a single DMA
                        if (DoDMCDMA) // only running DMC DMA
                        {
                            if (DMCDMA_Halt)
                            {
                                DMCDMA_Halted();
                            }
                            else
                            {
                                DMCDMA_Get();
                            }
                        }
                        else // only running OAM DMA
                        {
                            if (OAMDMA_Halt)
                            {
                                OAMDMA_Halted();
                            }
                            else
                            {
                                OAMDMA_Get();
                            }
                        }
                    }

                    DMCDMA_Halt = false; // both halt cycles get cleared after a get cycle.
                    OAMDMA_Halt = false;
                }

            }

void EndOfCPUCycle()
{
            if (DoDMCDMA && APU_ImplicitAbortDMC4015)
            {
                APU_ImplicitAbortDMC4015 = false; // If this was delayed by a write cycle, it won't run at all.
            }
}

// NESRecomp oracle self-check. A DMA that halts the CPU re-reads addressBus,
// which TriCNES's CPU is meant to leave on the address of its next access.
// Count the CPU cycles where the access used a different address, by opcode
// (index 256: opcode fetches) and operationCycle.
static uint32_t oracle_stale_address[257][16];
static ushort oracle_cycle_address;
static int oracle_cycle_key = -1;

static void OracleCheckAccess(ushort Address)
{
            if (oracle_cycle_key < 0) return;
            if (Address != oracle_cycle_address) oracle_stale_address[oracle_cycle_key][operationCycle & 15]++;
            oracle_cycle_key = -1;
}

void cyc_oracle_address_report(void *file)
{
            FILE *f = (FILE *)file;
            unsigned sites = 0;
            for (int op = 0; op < 257; op++)
                for (int c = 0; c < 16; c++)
                    if (oracle_stale_address[op][c]) {
                        if (op == 256) fprintf(f, "opcode fetch: %u\n", oracle_stale_address[op][c]);
                        else fprintf(f, "opcode %02X cycle %d: %u\n", op, c, oracle_stale_address[op][c]);
                        sites++;
                    }
            fprintf(f, "%u (opcode, cycle) positions accessed an address other than addressBus\n", sites);
}

// ORACLE FIX (RDY). On the 2A03 a DMA halts the CPU through RDY, which the
// 6502 honors only on read cycles; a halted CPU keeps the address it is about
// to read on the address bus (MOS MCS6500 datasheet, RDY pin). TriCNES instead
// halts whenever its CPU_Read flag is set, which stays set on the push cycles
// of PHA, PHP and interrupt entry, and its halt cycles re-read the addressBus
// variable, which on many cycles still holds the previous address (see
// cyc_oracle_address_report). The oracle therefore finds the CPU's next access
// by running its next cycle on a copy of the CPU state with the bus
// disconnected, and lets a pending DMA take the cycle only if that access is a
// read, halting on its address.
static bool   oracle_probe;          // Fetch/Store record the access and do nothing else
static bool   oracle_probe_seen, oracle_probe_write;
static ushort oracle_probe_address;

#define ORACLE_CPU_STATE(X_) \
    X_(programCounter) X_(opCode) X_(stackPointer) X_(flag_Carry) X_(flag_Zero) X_(flag_Interrupt) \
    X_(flag_Decimal) X_(flag_Overflow) X_(flag_Negative) X_(status) X_(A) X_(X) X_(Y) X_(H) X_(IgnoreH) \
    X_(dataBus) X_(internalBus) X_(addressBus) X_(specialBus) X_(dl) X_(operationCycle) X_(CPU_SYNC) \
    X_(temporaryAddress) X_(CPU_Read) X_(DoBRK) X_(DoNMI) X_(DoIRQ) X_(DoReset) X_(NMI_PinsSignal) \
    X_(NMI_PreviousPinsSignal) X_(IRQLine) X_(FixHighByte) X_(dataPinsAreNotFloating) \
    X_(oracle_cycle_address) X_(oracle_cycle_key)

// Whether a pending DMA takes this CPU cycle. oracle_probe_address is the
// address the halted CPU holds.
// CYC_ORACLE_UNFIXED=RDY in the environment runs TriCNES's original rule,
// for measuring what the fix changes.
static int oracle_rdy_fix = -1;

static bool OracleDMATakesCycle()
{
            if (oracle_rdy_fix < 0) {
                const char *unfixed = getenv("CYC_ORACLE_UNFIXED");
                oracle_rdy_fix = !(unfixed && strstr(unfixed, "RDY"));
            }
            if (!oracle_rdy_fix) {
                oracle_probe_address = addressBus;
                return DMAWantsCycle();
            }
            // DMAWantsCycle() without TriCNES's CPU_Read condition.
            if (!((DoDMCDMA && (APU_Status_DMC || APU_ImplicitAbortDMC4015)) || DoOAMDMA)) return false;
#define ORACLE_SAVE(v) auto saved_##v = v;
#define ORACLE_RESTORE(v) v = saved_##v;
            ORACLE_CPU_STATE(ORACLE_SAVE)
            oracle_probe = true;
            oracle_probe_seen = false;
            _6502();
            oracle_probe = false;
            ORACLE_CPU_STATE(ORACLE_RESTORE)
#undef ORACLE_SAVE
#undef ORACLE_RESTORE
            return oracle_probe_seen && !oracle_probe_write;
}

void _6502()
{

            if (!oracle_probe && OracleDMATakesCycle()) // NESRecomp: ORACLE FIX (RDY) above
            {
                ushort CPU_AddressBus = addressBus; // the value TriCNES's CPU code expects next cycle
                addressBus = oracle_probe_address;
                RunDMACycle();
                addressBus = CPU_AddressBus;
            }
            else if (CPU_SYNC) // We are not running any DMAs, and this is the first cycle of an instruction.
            {
                oracle_cycle_address = addressBus; // NESRecomp
                oracle_cycle_key = 256;
                CPU_SYNC = false;
                if (cyc_trace_enabled && !oracle_probe) // NESRecomp
                    cyc_trace_instruction(programCounter, A, X, Y, stackPointer, (byte)(
                        (flag_Negative ? 0x80 : 0) | (flag_Overflow ? 0x40 : 0) | (flag_Decimal ? 0x08 : 0) |
                        (flag_Interrupt ? 0x04 : 0) | (flag_Zero ? 0x02 : 0) | (flag_Carry ? 0x01 : 0)));
                // cycle 0. fetch opcode:
                addressBus = programCounter;

                opCode = Fetch(addressBus); // Fetch the value at the program counter. This is the opcode.


                if (DoNMI) // If an NMI is occurring,
                {
                    opCode = 0; // replace the opcode with 0. (A BRK, which has modified behavior for NMIs)
                }
                else if (DoIRQ) // If an IRQ is occurring,
                {
                    opCode = 0; // replace the opcode with 0. (A BRK, which has modified behavior for IRQs)
                }
                else if (DoReset) // If a RESET is occurring,
                {
                    opCode = 0; // replace the opcode with 0. (A BRK, which has modified behavior for RESETs)
                }
                else if (opCode == 0) // Otherwise, if an interrupt is not occurring, and the opcode is already 0
                {
                    DoBRK = true; // There's also specific behavior for the BRK instruction if it is in-fact a BRK, and not an interrupt.
                }


                if ((!DoNMI && !DoIRQ && !DoReset)) // If we aren't running any interrupts...
                {
                    programCounter++; // the PC is incremented to the next address
                    addressBus = programCounter;
                }

                operationCycle = 1; // set this for use in the following CPU cycle.

            }
            else
            {
                oracle_cycle_address = addressBus; // NESRecomp
                oracle_cycle_key = opCode;
                // a really big switch statement.
                // depending on the value of the opcode, different behavior will take place.
                // this is how instructions work.

                // All instructions are labeled. If it's an undocumented opcode, I also write "***" next to it.

                switch (opCode)
                {
                    case 0x00: //BRK
                        switch (operationCycle)
                        {
                            case 1:
                                if (!DoBRK)
                                {
                                    Fetch(addressBus); //dummy fetch without incrementing PC.
                                }
                                else
                                {
                                    GetImmediate(); //dummy fetch and PC increment
                                }
                                break;
                            case 2:
                                if (!DoReset)
                                {
                                    Push((byte)(programCounter >> 8));
                                }
                                else
                                {
                                    ResetReadPush();
                                }
                                break;
                            case 3:
                                if (!DoReset)
                                {
                                    Push((byte)programCounter);
                                }
                                else
                                {
                                    ResetReadPush();
                                }
                                break;
                            case 4:
                                if (!DoReset)
                                {
                                    status = flag_Carry ? (byte)0x01 : (byte)0;
                                    status |= flag_Zero ? (byte)0x02 : (byte)0;
                                    status |= flag_Interrupt ? (byte)0x04 : (byte)0;
                                    status |= flag_Decimal ? (byte)0x08 : (byte)0;
                                    status |= DoBRK ? (byte)0x10 : (byte)0;
                                    status |= 0x20;
                                    status |= flag_Overflow ? (byte)0x40 : (byte)0;
                                    status |= flag_Negative ? (byte)0x80 : (byte)0;
                                    Push(status);
                                }
                                else
                                {
                                    ResetReadPush();
                                }
                                PollInterrupts(); // check for NMI?
                                break;
                            case 5:
                                if (DoNMI)
                                {
                                    programCounter = (ushort)((programCounter & 0xFF00) | (Fetch(0xFFFA)));
                                }
                                else if (DoReset)
                                {
                                    programCounter = (ushort)((programCounter & 0xFF00) | (Fetch(0xFFFC)));
                                }
                                else
                                {
                                    programCounter = (ushort)((programCounter & 0xFF00) | (Fetch(0xFFFE)));
                                }

                                break;
                            case 6:
                                if (DoNMI)
                                {
                                    programCounter = (ushort)((programCounter & 0xFF) | (Fetch(0xFFFB) << 8));
                                }
                                else if (DoReset)
                                {
                                    programCounter = (ushort)((programCounter & 0xFF) | (Fetch(0xFFFD) << 8));
                                }
                                else
                                {
                                    programCounter = (ushort)((programCounter & 0xFF) | (Fetch(0xFFFF) << 8));
                                }

                                CompleteOperation(); // notably, BRK does not check the NMI edge detector at the end of the instruction
                                DoReset = false;

                                DoNMI = false;
                                DoIRQ = false;
                                IRQLine = false;

                                DoBRK = false;

                                flag_Interrupt = true;



                                break;
                        }
                        break;

                    case 0x01: //(ORA, X)
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffX();
                                break;
                            case 5: // read from address
                                PollInterrupts();
                                Op_ORA(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x02: ///HLT ***
                        switch (operationCycle)
                        {
                            case 1:
                                dl = Fetch(addressBus);
                                break;
                            case 2:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                break;
                            case 3:
                            case 4:
                                addressBus = 0xFFFE;
                                Fetch(addressBus);
                                break;
                            case 5:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                break;
                            case 6:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                operationCycle = 5; //makes this loop infinitely.
                                break;
                        }
                        break;

                    case 0x03: //(SLO, X)  *** 
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffX();
                                break;
                            case 5: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 6: // write back to the address
                                Store(dl, addressBus);
                                break; // perform the operation
                            case 7:
                                PollInterrupts();
                                Op_SLO(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x04: //NOP zp ***
                        if (operationCycle == 1)
                        {
                            GetAddressZeroPage();
                        }
                        else
                        {
                            // read from address
                            PollInterrupts();
                            Fetch(addressBus);
                            CompleteOperation();
                        }
                        break;

                    case 0x05: //ORA zp
                        if (operationCycle == 1)
                        {
                            GetAddressZeroPage();
                        }
                        else
                        {
                            // read from address
                            PollInterrupts();
                            Op_ORA(Fetch(addressBus));
                            CompleteOperation();
                        }
                        break;

                    case 0x06: //ASL, zp
                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                break;
                            case 2: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 3: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 4: // perform operation
                                PollInterrupts();
                                Op_ASL(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x07: //SLO zp  *** 
                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                break;
                            case 2: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 3: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 4: // perform operation
                                PollInterrupts();
                                Op_SLO(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x08: //PHP

                        if (operationCycle == 1)
                        {
                            //dummy fetch
                            Fetch(addressBus);
                        }
                        else
                        {
                            PollInterrupts();
                            // read from address
                            status = flag_Carry ? (byte)0x01 : (byte)0;
                            status += flag_Zero ? (byte)0x02 : (byte)0;
                            status += flag_Interrupt ? (byte)0x04 : (byte)0;
                            status += flag_Decimal ? (byte)0x08 : (byte)0;
                            status += 0x10; //always set in PHP
                            status += 0x20; //always set in PHP
                            status += flag_Overflow ? (byte)0x40 : (byte)0;
                            status += flag_Negative ? (byte)0x80 : (byte)0;
                            Push(status);
                            CompleteOperation();
                        }
                        break;

                    case 0x09: //ORA Imm
                        PollInterrupts();
                        GetImmediate();
                        Op_ORA(dl);
                        CompleteOperation();
                        break;

                    case 0x0A: //ASL A
                        PollInterrupts();
                        Fetch(addressBus); // dummy read
                        Op_ASL_A();
                        CompleteOperation();
                        break;

                    case 0x0B: //ANC Imm ***
                        PollInterrupts();
                        GetImmediate();
                        A = (byte)(A & dl);
                        flag_Carry = A >= 0x80;
                        flag_Zero = A == 0;
                        flag_Negative = A >= 0x80;
                        CompleteOperation();

                        break;

                    case 0x0C: //NOP Absolute ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Fetch(addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x0D: //ORA Abs
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Op_ORA(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x0E: //ASL, Abs
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                break;
                            case 3: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 4: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 5:
                                PollInterrupts();
                                Op_ASL(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x0F: //SLO Abs  *** 
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                break;
                            case 3: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 4: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 5:
                                PollInterrupts();
                                Op_SLO(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x10: //BPL
                        switch (operationCycle)
                        {
                            case 1:
                                PollInterrupts();
                                GetImmediate();
                                if (flag_Negative)
                                {
                                    CompleteOperation();
                                }
                                break;
                            case 2:
                                addressBus = programCounter;
                                Fetch(addressBus); // dummy read
                                temporaryAddress = (ushort)(programCounter + ((dl >= 0x80) ? -(256 - dl) : dl));
                                programCounter = (ushort)((programCounter & 0xFF00) | (byte)((programCounter & 0xFF) + dl));
                                if ((temporaryAddress & 0xFF00) == (programCounter & 0xFF00))
                                {
                                    CompleteOperation();
                                }
                                break;
                            case 3: // read from address
                                PollInterrupts_CantDisableIRQ(); // If the first poll detected an IRQ, this second poll should not be allowed to un-set the IRQ.
                                addressBus = programCounter;
                                Fetch(addressBus); // dummy read
                                programCounter = (ushort)((programCounter & 0xFF) | (temporaryAddress & 0xFF00));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x11: //(ORA) Y
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffY(true);
                                break;
                            case 5: // read from address
                                PollInterrupts();
                                Op_ORA(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x12: ///HLT ***
                        switch (operationCycle)
                        {
                            case 1:
                                dl = Fetch(addressBus);
                                break;
                            case 2:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                break;
                            case 3:
                            case 4:
                                addressBus = 0xFFFE;
                                Fetch(addressBus);
                                break;
                            case 5:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                break;
                            case 6:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                operationCycle = 5; //makes this loop infinitely.
                                break;
                        }
                        break;

                    case 0x13: //(SLO) Y  *** 
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffY(false);
                                break;
                            case 5: // dummy read
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 6: // dummy write
                                Store(dl, addressBus);
                                break;
                            case 7: // read from address
                                PollInterrupts();
                                Op_SLO(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x14: //NOP zp, X ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffX();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Fetch(addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x15: //ORA zp, X
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffX();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Op_ORA(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x16: //ASL, zp X
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffX();
                                break;
                            case 3: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 4: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 5:
                                PollInterrupts();
                                Op_ASL(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x17: //SLO zp, X *** 
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffX();
                                break;
                            case 3: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 4: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 5:
                                PollInterrupts();
                                Op_SLO(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x18: //CLC
                        PollInterrupts();
                        Fetch(addressBus); // dummy read
                        flag_Carry = false;
                        CompleteOperation();
                        break;

                    case 0x19: //ORA Abs, Y
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffY(true);
                                break;
                            case 4: // read from address
                                PollInterrupts();
                                Op_ORA(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x1A: //NOP ***
                        PollInterrupts();
                        Fetch(addressBus);
                        CompleteOperation();
                        break;

                    case 0x1B: //SLO Abs Y *** 
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressAbsOffY(false);
                                if (operationCycle == 4) { CPU_Read = false; }
                                break;
                            case 5:// dummy write
                                Store(dl, addressBus);
                                break;
                            case 6:// read from address
                                PollInterrupts();
                                Op_SLO(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x1C: //NOP Abs, X ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffX(true);
                                break;
                            case 4: // read from address
                                PollInterrupts();
                                Fetch(addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x1D: //ORA Abs, X
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffX(true);
                                break;
                            case 4: // read from address
                                PollInterrupts();
                                Op_ORA(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x1E: //ASL, Abs, X
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressAbsOffX(false);
                                if (operationCycle == 4) { CPU_Read = false; }
                                break;
                            case 5:// dummy write
                                Store(dl, addressBus);
                                break;
                            case 6:// read from address
                                PollInterrupts();
                                Op_ASL(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;


                    case 0x1F: //SLO Abs, X *** 
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressAbsOffX(false);
                                if (operationCycle == 4) { CPU_Read = false; }
                                break;
                            case 5:// dummy write
                                Store(dl, addressBus);
                                break;
                            case 6:// read from address
                                PollInterrupts();
                                Op_SLO(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x20: //JSR

                        switch (operationCycle)
                        {
                            // this is pretty cursed, though according to visual6502, this is apparently what happens.
                            case 1: // fetch the byte that will be PC low
                                addressBus = programCounter;
                                dl = Fetch(addressBus);
                                programCounter++;
                                break;
                            case 2: // transfer stack pointer to address bus, and alu to stack pointer. I'm just reusing `dl` here, but this instruction actually uses the Arithmetic Logic Unit for this.
                                addressBus = (ushort)(0x100 | stackPointer);
                                stackPointer = dl;
                                CPU_Read = false;
                                Fetch(addressBus); // dummy read
                                break;
                            case 3: // push PC high to stack via address bus
                                Store((byte)((programCounter & 0xFF00) >> 8), addressBus);
                                addressBus = (ushort)((byte)(addressBus - 1) | 0x100);
                                break;
                            case 4: // push PC low to stack via address bus
                                Store((byte)(programCounter & 0xFF), addressBus);
                                addressBus = (ushort)((byte)(addressBus - 1) | 0x100);
                                specialBus = (byte)addressBus;
                                CPU_Read = true;
                                break;
                            case 5: // fetch PC High, transfer stack pointer to PC low, address bus to stack pointer.
                                PollInterrupts();
                                addressBus = programCounter;
                                programCounter = (ushort)((Fetch(addressBus) << 8) | stackPointer);
                                stackPointer = specialBus;
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x21: //(AND, X)
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffX();
                                break;
                            case 5: // read from address
                                PollInterrupts();
                                Op_AND(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x22: ///HLT ***
                        switch (operationCycle)
                        {
                            case 1:
                                dl = Fetch(addressBus);
                                break;
                            case 2:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                break;
                            case 3:
                            case 4:
                                addressBus = 0xFFFE;
                                Fetch(addressBus);
                                break;
                            case 5:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                break;
                            case 6:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                operationCycle = 5; //makes this loop infinitely.
                                break;
                        }
                        break;

                    case 0x23: //(RLA, X)  ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffX();
                                break;
                            case 5: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 6: // write back to the address
                                Store(dl, addressBus);
                                break; // perform the operation
                            case 7:
                                PollInterrupts();
                                Op_RLA(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x24: //BIT Zp
                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                break;
                            case 2: // read from address
                                PollInterrupts();
                                dl = Fetch(addressBus);
                                flag_Zero = (A & dl) == 0;
                                flag_Negative = (dl & 0x80) != 0;
                                flag_Overflow = (dl & 0x40) != 0;
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x25: //AND zp
                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                break;
                            case 2: // read from address
                                PollInterrupts();
                                Op_AND(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x26: //ROL zp
                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                break;
                            case 2: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 3: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 4: // perform operation
                                PollInterrupts();
                                Op_ROL(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x27: //RLA zp  ***
                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                break;
                            case 2: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 3: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 4: // perform operation
                                PollInterrupts();
                                Op_RLA(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x28: //PLP
                        switch (operationCycle)
                        {
                            case 1: //dummy fetch
                                Fetch(addressBus);
                                break;
                            case 2: //increment S
                                addressBus = (ushort)(0x100 + stackPointer);
                                Fetch(addressBus); // dummy read
                                stackPointer++;
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                addressBus = (ushort)(0x100 + stackPointer);
                                status = Fetch(addressBus);
                                flag_Carry = (status & 1) == 1;
                                flag_Zero = ((status & 0x02) >> 1) == 1;
                                flag_Interrupt = ((status & 0x04) >> 2) == 1;
                                flag_Decimal = ((status & 0x08) >> 3) == 1;
                                flag_Overflow = ((status & 0x40) >> 6) == 1;
                                flag_Negative = ((status & 0x80) >> 7) == 1;
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x29: //AND Imm
                        PollInterrupts();
                        GetImmediate();
                        Op_AND(dl);
                        CompleteOperation();
                        break;

                    case 0x2A: //ROL A
                        PollInterrupts();
                        Fetch(addressBus); // dummy read
                        Op_ROL_A();
                        CompleteOperation();
                        break;

                    case 0x2B: //ANC Imm *** (same as 0x0B)
                        PollInterrupts();
                        GetImmediate();
                        A = (byte)(A & dl);
                        flag_Carry = A >= 0x80;
                        flag_Zero = A == 0;
                        flag_Negative = A >= 0x80;
                        CompleteOperation();

                        break;

                    case 0x2C: //BIT Abs
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                dl = Fetch(addressBus);
                                flag_Zero = (A & dl) == 0;
                                flag_Negative = (dl & 0x80) != 0;
                                flag_Overflow = (dl & 0x40) != 0;
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x2D: //AND Abs
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Op_AND(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x2E: //ROL Abs
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                break;
                            case 3: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 4: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 5:
                                PollInterrupts();
                                Op_ROL(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x2F: //RLA Abs ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                break;
                            case 3: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 4: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 5:
                                PollInterrupts();
                                Op_RLA(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x30: //BMI
                        switch (operationCycle)
                        {
                            case 1:
                                PollInterrupts();
                                GetImmediate();
                                if (!flag_Negative)
                                {
                                    CompleteOperation();
                                }
                                break;
                            case 2:
                                addressBus = programCounter;
                                Fetch(addressBus); // dummy read
                                temporaryAddress = (ushort)(programCounter + ((dl >= 0x80) ? -(256 - dl) : dl));
                                programCounter = (ushort)((programCounter & 0xFF00) | (byte)((programCounter & 0xFF) + dl));
                                if ((temporaryAddress & 0xFF00) == (programCounter & 0xFF00))
                                {
                                    CompleteOperation();
                                }
                                break;
                            case 3: // read from address
                                PollInterrupts_CantDisableIRQ(); // If the first poll detected an IRQ, this second poll should not be allowed to un-set the IRQ.
                                addressBus = programCounter;
                                Fetch(addressBus); // dummy read
                                programCounter = (ushort)((programCounter & 0xFF) | (temporaryAddress & 0xFF00));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x31: //(AND), Y
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffY(true);
                                break;
                            case 5: // read from address
                                PollInterrupts();
                                Op_AND(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x32: ///HLT ***
                        switch (operationCycle)
                        {
                            case 1:
                                dl = Fetch(addressBus);
                                break;
                            case 2:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                break;
                            case 3:
                            case 4:
                                addressBus = 0xFFFE;
                                Fetch(addressBus);
                                break;
                            case 5:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                break;
                            case 6:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                operationCycle = 5; //makes this loop infinitely.
                                break;
                        }
                        break;
                    case 0x33: //(RLA), Y  ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffY(false);
                                break;
                            case 5: // dummy read
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 6: // dummy write
                                Store(dl, addressBus);
                                break;
                            case 7: // read from address
                                PollInterrupts();
                                Op_RLA(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x34: //NOP zp, X ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffX();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Fetch(addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x35: //AND zp, X
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffX();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Op_AND(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x36: //ROL zp, X
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffX();
                                break;
                            case 3: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 4: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 5:
                                PollInterrupts();
                                Op_ROL(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x37: //RLA zp, X  ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffX();
                                break;
                            case 3: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 4: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 5:
                                PollInterrupts();
                                Op_RLA(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x38: //SEC
                        PollInterrupts();
                        Fetch(addressBus); // dummy read
                        flag_Carry = true;
                        CompleteOperation();
                        break;

                    case 0x39: //AND Abs, Y
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffY(true);
                                break;
                            case 4: // read from address
                                PollInterrupts();
                                Op_AND(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x3A: //NOP ***
                        PollInterrupts();
                        addressBus = programCounter; Fetch(addressBus);
                        CompleteOperation();
                        break;

                    case 0x3B: //RLA Abs, Y ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressAbsOffY(false);
                                if (operationCycle == 4) { CPU_Read = false; }
                                break;
                            case 5:// dummy write
                                Store(dl, addressBus);
                                break;
                            case 6:// read from address
                                PollInterrupts();
                                Op_RLA(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x3C: //NOP Absolute, X ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffX(true);
                                break;
                            case 4: // read from address
                                PollInterrupts();
                                Fetch(addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x3D: //AND Abs, X
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffX(true);
                                break;
                            case 4: // read from address
                                PollInterrupts();
                                Op_AND(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x3E: //ROL Abs, X
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressAbsOffX(false);
                                if (operationCycle == 4) { CPU_Read = false; }
                                break;
                            case 5:// dummy write
                                Store(dl, addressBus);
                                break;
                            case 6:// read from address
                                PollInterrupts();
                                Op_ROL(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x3F: //RLA Abs, X ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressAbsOffX(false);
                                if (operationCycle == 4) { CPU_Read = false; }
                                break;
                            case 5:// dummy write
                                Store(dl, addressBus);
                                break;
                            case 6:// read from address
                                PollInterrupts();
                                Op_RLA(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x40: //RTI
                        switch (operationCycle)
                        {
                            case 1:
                                GetImmediate();
                                break;
                            case 2:
                                addressBus = (ushort)(0x100 | stackPointer);
                                Fetch(addressBus);
                                addressBus = (ushort)((byte)(addressBus + 1) | 0x100);
                                break;
                            case 3:
                                status = Fetch(addressBus);
                                flag_Carry = (status & 1) != 0;
                                flag_Zero = (status & 0x02) != 0;
                                flag_Interrupt = (status & 0x04) != 0;
                                flag_Decimal = (status & 0x08) != 0;
                                flag_Overflow = (status & 0x40) != 0;
                                flag_Negative = (status & 0x80) != 0;

                                addressBus = (ushort)((byte)(addressBus + 1) | 0x100);
                                break;
                            case 4:
                                dl = Fetch(addressBus);
                                programCounter = (ushort)((programCounter & 0xFF00) | dl); //technically not accurate, as this happens in cycle 5
                                addressBus = (ushort)((byte)(addressBus + 1) | 0x100);
                                break;
                            case 5:
                                PollInterrupts();
                                dl = Fetch(addressBus);
                                programCounter = (ushort)((programCounter & 0xFF) | (dl << 8));
                                stackPointer = (byte)addressBus;
                                CompleteOperation();
                                break;

                        }
                        break;

                    case 0x41: //(EOR X)
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffX();
                                break;
                            case 5: // read from address
                                PollInterrupts();
                                Op_EOR(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x42: ///HLT ***
                        switch (operationCycle)
                        {
                            case 1:
                                dl = Fetch(addressBus);
                                break;
                            case 2:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                break;
                            case 3:
                            case 4:
                                addressBus = 0xFFFE;
                                Fetch(addressBus);
                                break;
                            case 5:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                break;
                            case 6:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                operationCycle = 5; //makes this loop infinitely.
                                break;
                        }
                        break;

                    case 0x43: //(SRE, X) ***

                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffX();
                                break;
                            case 5: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 6: // write back to the address
                                Store(dl, addressBus);
                                break; // perform the operation
                            case 7:
                                PollInterrupts();
                                Op_SRE(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x44: //NOP zp ***
                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                break;
                            case 2: // read from address
                                PollInterrupts();
                                Fetch(addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x45: //EOR zp
                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                break;
                            case 2: // read from address
                                PollInterrupts();
                                Op_EOR(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x46: //LSR zp
                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                break;
                            case 2: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 3: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 4: // perform operation
                                PollInterrupts();
                                Op_LSR(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x47: //SRE zp ***

                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                break;
                            case 2: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 3: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 4: // perform operation
                                PollInterrupts();
                                Op_SRE(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x48: //PHA

                        switch (operationCycle)
                        {
                            case 1: //dummy fetch
                                dl = Fetch(addressBus);
                                break;
                            case 2: // read from address
                                PollInterrupts();
                                Push(A);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x49: //EOR Imm
                        PollInterrupts();
                        GetImmediate();
                        Op_EOR(dl);
                        CompleteOperation();
                        break;

                    case 0x4A: //LSR A
                        PollInterrupts();
                        Fetch(addressBus); // dummy read
                        Op_LSR_A();
                        CompleteOperation();
                        break;

                    case 0x4B: //ASR Imm ***
                        PollInterrupts();
                        GetImmediate();
                        A = (byte)(A & dl);
                        Op_LSR_A();
                        CompleteOperation();
                        break;

                    case 0x4C: //JMP
                        if (operationCycle == 1)
                        {
                            GetAddressAbsolute();

                        }
                        else
                        {
                            PollInterrupts();
                            GetAddressAbsolute();
                            programCounter = addressBus;
                            CompleteOperation();
                        }
                        break;

                    case 0x4D: //EOR Abs
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Op_EOR(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x4E: //LSR abs

                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                break;
                            case 3: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 4: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 5:
                                PollInterrupts();
                                Op_LSR(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x4F: //SRE abs ***

                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                break;
                            case 3: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 4: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 5:
                                PollInterrupts();
                                Op_SRE(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x50: //BVC

                        switch (operationCycle)
                        {
                            case 1:
                                PollInterrupts();
                                GetImmediate();
                                if (flag_Overflow)
                                {
                                    CompleteOperation();
                                }
                                break;
                            case 2:
                                addressBus = programCounter;
                                Fetch(addressBus); // dummy read
                                temporaryAddress = (ushort)(programCounter + ((dl >= 0x80) ? -(256 - dl) : dl));
                                programCounter = (ushort)((programCounter & 0xFF00) | (byte)((programCounter & 0xFF) + dl));
                                if ((temporaryAddress & 0xFF00) == (programCounter & 0xFF00))
                                {
                                    CompleteOperation();
                                }
                                break;
                            case 3: // read from address
                                PollInterrupts_CantDisableIRQ(); // If the first poll detected an IRQ, this second poll should not be allowed to un-set the IRQ.
                                addressBus = programCounter;
                                Fetch(addressBus); // dummy read
                                programCounter = (ushort)((programCounter & 0xFF) | (temporaryAddress & 0xFF00));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x51: //(EOR), Y
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffY(true);
                                break;
                            case 5: // read from address
                                PollInterrupts();
                                Op_EOR(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x52: ///HLT ***
                        switch (operationCycle)
                        {
                            case 1:
                                dl = Fetch(addressBus);
                                break;
                            case 2:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                break;
                            case 3:
                            case 4:
                                addressBus = 0xFFFE;
                                Fetch(addressBus);
                                break;
                            case 5:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                break;
                            case 6:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                operationCycle = 5; //makes this loop infinitely.
                                break;
                        }
                        break;

                    case 0x53: //(SRE) Y ***

                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffY(false);
                                break;
                            case 5: // dummy read
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 6: // dummy write
                                Store(dl, addressBus);
                                break;
                            case 7: // read from address
                                PollInterrupts();
                                Op_SRE(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x54: //NOP zp, X ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffX();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Fetch(addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x55: //EOR zp, X
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffX();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Op_EOR(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x56: //LSR zp, X

                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffX();
                                break;
                            case 3: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 4: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 5:
                                PollInterrupts();
                                Op_LSR(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x57: //SRE zp X ***

                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffX();
                                break;
                            case 3: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 4: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 5:
                                PollInterrupts();
                                Op_SRE(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x58: //CLI
                        PollInterrupts();
                        Fetch(addressBus); // dummy read
                        flag_Interrupt = false;
                        CompleteOperation();
                        break;

                    case 0x59: //EOR Abs Y
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffY(true);
                                break;
                            case 4: // read from address
                                PollInterrupts();
                                Op_EOR(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x5A: //NOP ***
                        PollInterrupts();
                        addressBus = programCounter; Fetch(addressBus);
                        CompleteOperation();
                        break;

                    case 0x5B: //SRE abs, Y ***

                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressAbsOffY(false);
                                if (operationCycle == 4) { CPU_Read = false; }
                                break;
                            case 5:// dummy write
                                Store(dl, addressBus);
                                break;
                            case 6:// read from address
                                PollInterrupts();
                                Op_SRE(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x5C: //NOP Absolute, X ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffX(true);
                                break;
                            case 4: // read from address
                                PollInterrupts();
                                Fetch(addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x5D: //EOR Abs, X
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffX(true);
                                break;
                            case 4: // read from address
                                PollInterrupts();
                                Op_EOR(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x5E: //LSR abs, X

                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressAbsOffX(false);
                                if (operationCycle == 4) { CPU_Read = false; }
                                break;
                            case 5:// dummy write
                                Store(dl, addressBus);
                                break;
                            case 6:// read from address
                                PollInterrupts();
                                Op_LSR(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x5F: //SRE abs, X ***

                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressAbsOffX(false);
                                if (operationCycle == 4) { CPU_Read = false; }
                                break;
                            case 5:// dummy write
                                Store(dl, addressBus);
                                break;
                            case 6:// read from address
                                PollInterrupts();
                                Op_SRE(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x60: //RTS


                        switch (operationCycle)
                        {
                            case 1:
                                GetImmediate();
                                break;
                            case 2:
                                addressBus = (ushort)(0x100 | stackPointer);
                                Fetch(addressBus);
                                addressBus = (ushort)((byte)(addressBus + 1) | 0x100);
                                break;
                            case 3:
                                dl = Fetch(addressBus);
                                programCounter = (ushort)((programCounter & 0xFF00) | dl); //technically not accurate, as this happens in cycle 5
                                addressBus = (ushort)((byte)(addressBus + 1) | 0x100);
                                break;
                            case 4:
                                dl = Fetch(addressBus);
                                programCounter = (ushort)((programCounter & 0xFF) | (dl << 8));
                                break;
                            case 5:
                                PollInterrupts();
                                stackPointer = (byte)addressBus;
                                GetImmediate();
                                CompleteOperation();
                                break;

                        }
                        break;

                    case 0x61: //(ADC X)
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffX();
                                break;
                            case 5: // read from address
                                PollInterrupts();
                                Op_ADC(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x62: ///HLT ***
                        switch (operationCycle)
                        {
                            case 1:
                                dl = Fetch(addressBus);
                                break;
                            case 2:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                break;
                            case 3:
                            case 4:
                                addressBus = 0xFFFE;
                                Fetch(addressBus);
                                break;
                            case 5:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                break;
                            case 6:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                operationCycle = 5; //makes this loop infinitely.
                                break;
                        }
                        break;

                    case 0x63: //(RRA X) ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffX();
                                break;
                            case 5: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 6: // write back to the address
                                Store(dl, addressBus);
                                break; // perform the operation
                            case 7:
                                PollInterrupts();
                                Op_RRA(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x64: //NOP zp ***
                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                break;
                            case 2: // read from address
                                PollInterrupts();
                                Fetch(addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x65: //ADC Zp
                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                break;
                            case 2: // read from address
                                PollInterrupts();
                                Op_ADC(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x66: //ROR zp
                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                break;
                            case 2: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 3: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 4: // perform operation
                                PollInterrupts();
                                Op_ROR(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x67: //RRA zp ***
                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                break;
                            case 2: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 3: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 4: // perform operation
                                PollInterrupts();
                                Op_RRA(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;
                    case 0x68: //PLA

                        switch (operationCycle)
                        {
                            case 1: //dummy fetch
                                addressBus = programCounter;
                                Fetch(addressBus);
                                break;
                            case 2: // read from address
                                addressBus = (ushort)(0x100 | (stackPointer));
                                Fetch(addressBus); // dummy read
                                stackPointer++;
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                addressBus = (ushort)(0x100 | (stackPointer));
                                A = Fetch(addressBus);
                                flag_Zero = A == 0;
                                flag_Negative = A >= 0x80;
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x69: //ADC Imm
                        PollInterrupts();
                        GetImmediate();
                        Op_ADC(dl);
                        CompleteOperation();
                        break;

                    case 0x6A: //ROR A
                        PollInterrupts();
                        Fetch(addressBus); // dummy read
                        Op_ROR_A();
                        CompleteOperation();
                        break;

                    case 0x6B: // ARR ***
                        PollInterrupts();
                        GetImmediate();
                        A = (byte)(A & dl);
                        Op_ROR_A();
                        flag_Zero = A == 0;
                        flag_Carry = ((A & 0x40) >> 6) == 1;
                        flag_Overflow = (((A & 0x20) >> 5) ^ ((A & 0x40) >> 6)) == 1;
                        flag_Negative = A >= 0x80;
                        CompleteOperation();
                        break;

                    case 0x6C: //JMP (indirect)
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                break;
                            case 3:
                                specialBus = Fetch(addressBus); // Okay, this doesn't actually use the SB register. I'm just reusing that variable.
                                break;
                            case 4:
                                PollInterrupts();
                                dl = Fetch((ushort)((addressBus & 0xFF00) | (byte)(addressBus + 1)));
                                programCounter = (ushort)((dl << 8) | specialBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x6D: //ADC Abs
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Op_ADC(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x6E: //ROR Abs
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                break;
                            case 3: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 4: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 5:
                                PollInterrupts();
                                Op_ROR(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x6F: //RRA Abs ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                break;
                            case 3: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 4: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 5:
                                PollInterrupts();
                                Op_RRA(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x70: //BVS
                        switch (operationCycle)
                        {
                            case 1:
                                PollInterrupts();
                                GetImmediate();
                                if (!flag_Overflow)
                                {
                                    CompleteOperation();
                                }
                                break;
                            case 2:
                                addressBus = programCounter;
                                Fetch(addressBus); // dummy read
                                temporaryAddress = (ushort)(programCounter + ((dl >= 0x80) ? -(256 - dl) : dl));
                                programCounter = (ushort)((programCounter & 0xFF00) | (byte)((programCounter & 0xFF) + dl));
                                if ((temporaryAddress & 0xFF00) == (programCounter & 0xFF00))
                                {
                                    CompleteOperation();
                                }
                                break;
                            case 3: // read from address
                                PollInterrupts_CantDisableIRQ(); // If the first poll detected an IRQ, this second poll should not be allowed to un-set the IRQ.
                                addressBus = programCounter;
                                Fetch(addressBus); // dummy read
                                programCounter = (ushort)((programCounter & 0xFF) | (temporaryAddress & 0xFF00));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x71: //(ADC), Y
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffY(true);
                                break;
                            case 5: // read from address
                                PollInterrupts();
                                Op_ADC(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x72: ///HLT ***
                        switch (operationCycle)
                        {
                            case 1:
                                dl = Fetch(addressBus);
                                break;
                            case 2:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                break;
                            case 3:
                            case 4:
                                addressBus = 0xFFFE;
                                Fetch(addressBus);
                                break;
                            case 5:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                break;
                            case 6:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                operationCycle = 5; //makes this loop infinitely.
                                break;
                        }
                        break;

                    case 0x73: //(RRA) Y ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffY(false);
                                break;
                            case 5: // dummy read
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 6: // dummy write
                                Store(dl, addressBus);
                                break;
                            case 7: // read from address
                                PollInterrupts();
                                Op_RRA(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x74: //NOP zp, X ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffX();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Fetch(addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x75: //ADC zp, X

                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffX();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Op_ADC(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x76: //ROR zp, X
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffX();
                                break;
                            case 3: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 4: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 5:
                                PollInterrupts();
                                Op_ROR(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x77: //RRA zp X ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffX();
                                break;
                            case 3: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 4: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 5:
                                PollInterrupts();
                                Op_RRA(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x78: //SEI
                        PollInterrupts();
                        Fetch(addressBus); // dummy read
                        flag_Interrupt = true;
                        CompleteOperation();
                        break;
                    case 0x79: //ADC Abs, Y

                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffY(true);
                                break;
                            case 4: // read from address
                                PollInterrupts();
                                Op_ADC(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x7A: //NOP ***
                        PollInterrupts();
                        addressBus = programCounter;
                        Fetch(addressBus);
                        CompleteOperation();
                        break;

                    case 0x7B: //RRA Abs, Y ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressAbsOffY(false);
                                if (operationCycle == 4) { CPU_Read = false; }
                                break;
                            case 5:// dummy write
                                Store(dl, addressBus);
                                break;
                            case 6:// read from address
                                PollInterrupts();
                                Op_RRA(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x7C: //NOP Absolute, X ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffX(true);
                                break;
                            case 4: // read from address
                                PollInterrupts();
                                Fetch(addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x7D: //ADC Abs, X

                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffX(true);
                                break;
                            case 4: // read from address
                                PollInterrupts();
                                Op_ADC(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x7E: //ROR Abs, X
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressAbsOffX(false);
                                if (operationCycle == 4) { CPU_Read = false; }
                                break;
                            case 5:// dummy write
                                Store(dl, addressBus);
                                break;
                            case 6:// read from address
                                PollInterrupts();
                                Op_ROR(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x7F: //RRA Abs, X ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressAbsOffX(false);
                                if (operationCycle == 4) { CPU_Read = false; }
                                break;
                            case 5:// dummy write
                                Store(dl, addressBus);
                                break;
                            case 6:// read from address
                                PollInterrupts();
                                Op_RRA(dl, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x80: //NOP Immediate ***
                        PollInterrupts();
                        GetImmediate();
                        CompleteOperation();
                        break;


                    case 0x81: //(STA X)
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffX();
                                if (operationCycle == 4) { CPU_Read = false; }
                                break;
                            case 5: // read from address
                                PollInterrupts();
                                Store(A, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x82: //NOP Immediate ***
                        PollInterrupts();
                        GetImmediate();
                        CompleteOperation();
                        break;

                    case 0x83: //(SAX X)
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffX();
                                if (operationCycle == 4) { CPU_Read = false; }
                                break;
                            case 5: // read from address
                                PollInterrupts();
                                Store((byte)(A & X), addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x84: //STY zp
                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                CPU_Read = false;
                                break;
                            case 2: // read from address
                                PollInterrupts();
                                Store(Y, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x85: //STA zp
                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                CPU_Read = false;
                                break;
                            case 2:
                                PollInterrupts();
                                Store(A, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x86: //STX zp
                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                CPU_Read = false;
                                break;
                            case 2:
                                PollInterrupts();
                                Store(X, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;
                    case 0x87: //SAX zp
                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                CPU_Read = false;
                                break;
                            case 2:
                                PollInterrupts();
                                Store((byte)(A & X), addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x88: //DEY

                        PollInterrupts();
                        Y--;
                        flag_Zero = Y == 0;
                        flag_Negative = Y >= 0x80;
                        Fetch(addressBus); // dummy read
                        CompleteOperation();

                        break;

                    case 0x89: //NOP Immediate ***
                        PollInterrupts();
                        GetImmediate();
                        CompleteOperation();

                        break;

                    case 0x8A: //TXA
                        PollInterrupts();
                        A = X;
                        flag_Zero = A == 0;
                        flag_Negative = A >= 0x80;
                        Fetch(addressBus); // dummy read
                        CompleteOperation();
                        break;

                    case 0x8B: //ANE
                        PollInterrupts();
                        GetImmediate();
                        //A = (((A | 0xFF) & X) & temp); 
                        // Magic = FF
                        A = (byte)((A | 0xFF) & X & dl); // 0xEE is also known as "MAGIC", and can supposedly be different depending on the CPU's temperature.
                        flag_Zero = A == 0;
                        flag_Negative = A >= 0x80;
                        CompleteOperation();
                        break;

                    case 0x8C: //STY Abs
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                if (operationCycle == 2) { CPU_Read = false; }
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Store(Y, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x8D: //STA Abs
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                if (operationCycle == 2) { CPU_Read = false; }
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Store(A, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x8E: //STX Abs
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                if (operationCycle == 2) { CPU_Read = false; }
                                break;
                            case 3:
                                PollInterrupts();
                                Store(X, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x8F: //SAX Abs
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                if (operationCycle == 2) { CPU_Read = false; }
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Store((byte)(A & X), addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x90: //BCC
                        switch (operationCycle)
                        {
                            case 1:
                                PollInterrupts();
                                GetImmediate();
                                if (flag_Carry)
                                {
                                    CompleteOperation();
                                }
                                break;
                            case 2:
                                addressBus = programCounter;
                                Fetch(addressBus); // dummy read
                                temporaryAddress = (ushort)(programCounter + ((dl >= 0x80) ? -(256 - dl) : dl));
                                programCounter = (ushort)((programCounter & 0xFF00) | (byte)((programCounter & 0xFF) + dl));
                                if ((temporaryAddress & 0xFF00) == (programCounter & 0xFF00))
                                {
                                    CompleteOperation();
                                }
                                break;
                            case 3: // read from address
                                PollInterrupts_CantDisableIRQ(); // If the first poll detected an IRQ, this second poll should not be allowed to un-set the IRQ.
                                addressBus = programCounter;
                                Fetch(addressBus); // dummy read
                                programCounter = (ushort)((programCounter & 0xFF) | (temporaryAddress & 0xFF00));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x91: //(STA), Y
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffY(false);
                                if (operationCycle == 4) { CPU_Read = false; }
                                break;
                            case 5:
                                PollInterrupts();
                                Store(A, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x92: ///HLT ***
                        switch (operationCycle)
                        {
                            case 1:
                                dl = Fetch(addressBus);
                                break;
                            case 2:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                break;
                            case 3:
                            case 4:
                                addressBus = 0xFFFE;
                                Fetch(addressBus);
                                break;
                            case 5:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                break;
                            case 6:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                operationCycle = 5; //makes this loop infinitely.
                                break;
                        }
                        break;

                    case 0x93: // (SHA) Y ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffY(false);
                                if (operationCycle == 4)
                                {
                                    CPU_Read = false;
                                }
                                break;
                            case 5: // read from address
                                PollInterrupts();
                                if ((temporaryAddress & 0xFF00) != (addressBus & 0xFF00))
                                {
                                    // if adding Y to the target address crossed a page boundary, this opcode has "gone unstable"
                                    addressBus = (ushort)((byte)addressBus | ((addressBus >> 8) /*& A*/ & X) << 8); // Alternate SHA behavior. The A register isn't used here!
                                }
                                // pd = the high byte of the target address + 1
                                if (IgnoreH)
                                {
                                    H = 0xFF;
                                }
                                Store((byte)(A & (X | 0xF5) & H), addressBus); // Alternate SHA behavior. X is ORed with a magic number. On my console, it's $F5 for a few hours, then it flickers from $F5 and $FD.
                                CompleteOperation();
                                break;
                        }


                        break;

                    case 0x94: //STY zp, X
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffX();
                                if (operationCycle == 2) { CPU_Read = false; }
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Store(Y, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x95: //STA zp, X

                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffX();
                                if (operationCycle == 2) { CPU_Read = false; }
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Store(A, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x96: //STX zp, Y
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffY();
                                if (operationCycle == 2) { CPU_Read = false; }
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Store(X, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x97: //SAX zp, Y
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffY();
                                if (operationCycle == 2) { CPU_Read = false; }
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Store((byte)(A & X), addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x98: //TYA
                        PollInterrupts();
                        A = Y;
                        Fetch(addressBus); // dummy read
                        flag_Zero = A == 0;
                        flag_Negative = A >= 0x80;
                        CompleteOperation();

                        break;

                    case 0x99: //STA Abs, Y
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffY(false);
                                if (operationCycle == 3) { CPU_Read = false; }
                                break;
                            case 4: // read from address
                                PollInterrupts();
                                Store(A, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x9A: //TXS
                        PollInterrupts();
                        stackPointer = X;
                        Fetch(addressBus); // dummy read
                        CompleteOperation();
                        break;


                    case 0x9B: //SHS, Abs Y ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffY(false);
                                if (operationCycle == 3) { CPU_Read = false; }
                                break;
                            case 4: // read from address
                                PollInterrupts();
                                if ((temporaryAddress & 0xFF00) != (addressBus & 0xFF00))
                                {
                                    // if adding Y to the target address crossed a page boundary, this opcode has "gone unstable"
                                    addressBus = (ushort)((byte)addressBus | ((addressBus >> 8) /*& A*/ & X) << 8); // Alternate SHS behavior. The A register isn't used here!
                                }
                                // pd = the high byte of the target address + 1
                                stackPointer = (byte)(A & X);
                                if (IgnoreH)
                                {
                                    H = 0xFF;
                                }
                                Store((byte)(A & (X | 0xF5) & H), addressBus); // Alternate SHS behavior. X is ORed with a magic number. On my console, it's $F5 for a few hours, then it flickers from $F5 and $FD.
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x9C: //SHY Abs, X ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffX(false);
                                if (operationCycle == 3) { CPU_Read = false; }
                                break;
                            case 4:
                                PollInterrupts();
                                if ((temporaryAddress & 0xFF00) != (addressBus & 0xFF00))
                                {
                                    // if adding X to the target address crossed a page boundary, this opcode has "gone unstable"
                                    addressBus = (ushort)((byte)addressBus | ((addressBus >> 8) & Y) << 8);
                                }
                                if (IgnoreH)
                                {
                                    H = 0xFF;
                                }
                                Store((byte)(Y & H), addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x9D: //STA Abs, X
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffX(false);
                                if (operationCycle == 3) { CPU_Read = false; }
                                break;
                            case 4:
                                PollInterrupts();
                                Store(A, addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x9E: // SHX Abs, Y***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffY(false);
                                if (operationCycle == 3) { CPU_Read = false; }
                                break;
                            case 4:
                                PollInterrupts();
                                // Not even close to what the documentation says this instruction does.
                                if ((temporaryAddress & 0xFF00) != (addressBus & 0xFF00))
                                {
                                    // if adding Y to the target address crossed a page boundary, this opcode has "gone unstable"
                                    addressBus = (ushort)((byte)addressBus | ((addressBus >> 8) & X) << 8);
                                }
                                if (IgnoreH)
                                {
                                    H = 0xFF;
                                }
                                Store((byte)(X & H), addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0x9F: // SHA Abs, Y***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffY(false);
                                if (operationCycle == 3) { CPU_Read = false; }
                                break;
                            case 4: // read from address
                                PollInterrupts();
                                if ((temporaryAddress & 0xFF00) != (addressBus & 0xFF00))
                                {
                                    // if adding Y to the target address crossed a page boundary, this opcode has "gone unstable"
                                    addressBus = (ushort)((byte)addressBus | ((addressBus >> 8) /*& A*/ & X) << 8); // Alternate SHA behavior. The A register isn't used here!
                                }
                                if (IgnoreH)
                                {
                                    H = 0xFF;
                                }
                                Store((byte)(A & (X | 0xF5) & H), addressBus); // Alternate SHA behavior. X is ORed with a magic number. On my console, it's $F5 for a few hours, then it flickers from $F5 and $FD.
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xA0: //LDY imm
                        PollInterrupts();
                        GetImmediate();
                        Y = dl;
                        flag_Zero = Y == 0;
                        flag_Negative = Y >= 0x80;
                        CompleteOperation();

                        break;

                    case 0xA1: //(LDA, X)
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffX();
                                break;
                            case 5: // read from address
                                PollInterrupts();
                                A = Fetch(addressBus);
                                flag_Zero = A == 0;
                                flag_Negative = A >= 0x80;
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xA2: //LDX imm
                        PollInterrupts();
                        GetImmediate();
                        X = dl;
                        flag_Zero = X == 0;
                        flag_Negative = X >= 0x80;
                        CompleteOperation();

                        break;

                    case 0xA3: //(LAX, X) ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffX();
                                break;
                            case 5:
                                PollInterrupts();
                                A = Fetch(addressBus);
                                X = A;
                                flag_Zero = X == 0;
                                flag_Negative = X >= 0x80;
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xA4: //LDY zp
                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                break;
                            case 2: // read from address
                                PollInterrupts();
                                Y = Fetch(addressBus);
                                flag_Zero = Y == 0;
                                flag_Negative = Y >= 0x80;
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xA5: //LDA zp
                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                break;
                            case 2: // read from address
                                PollInterrupts();
                                A = Fetch(addressBus);
                                flag_Zero = A == 0;
                                flag_Negative = A >= 0x80;
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xA6: //LDX zp
                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                break;
                            case 2: // read from address
                                PollInterrupts();
                                X = Fetch(addressBus);
                                flag_Zero = X == 0;
                                flag_Negative = X >= 0x80;
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xA7: //LAX zp ***
                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                break;
                            case 2: // read from address
                                PollInterrupts();
                                A = Fetch(addressBus);
                                X = A;
                                flag_Zero = X == 0;
                                flag_Negative = X >= 0x80;
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xA8: //TAY
                        PollInterrupts();
                        Y = A;
                        Fetch(addressBus); // dummy read
                        flag_Zero = A == 0;
                        flag_Negative = Y >= 0x80;
                        CompleteOperation();
                        break;

                    case 0xA9: //LDA Imm
                        PollInterrupts();
                        GetImmediate();
                        A = dl;
                        flag_Zero = A == 0;
                        flag_Negative = A >= 0x80;
                        CompleteOperation();
                        break;

                    case 0xAA: //TAX
                        PollInterrupts();
                        X = A;
                        Fetch(addressBus); // dummy read
                        flag_Zero = X == 0;
                        flag_Negative = X >= 0x80;
                        CompleteOperation();
                        break;

                    case 0xAB: //LXA ***
                        PollInterrupts();
                        GetImmediate();
                        A = (byte)((A | 0xFF) & dl); // 0xEE is also known as "MAGIC", and can supposedly be different depending on the CPU's temperature.
                        X = A;  // this instruction is basically XAA but using LAX behavior, so X is also affected..
                        flag_Negative = X >= 0x80;
                        flag_Zero = X == 0x00;
                        CompleteOperation();
                        break;

                    case 0xAC: //LDY Abs
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Y = Fetch(addressBus);
                                flag_Negative = Y >= 0x80;
                                flag_Zero = Y == 0x00;
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xAD: //LDA Abs
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                if(addressBus > 0x4000 && addressBus < 0x6000)
                                {

                                }
                                A = Fetch(addressBus);
                                flag_Negative = A >= 0x80;
                                flag_Zero = A == 0x00;
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xAE: //LDX Abs
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                X = Fetch(addressBus);
                                flag_Negative = X >= 0x80;
                                flag_Zero = X == 0x00;
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xAF: //LAX Abs ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                A = Fetch(addressBus);
                                X = A;
                                flag_Negative = X >= 0x80;
                                flag_Zero = X == 0x00;
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xB0: //BCS
                        switch (operationCycle)
                        {
                            case 1:
                                PollInterrupts();
                                GetImmediate();
                                if (!flag_Carry)
                                {
                                    CompleteOperation();
                                }
                                break;
                            case 2:
                                addressBus = programCounter;
                                Fetch(addressBus); // dummy read
                                temporaryAddress = (ushort)(programCounter + ((dl >= 0x80) ? -(256 - dl) : dl));
                                programCounter = (ushort)((programCounter & 0xFF00) | (byte)((programCounter & 0xFF) + dl));
                                if ((temporaryAddress & 0xFF00) == (programCounter & 0xFF00))
                                {
                                    CompleteOperation();
                                }
                                break;
                            case 3: // read from address
                                PollInterrupts_CantDisableIRQ(); // If the first poll detected an IRQ, this second poll should not be allowed to un-set the IRQ.
                                addressBus = programCounter;
                                Fetch(addressBus); // dummy read
                                programCounter = (ushort)((programCounter & 0xFF) | (temporaryAddress & 0xFF00));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xB1: //(LDA), Y

                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffY(true);
                                break;
                            case 5:
                                PollInterrupts();
                                A = Fetch(addressBus);
                                flag_Zero = A == 0;
                                flag_Negative = A >= 0x80;
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xB2: ///HLT ***
                        switch (operationCycle)
                        {
                            case 1:
                                dl = Fetch(addressBus);
                                break;
                            case 2:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                break;
                            case 3:
                            case 4:
                                addressBus = 0xFFFE;
                                Fetch(addressBus);
                                break;
                            case 5:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                break;
                            case 6:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                operationCycle = 5; //makes this loop infinitely.
                                break;
                        }
                        break;

                    case 0xB3: //(LAX), Y ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffY(true);
                                break;
                            case 5: // read from address
                                PollInterrupts();
                                A = Fetch(addressBus);
                                X = A;
                                flag_Zero = X == 0;
                                flag_Negative = X >= 0x80;
                                CompleteOperation();
                                break;
                        }
                        break;
                    case 0xB4: //LDY zp, X
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffX();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Y = Fetch(addressBus);
                                flag_Zero = Y == 0;
                                flag_Negative = Y >= 0x80;
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xB5: //LDA zp, X
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffX();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                A = Fetch(addressBus);
                                flag_Zero = A == 0;
                                flag_Negative = A >= 0x80;
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xB6: //LDX zp,  Y
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffY();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                X = Fetch(addressBus);
                                flag_Zero = X == 0;
                                flag_Negative = X >= 0x80;
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xB7: //LAX zp, Y ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffY();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                A = Fetch(addressBus);
                                X = A;
                                flag_Zero = X == 0;
                                flag_Negative = X >= 0x80;
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xB8: //CLV
                        PollInterrupts();
                        Fetch(addressBus); // dummy read
                        flag_Overflow = false;
                        CompleteOperation();
                        break;

                    case 0xB9: //LDA abs , Y
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffY(true);
                                break;
                            case 4: // read from address
                                PollInterrupts();
                                A = Fetch(addressBus);
                                flag_Zero = A == 0;
                                flag_Negative = A >= 0x80;
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xBA: //TSX

                        PollInterrupts();
                        X = stackPointer;
                        Fetch(addressBus); // dummy read
                        flag_Negative = X >= 0x80;
                        flag_Zero = X == 0;
                        CompleteOperation();
                        break;

                    case 0xBB: //LAE Abs, Y***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffY(true);
                                break;
                            case 4: // read from address
                                PollInterrupts();
                                dl = Fetch(addressBus);
                                A = (byte)(dl & stackPointer);
                                X = (byte)(dl & stackPointer);
                                stackPointer = (byte)(dl & stackPointer);
                                flag_Negative = X >= 0x80;
                                flag_Zero = X == 0;
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xBC: //LDY abs, X
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffX(true);
                                break;
                            case 4: // read from address
                                PollInterrupts();
                                Y = Fetch(addressBus);
                                flag_Negative = Y >= 0x80;
                                flag_Zero = Y == 0;
                                CompleteOperation();
                                break;
                        }
                        break;


                    case 0xBD: //LDA abs, X

                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffX(true);
                                break;
                            case 4: // read from address
                                PollInterrupts();
                                A = Fetch(addressBus);
                                flag_Negative = A >= 0x80;
                                flag_Zero = A == 0;
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xBE: //LDX abs , Y
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffY(true);
                                break;
                            case 4: // read from address
                                PollInterrupts();
                                X = Fetch(addressBus);
                                flag_Negative = X >= 0x80;
                                flag_Zero = X == 0;
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xBF: //LAX Abs, Y ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffY(true);
                                break;
                            case 4: // read from address
                                PollInterrupts();
                                A = Fetch(addressBus);
                                X = A;
                                flag_Negative = X >= 0x80;
                                flag_Zero = X == 0;
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xC0: //CPY Imm
                        PollInterrupts();
                        GetImmediate();
                        Op_CPY(dl);
                        CompleteOperation();

                        break;

                    case 0xC1: //(CMP X),
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffX();
                                break;
                            case 5: // read from address
                                PollInterrupts();
                                Op_CMP(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xC2: //NOP Immediate ***
                        PollInterrupts();
                        GetImmediate();
                        CompleteOperation();

                        break;

                    case 0xC3: //(DCP, X) ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffX();
                                break;
                            case 5: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 6: // write back to the address
                                Store(dl, addressBus);
                                break; // perform the operation
                            case 7:
                                PollInterrupts();
                                dl--;
                                Store(dl, addressBus);
                                Op_CMP(dl);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xC4: //CPY zp
                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                break;
                            case 2: // read from address
                                PollInterrupts();
                                Op_CPY(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xC5: //CMP zp
                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                break;
                            case 2: // read from address
                                PollInterrupts();
                                Op_CMP(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xC6: //DEC zp
                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                break;
                            case 2:
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 3:
                                Store(dl, addressBus); //dummy write
                                break;
                            case 4: // read from address
                                PollInterrupts();
                                Op_DEC(addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xC7: //DCP zp ***
                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                break;
                            case 2:
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 3:
                                Store(dl, addressBus); //dummy write
                                break;
                            case 4: // read from address
                                PollInterrupts();
                                Op_DEC(addressBus);
                                Op_CMP(dl);
                                CompleteOperation();
                                break;
                        }
                        break;


                    case 0xC8: //INY
                        PollInterrupts();
                        Y++;
                        Fetch(addressBus); // dummy read
                        flag_Zero = Y == 0;
                        flag_Negative = Y >= 0x80;
                        CompleteOperation();
                        break;

                    case 0xC9: //CMP Imm
                        PollInterrupts();
                        GetImmediate();
                        Op_CMP(dl);
                        CompleteOperation();
                        break;

                    case 0xCA: //DEX
                        PollInterrupts();
                        X--;
                        Fetch(addressBus); // dummy read
                        flag_Zero = X == 0;
                        flag_Negative = X >= 0x80;
                        CompleteOperation();

                        break;

                    case 0xCB: // AXS ***
                        PollInterrupts();
                        GetImmediate();
                        X = (byte)(X & A);
                        flag_Carry = X >= dl;
                        X -= dl;
                        flag_Zero = X == 0;
                        flag_Negative = (X >= 0x80);

                        CompleteOperation();
                        break;


                    case 0xCC: //CPY Abs
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Op_CPY(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xCD: //CMP Abs
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Op_CMP(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xCE: //DEC Abs
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                break;
                            case 3:
                                // dummy read
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 4:
                                // dummy write
                                Store(dl, addressBus);
                                break;
                            case 5: // write
                                PollInterrupts();
                                Op_DEC(addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xCF: //DCP Abs ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                break;
                            case 3:
                                // dummy read
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 4:
                                // dummy write
                                Store(dl, addressBus);
                                break;
                            case 5: // write
                                PollInterrupts();
                                Op_DEC(addressBus);
                                Op_CMP(dl);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xD0: //BNE
                        switch (operationCycle)
                        {
                            case 1:
                                PollInterrupts();
                                GetImmediate();
                                if (flag_Zero)
                                {
                                    CompleteOperation();
                                }
                                break;
                            case 2:
                                addressBus = programCounter;
                                Fetch(addressBus); // dummy read
                                temporaryAddress = (ushort)(programCounter + ((dl >= 0x80) ? -(256 - dl) : dl));
                                programCounter = (ushort)((programCounter & 0xFF00) | (byte)((programCounter & 0xFF) + dl));
                                if ((temporaryAddress & 0xFF00) == (programCounter & 0xFF00))
                                {
                                    CompleteOperation();
                                }
                                break;
                            case 3: // read from address
                                PollInterrupts_CantDisableIRQ(); // If the first poll detected an IRQ, this second poll should not be allowed to un-set the IRQ.
                                addressBus = programCounter;
                                Fetch(addressBus); // dummy read
                                programCounter = (ushort)((programCounter & 0xFF) | (temporaryAddress & 0xFF00));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xD1: //(CMP), Y
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffY(true);
                                break;
                            case 5: // read from address
                                PollInterrupts();
                                Op_CMP(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xD2: ///HLT ***
                        switch (operationCycle)
                        {
                            case 1:
                                dl = Fetch(addressBus);
                                break;
                            case 2:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                break;
                            case 3:
                            case 4:
                                addressBus = 0xFFFE;
                                Fetch(addressBus);
                                break;
                            case 5:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                break;
                            case 6:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                operationCycle = 5; //makes this loop infinitely.
                                break;
                        }
                        break;

                    case 0xD3: //(DCP) Y ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffY(false);
                                break;
                            case 5: // dummy read
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 6: // dummy write
                                Store(dl, addressBus);
                                break;
                            case 7: // read from address
                                PollInterrupts();
                                Op_DEC(addressBus);
                                Op_CMP(dl);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xD4: //NOP zp, X ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffX();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Fetch(addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xD5: //CMP zp, X
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffX();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Op_CMP(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xD6: //DEC zp, X

                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffX();
                                break;
                            case 3:
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 4:
                                Store(dl, addressBus); //dummy write
                                break;
                            case 5: // read from address
                                PollInterrupts();
                                Op_DEC(addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xD7: //DCP Zp X ***

                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffX();
                                break;
                            case 3:
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 4:
                                Store(dl, addressBus); //dummy write
                                break;
                            case 5: // read from address
                                PollInterrupts();
                                Op_DEC(addressBus);
                                Op_CMP(dl);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xD8: //CLD
                        PollInterrupts();
                        Fetch(addressBus); // dummy read
                        flag_Decimal = false;
                        CompleteOperation();

                        break;
                    case 0xD9: //CMP abs, Y
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffY(true);
                                break;
                            case 4: // read from address
                                PollInterrupts();
                                Op_CMP(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xDA: //NOP ***
                        PollInterrupts();
                        Fetch(addressBus); // dummy read
                        CompleteOperation();
                        break;

                    case 0xDB: //DCP Abs Y ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressAbsOffY(false);
                                if (operationCycle == 4) { CPU_Read = false; }
                                break;
                            case 5:// dummy write
                                Store(dl, addressBus);
                                break;
                            case 6:// read from address
                                PollInterrupts();
                                Op_DEC(addressBus);
                                Op_CMP(dl);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xDC: //NOP Absolute, X ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffX(true);
                                break;
                            case 4: // read from address
                                PollInterrupts();
                                Fetch(addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xDD: //CMP abs, X

                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffX(true);
                                break;
                            case 4: // read from address
                                PollInterrupts();
                                Op_CMP(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xDE: //DEC Abs X

                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressAbsOffX(false);
                                if (operationCycle == 4) { CPU_Read = false; }
                                break;
                            case 5:// dummy write
                                Store(dl, addressBus);
                                break;
                            case 6:// read from address
                                PollInterrupts();
                                Op_DEC(addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xDF: //DCP Abs X ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressAbsOffX(false);
                                if (operationCycle == 4) { CPU_Read = false; }
                                break;
                            case 5:// dummy write
                                Store(dl, addressBus);
                                break;
                            case 6:// read from address
                                PollInterrupts();
                                Op_DEC(addressBus);
                                Op_CMP(dl);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xE0: //CPX Imm
                        PollInterrupts();
                        GetImmediate();
                        Op_CPX(dl);
                        CompleteOperation();
                        break;

                    case 0xE1: //(SBC X)
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffX();
                                break;
                            case 5: // read from address
                                PollInterrupts();
                                Op_SBC(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xE2: //NOP Immediate ***
                        PollInterrupts();
                        GetImmediate();
                        CompleteOperation();
                        break;

                    case 0xE3: //(ISC, X) ***

                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffX();
                                break;
                            case 5: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 6: // write back to the address
                                Store(dl, addressBus);
                                break; // perform the operation
                            case 7:
                                PollInterrupts();
                                Op_INC(addressBus);
                                Op_SBC(dl);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xE4: //CPX zp
                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                break;
                            case 2: // read from address
                                PollInterrupts();
                                Op_CPX(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xE5: //SBC Zp

                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                break;
                            case 2: // read from address
                                PollInterrupts();
                                Op_SBC(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xE6: //INC zp
                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                break;
                            case 2: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 3: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 4: // perform operation
                                PollInterrupts();
                                Op_INC(addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xE7: //ISC zp ***
                        switch (operationCycle)
                        {
                            case 1:
                                GetAddressZeroPage();
                                break;
                            case 2: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 3: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 4: // perform operation
                                PollInterrupts();
                                Op_INC(addressBus);
                                Op_SBC(dl);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xE8: //INX
                        PollInterrupts();
                        Fetch(addressBus); // dummy read
                        X++;
                        flag_Zero = X == 0;
                        flag_Negative = X >= 0x80;
                        CompleteOperation();
                        break;

                    case 0xE9: //SBC Imm
                        PollInterrupts();
                        GetImmediate();
                        Op_SBC(dl);
                        CompleteOperation();
                        break;

                    case 0xEA: //NOP
                        PollInterrupts();
                        Fetch(addressBus); // dummy read
                        CompleteOperation();
                        break;

                    case 0xEB: //SBC Imm ***
                        PollInterrupts();
                        GetImmediate();
                        Op_SBC(dl);
                        CompleteOperation();
                        break;

                    case 0xEC: //CPX Abs
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Op_CPX(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xED: //SBC Abs

                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Op_SBC(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xEE: //INC Abs
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                if (addressBus == 0x4014)
                                {

                                }
                                break;
                            case 3: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 4: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 5:
                                PollInterrupts();
                                Op_INC(addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xEF: //ISC Abs ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressAbsolute();
                                break;
                            case 3: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 4: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 5:
                                PollInterrupts();
                                Op_INC(addressBus);
                                Op_SBC(dl);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xF0: //BEQ
                        switch (operationCycle)
                        {
                            case 1:
                                PollInterrupts();
                                GetImmediate();
                                if (!flag_Zero)
                                {
                                    CompleteOperation();
                                }
                                break;
                            case 2:
                                addressBus = programCounter;
                                Fetch(addressBus); // dummy read
                                temporaryAddress = (ushort)(programCounter + ((dl >= 0x80) ? -(256 - dl) : dl));
                                programCounter = (ushort)((programCounter & 0xFF00) | (byte)((programCounter & 0xFF) + dl));
                                if ((temporaryAddress & 0xFF00) == (programCounter & 0xFF00))
                                {
                                    CompleteOperation();
                                }
                                break;
                            case 3: // read from address
                                PollInterrupts_CantDisableIRQ(); // If the first poll detected an IRQ, this second poll should not be allowed to un-set the IRQ.
                                addressBus = programCounter;
                                Fetch(addressBus); // dummy read
                                programCounter = (ushort)((programCounter & 0xFF) | (temporaryAddress & 0xFF00));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xF1: //(SBC) Y
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffY(true);
                                break;
                            case 5: // read from address
                                PollInterrupts();
                                Op_SBC(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xF2: ///HLT ***
                        switch (operationCycle)
                        {
                            case 1:
                                dl = Fetch(addressBus);
                                break;
                            case 2:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                break;
                            case 3:
                            case 4:
                                addressBus = 0xFFFE;
                                Fetch(addressBus);
                                break;
                            case 5:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                break;
                            case 6:
                                addressBus = 0xFFFF;
                                Fetch(addressBus);
                                operationCycle = 5; //makes this loop infinitely.
                                break;
                        }
                        break;

                    case 0xF3: //(ISC) Y
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressIndOffY(false);
                                break;
                            case 5: // dummy read
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 6: // dummy write
                                Store(dl, addressBus);
                                break;
                            case 7: // read from address
                                PollInterrupts();
                                Op_INC(addressBus);
                                Op_SBC(dl);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xF4: //NOP zp, X ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffX();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Fetch(addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xF5: //SBC Zp, X

                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffX();
                                break;
                            case 3: // read from address
                                PollInterrupts();
                                Op_SBC(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xF6: //INC Zp, X
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffX();
                                break;
                            case 3: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 4: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 5:
                                PollInterrupts();
                                Op_INC(addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xF7: //ISC zp, X
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                                GetAddressZPOffX();
                                break;
                            case 3: // read from address
                                dl = Fetch(addressBus);
                                CPU_Read = false;
                                break;
                            case 4: //dummy write
                                Store(dl, addressBus);
                                break;
                            case 5:
                                PollInterrupts();
                                Op_INC(addressBus);
                                Op_SBC(dl);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xF8: //SED
                        PollInterrupts();
                        Fetch(addressBus); // dummy read
                        flag_Decimal = true;
                        CompleteOperation();
                        break;

                    case 0xF9: //SBC Abs Y

                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffY(true);
                                break;
                            case 4: // read from address
                                PollInterrupts();
                                Op_SBC(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xFA: //NOP ***
                        PollInterrupts();
                        Fetch(addressBus); // dummy read
                        CompleteOperation();
                        break;

                    case 0xFB: //ISC Abs Y ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressAbsOffY(false);
                                if (operationCycle == 4) { CPU_Read = false; }
                                break;
                            case 5:// dummy write
                                Store(dl, addressBus);
                                break;
                            case 6:// read from address
                                PollInterrupts();
                                Op_INC(addressBus);
                                Op_SBC(dl);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xFC: //NOP Absolute ,X ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffX(true);
                                break;
                            case 4: // read from address
                                PollInterrupts();
                                Fetch(addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xFD: //SBC Abs, X
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                                GetAddressAbsOffX(true);
                                break;
                            case 4: // read from address
                                PollInterrupts();
                                Op_SBC(Fetch(addressBus));
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xFE: //INC Abs, X
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressAbsOffX(false);
                                if (operationCycle == 4) { CPU_Read = false; }
                                break;
                            case 5:// dummy write
                                Store(dl, addressBus);
                                break;
                            case 6:// read from address
                                PollInterrupts();
                                Op_INC(addressBus);
                                CompleteOperation();
                                break;
                        }
                        break;

                    case 0xFF: //ISC Abs, X ***
                        switch (operationCycle)
                        {
                            case 1:
                            case 2:
                            case 3:
                            case 4:
                                GetAddressAbsOffX(false);
                                if (operationCycle == 4) { CPU_Read = false; }
                                break;
                            case 5:// dummy write
                                Store(dl, addressBus);
                                break;
                            case 6:// read from address
                                PollInterrupts();
                                Op_INC(addressBus);
                                Op_SBC(dl);
                                CompleteOperation();
                                break;
                        }
                        break;
                    // And that's all 256 instructions!
                }
                operationCycle++; // increment this for next CPU cycle.
            }
            if (oracle_probe) return; // NESRecomp
            EndOfCPUCycle();
            if (cyc_trace_enabled) cyc_trace_cycle_end(CPU_SYNC, dataBus); // NESRecomp
        }

void ResetReadPush()
{
            // the RESET instruction has unique behavior where it reads from the stack, and decrements the stack pointer.
            Fetch((ushort)(0x100 + stackPointer));
            stackPointer--;
        }

void Push(byte A)
{
            // Store to the stack, and decrement the stack pointer.
            Store(A, (ushort)(0x100 + stackPointer));
            stackPointer--;
        }


byte Observe(ushort Address)
{
            // Reading from anywhere goes through this function.
            if ((Address >= 0x8000))
            {
                // Reading from ROM.
                // Different mappers could rearrange the data from the ROM into different locations on the system bus.
                return MapperObserve(Address);
            }
            else if (Address < 0x2000)
            {
                // Reading from RAM.
                // Ram mirroring! Only addresses $0000 through $07FF exist in RAM, so ignore bits 11 and 12
                return RAM[Address & 0x7FF];
            }
            else if (Address >= 0x2000 && Address < 0x4000)
            {
                // PPU registers. most of these aren't meant to be read.
                Address = (ushort)(Address & 0x2007);
                switch (Address)
                {
                    case 0x2000:
                        // Write only. Return the PPU databus.
                        return PPUBus;
                    case 0x2001:
                        // Write only. Return the PPU databus.
                        return PPUBus;
                    case 0x2002:
                        // PPU Flags.
                        return (byte)((((PPUStatus_VBlank ? 0x80 : 0) | (PPUStatus_SpriteZeroHit ? 0x40 : 0) | (PPUStatus_SpriteOverflow ? 0x20 : 0)) & 0xE0) + (PPUBus & 0x1F));
                    case 0x2003:
                        // write only. Return the PPU databus.
                        return PPUBus;
                    case 0x2004:
                        // Read from OAM
                        return (byte)(ReadOAM());
                    case 0x2005:
                        // write only. Return the PPU databus.
                        return PPUBus;
                    case 0x2006:
                        // write only. Return the PPU databus.
                        return PPUBus;
                    case 0x2007:
                        // Reading from VRAM.
                        return ObservePPU(PPU_v);
                }

            }
            else if (Address >= 0x4000 && Address <= 0x401F) // observe the APU registers
            {
                //addressBus 
                byte Reg = (byte)(Address & 0x1F);
                if (Reg == 0x15)
                {

                    byte InternalBus = dataBus;

                    InternalBus &= 0x20;
                    InternalBus |= (byte)(APU_Status_DMCInterrupt ? 0x80 : 0);
                    InternalBus |= (byte)(APU_Status_FrameInterrupt ? 0x40 : 0);
                    InternalBus |= (byte)((APU_DMC_BytesRemaining != 0 && APU_Status_DelayedDMC) ? 0x10 : 0); // see footnote.
                    InternalBus |= (byte)((APU_LengthCounter_Noise != 0) ? 0x08 : 0);
                    InternalBus |= (byte)((APU_LengthCounter_Triangle != 0) ? 0x04 : 0);
                    InternalBus |= (byte)((APU_LengthCounter_Pulse2 != 0) ? 0x02 : 0);
                    InternalBus |= (byte)((APU_LengthCounter_Pulse1 != 0) ? 0x01 : 0);
                    return InternalBus; // reading from $4015 can not affect the databus
                }
                else if (Reg == 0x16 || Reg == 0x17)
                {
                    return (byte)((((Reg == 0x16) ? (ControllerShiftRegister1 & 0x80) : (ControllerShiftRegister2 & 0x80)) == 0 ? 0 : 1) | (dataBus & 0xE0));
                }
            }
            else
            {
                //mapper chip stuff, but also open bus!
                return MapperObserve(Address);
            }

            return dataBus;
        }

// NESRecomp: every CPU and DMA bus read goes through Fetch(), so it records the
// access for the observable trace.
static byte FetchBus(ushort Address);
byte Fetch(ushort Address)
{
            if (oracle_probe) {
                if (!oracle_probe_seen) oracle_probe_seen = true, oracle_probe_address = Address, oracle_probe_write = false;
                return dataBus;
            }
            OracleCheckAccess(Address);
            byte Value = FetchBus(Address);
            if (cyc_trace_enabled) cyc_trace_access(Address, Value, false);
            return Value;
}

static byte FetchBus(ushort Address)
{
            dataPinsAreNotFloating = false; // assume the data pins are floating by default.
            // Reading from anywhere goes through this function.
            if ((Address >= 0x8000))
            {
                // Reading from ROM.
                // Different mappers could rearrange the data from the ROM into different locations on the system bus.
                MapperFetchPRG(Address);
                dataPinsAreNotFloating = true;
            }
            else if (Address < 0x2000)
            {
                // Reading from RAM.
                // Ram mirroring! Only addresses $0000 through $07FF exist in RAM, so ignore bits 11 and 12
                dataBus = RAM[Address & 0x7FF];
                dataPinsAreNotFloating = true;
            }
            else if (Address >= 0x2000 && Address < 0x4000)
            {
                // PPU registers. most of these aren't meant to be read.
                Address = (ushort)(Address & 0x2007);
                switch (Address)
                {
                    case 0x2000:
                        // Write only. Return the PPU databus.
                        dataBus = PPUBus;

                        break;
                    case 0x2001:
                        // Write only. Return the PPU databus.
                        dataBus = PPUBus;

                        break;
                    case 0x2002:
                        // PPU Flags.

                        dataBus = (byte)((((PPUStatus_VBlank ? 0x80 : 0)))); // The vblank flag is read at the start of the read...
                        PPU_Read2002 = true;
                        EmulateUntilEndOfRead();
                        dataBus |= (byte)((((PPUStatus_SpriteZeroHit_Delayed ? 0x40 : 0) | (PPUStatus_SpriteOverflow_Delayed ? 0x20 : 0)) & 0xE0) + (PPUBus & 0x1F)); // ...while the sprite flags are read at the end.

                        PPUAddrLatch = false;
                        PPUBus = dataBus;
                        for (int i = 5; i < 8; i++) { PPUBusDecay[i] = PPUBusDecayConstant; }

                        break;
                    case 0x2003:
                        // write only. Return the PPU databus.
                        dataBus = PPUBus; break;
                    case 0x2004:
                        // Read from OAM
                        EmulateUntilEndOfRead();
                        dataBus = ReadOAM();

                        PPUBus = dataBus;
                        for (int i = 0; i < 8; i++) { PPUBusDecay[i] = PPUBusDecayConstant; }

                        break;
                    case 0x2005:
                        // write only. Return the PPU databus.
                        dataBus = PPUBus; break;
                    case 0x2006:
                        // write only. Return the PPU databus.
                        dataBus = PPUBus; break;
                    case 0x2007:
                        // Reading from VRAM.

                        if ((PPU_AddressBus & 0x3FFF) >= 0x3F00)
                        {
                            // read from palette RAM.
                            // Palette RAM only returns bits 0-5, so bits 6 and 7 are PPU open bus.
                            ThisDotReadFromPaletteRAM = true;
                            ushort PalRAMAddr = (ushort)(PPU_v & 0x3F1F);
                            if ((PalRAMAddr & 3) == 0)
                            {
                                PalRAMAddr &= 0x3F0F;
                            }

                            dataBus = (byte)(((PaletteRAM[PalRAMAddr & 0x1F] & (PPU_Mask_Greyscale ? 0x30 : 0x3F)) | (PPUBus & 0xC0)));
                        }
                        else
                        {
                            dataBus = PPU_ReadBuffer;
                        }
                        PPUBus = dataBus;
                        for (int i = 0; i < 8; i++) { PPUBusDecay[i] = PPUBusDecayConstant; }

                        EmulateUntilEndOfRead();
                        PPU_2007_Read_SR = true; // set the SR latch at the end of the CPU read. Here's where the clock alignment differences begin. :)
                        PPU_2007_Read = true; // Start the $2007 Read state machine.

                        break;
                }
                dataPinsAreNotFloating = true;

            }
            else
            {
                //mapper chip stuff, but also open bus!
                MapperFetchPRG(Address);
                // NESRecomp: a board with work RAM drives the bus at $6000-$7FFF.
                if (Cart.MapperChip.CPU_Drove) dataPinsAreNotFloating = true;
            }

            if (addressBus >= 0x4000 && addressBus <= 0x401F) // If APU registers are active, bus conflicts can occur. Or perhaps you are intentionally reading from the APU registers...
            {
                //addressBus 
                byte Reg = (byte)(Address & 0x1F);
                if (Reg == 0x15)
                {

                    internalBus &= 0x20;
                    internalBus |= (byte)(APU_Status_DMCInterrupt ? 0x80 : 0);
                    internalBus |= (byte)(APU_Status_FrameInterrupt ? 0x40 : 0);
                    internalBus |= (byte)((APU_DMC_BytesRemaining != 0 && APU_Status_DelayedDMC) ? 0x10 : 0); // see footnote.
                    internalBus |= (byte)((APU_LengthCounter_Noise != 0) ? 0x08 : 0);
                    internalBus |= (byte)((APU_LengthCounter_Triangle != 0) ? 0x04 : 0);
                    internalBus |= (byte)((APU_LengthCounter_Pulse2 != 0) ? 0x02 : 0);
                    internalBus |= (byte)((APU_LengthCounter_Pulse1 != 0) ? 0x01 : 0);

                    Clearing_APU_FrameInterrupt = true;


                    // footnote:
                    // Consider the following. LDA #0, STA $4015, LDA $4015.
                    // The APU_DMC_BytesRemaining byte isn't cleared until 3 or 4 cycles after writing 0 to $4015.
                    // However, reading from $4015 after the needs to immediately have bit 4 cleared.

                    return internalBus; // reading from $4015 can not affect the databus
                }
                else if (Reg == 0x16 || Reg == 0x17)
                {
                    byte ControllerRead = (byte)((((Reg == 0x16) ? (ControllerShiftRegister1 & 0x80) : (ControllerShiftRegister2 & 0x80)) == 0 ? 0 : 1) | (dataBus & 0xE0));

                    // controller ports
                    // grab 1 bit from the controller's shift register.
                    // also add the upper 3 bits of the databus.

                    if (Reg == 0x16)
                    {
                        // if there are 2 CPU cycles in a row that read from this address, the registers don't get shifted
                        Controller1ShiftCounter = 2; // The shift register isn't shifted until this is 0, decremented in every APU PUT cycle
                    }
                    else
                    {
                        // if there are 2 CPU cycles in a row that read from this address, the registers don't get shifted
                        Controller2ShiftCounter = 2; // The shift register isn't shifted until this is 0, decremented in every APU PUT cycle
                    }

                    APU_ControllerPortsStrobed = false; // This allows data to rapidly be streamed in through the A button if the controllers are read while strobed.
                    if (DoOAMDMA && dataPinsAreNotFloating) // If all the databus pins are floating, then the controller bits are visible. Otherwise... not so much.
                    {
                        return dataBus;
                    }
                    dataBus = ControllerRead;

                }
            }

            internalBus = dataBus;
            return dataBus;
        }

byte ObservePPU(ushort Address)
{
            if(Address >= 0x3F00)
            {
                if((Address & 0x3) == 0)
                {
                    return PaletteRAM[Address & 0x0F];
                }
                return PaletteRAM[Address & 0x1F];
            }
            return Cart.MapperChip.SnoopPPU(Address);
        }

byte MapperObserve(ushort Address)
{
            return Cart.MapperChip.SnoopCPU(Address);
        }

void MapperFetchPRG(ushort Address)
{
            Cart.MapperChip.Connector_SetUpCPUAddressPins(Address);
            Cart.MapperChip.Connector_SetUpCPUDataPins(dataBus); // So when we read from the data pins, if nothing changed, we get back this value.
            Cart.MapperChip.FetchCPU();
            dataBus = Cart.MapperChip.Connector_ReadCPUDataPins();
            return;
        }

byte ReadOAM()
{
            if ((PPU_Mask_ShowBackground || PPU_Mask_ShowSprites) && PPU_Scanline < 240)
            {
                return PPU_OAMLatch; // Remember, PPU_OAMLatch is updated on the first half of a PPU cycle. OAMBuffer is set up on the second half.
            }
            return OAM[PPUOAMAddress];
        }

// ORACLE FIX (DATA BUS). A write to $4003, $4007 or $400B leaves the whole
// written value on the CPU data bus, as every other write does: the CPU drives
// the bus for the write cycle, and nothing in the 2A03 changes what is on it.
// TriCNES computes the timer's high bits with `Input &= 0x7` before its
// `dataBus = Input`, leaving only bits 0-2 as open bus. The observable
// difference is the open-bus value of the next read of an undriven address.
// CYC_ORACLE_UNFIXED=BUS restores the original behavior.
static int oracle_bus_fix = -1;
static byte OracleTimerHighBits(byte &Input)
{
            if (oracle_bus_fix < 0) {
                const char *unfixed = getenv("CYC_ORACLE_UNFIXED");
                oracle_bus_fix = !(unfixed && strstr(unfixed, "BUS"));
            }
            return oracle_bus_fix ? (byte)(Input & 0x7) : (Input &= 0x7);
}

void Store(byte Input, ushort Address)
{
            if (oracle_probe) { // NESRecomp
                if (!oracle_probe_seen) oracle_probe_seen = true, oracle_probe_address = Address, oracle_probe_write = true;
                return;
            }
            OracleCheckAccess(Address); // NESRecomp
            if (cyc_trace_enabled) cyc_trace_access(Address, Input, true); // NESRecomp
            Cart.MapperChip.Connector_SetUpCPUAddressPins(Address);
            Cart.MapperChip.Connector_SetUpCPUDataPins(Input);
            // This is used whenever writing anywhere with the CPU
            if (Address < 0x2000)
            {
                //guaranteed to be RAM
                // Even still, the address pins on the 72 pin connector get updated.
                RAM[Address & 0x7FF] = Input;

            }
            else if (Address < 0x4000)
            {
                // $2000 through $3FFF writes to the PPU registers
                StorePPURegisters(Address, Input);
            }
            else if (Address >= 0x4000 && Address <= 0x4015)
            {
                // Writing to $4000 through $4015 are APU registers
                switch (Address)
                {
                    default:
                        APU_Register[Address & 0xFF] = Input; break;
                    case 0x4003:
                        if (APU_Status_Pulse1)
                        {
                            APU_LengthCounter_ReloadValuePulse1 = APU_LengthCounterLUT[Input >> 3];
                            APU_LengthCounter_ReloadPulse1 = true;
                        }
                        APU_ChannelTimer_Pulse1 |= (ushort)((OracleTimerHighBits(Input)) << 8);
                        break;
                    case 0x4007:
                        if (APU_Status_Pulse2)
                        {
                            APU_LengthCounter_ReloadValuePulse2 = APU_LengthCounterLUT[Input >> 3];
                            APU_LengthCounter_ReloadPulse2 = true;
                        }
                        APU_ChannelTimer_Pulse2 |= (ushort)((OracleTimerHighBits(Input)) << 8);
                        break;
                    case 0x400B:
                        if (APU_Status_Triangle)
                        {
                            APU_LengthCounter_ReloadValueTriangle = APU_LengthCounterLUT[Input >> 3];
                            APU_LengthCounter_ReloadTriangle = true;

                        }
                        APU_ChannelTimer_Triangle |= (ushort)((OracleTimerHighBits(Input)) << 8);
                        break;
                    case 0x400F:
                        if (APU_Status_Noise)
                        {
                            APU_LengthCounter_ReloadValueNoise = APU_LengthCounterLUT[Input >> 3];
                            APU_LengthCounter_ReloadNoise = true;
                        }
                        break;

                    case 0x4010:
                        APU_DMC_EnableIRQ = (Input & 0x80) != 0;
                        APU_DMC_Loop = (Input & 0x40) != 0;
                        APU_DMC_Rate = APU_DMCRateLUT[Input & 0xF];
                        if (!APU_DMC_EnableIRQ)
                        {
                            APU_Status_DMCInterrupt = false;
                            if (!OracleIRQFix()) IRQ_LevelDetector = false; // NESRecomp: ORACLE FIX (IRQ)
                        }
                        break;

                    case 0x4011:
                        APU_DMC_Output = (byte)(Input & 0x7F);

                        break;

                    case 0x4012:
                        APU_DMC_SampleAddress = (ushort)(0xC000 | (Input << 6));
                        break;

                    case 0x4013:
                        APU_DMC_SampleLength = (ushort)((Input << 4) | 1);
                        break;

                    case 0x4014:    //OAM DMA
                        DoOAMDMA = true;
                        FirstCycleOfOAMDMA = true;
                        DMAAddress = 0; // the starting address for the OAM DMC is always page aligned.
                        DMAPage = Input;
                        break;
                    case 0x4015:    //DMC DMA (and other audio channels)

                        APU_Status_DelayedDMC = (Input & 0x10) != 0;
                        APU_Status_Noise = (Input & 0x08) != 0;
                        APU_Status_Triangle = (Input & 0x04) != 0;
                        APU_Status_Pulse2 = (Input & 0x02) != 0;
                        APU_Status_Pulse1 = (Input & 0x01) != 0;

                        APU_DelayedDMC4015 = (byte)(APU_PutCycle ? 3 : 4); // Enable in 1 APU cycles, or 1.5 APU cycles. (it will be decremented later this cycle, so it's really like 2 : 3.

                        if (APU_Status_DelayedDMC && APU_DMC_BytesRemaining == 0)
                        {
                            // sets up the sample bytes_remaining and sample address.
                            StartDMCSample();
                            // However, the sample will only begin playing if the DMC is currently silent
                            if (APU_Silent)
                            {
                                DMCDMADelay = 2; // 2 APU cycles
                            }
                        }

                        if (!APU_Status_Noise) { APU_LengthCounter_Noise = 0; }
                        if (!APU_Status_Triangle) { APU_LengthCounter_Triangle = 0; }
                        if (!APU_Status_Pulse2) { APU_LengthCounter_Pulse2 = 0; }
                        if (!APU_Status_Pulse1) { APU_LengthCounter_Pulse1 = 0; }
                        APU_Status_DMCInterrupt = false;
                        if (!OracleIRQFix()) IRQ_LevelDetector = false; // NESRecomp: ORACLE FIX (IRQ)

                        // Explicit abort stuff.
                        if (!APU_Status_DelayedDMC && ((APU_ChannelTimer_DMC == 2 && !APU_PutCycle) || (APU_ChannelTimer_DMC == APU_DMC_Rate && APU_PutCycle))) // this will be the APU cycle that fires a DMC DMA
                        {
                            APU_DelayedDMC4015 = (byte)(APU_PutCycle ? 5 : 6); // Disable in 2.5 APU cycles, or 3 APU cycles.
                            // basically, if the DMA has already begun, don't abort it for *this* edge case.
                        }

                        // Implicit abort stuff.
                        if (APU_Status_DelayedDMC && ((APU_ChannelTimer_DMC == 10 && !APU_PutCycle) || (APU_ChannelTimer_DMC == 8 && APU_PutCycle)))
                        {
                            // okay, so the series of events is as follows:
                            // the Load DMA will occur
                            // regardless of the buffer being empty, there will be a 1-cycle DMA that gets aborted 2 cycles after the load DMA ends.
                            APU_SetImplicitAbortDMC4015 = true; // This will occur in 8 (or 9) cpu cycles
                        }

                        break;
                }

            }
            else if (Address == 0x4016)
            {
                if (TAS_ReadingTAS)
                {
                    APU_ControllerPortsStrobing = ((Input & 1) != 0);
                }
                APU_ControllerPortsStrobing = ((Input & 1) != 0);
                if (!APU_ControllerPortsStrobing)
                {
                    APU_ControllerPortsStrobed = false;
                }
            }
            else if (Address == 0x4017)
            {
                APU_FrameCounterMode = (Input & 0x80) != 0;
                APU_FrameCounterInhibitIRQ = (Input & 0x40) != 0;
                if (APU_FrameCounterMode)
                {
                    APU_HalfFrameClock = true;
                    APU_QuarterFrameClock = true;
                }
                if (APU_FrameCounterInhibitIRQ)
                {
                    APU_Status_FrameInterrupt = false;
                    IRQ_LevelDetector = false;
                }
                APU_FrameCounterReset = (byte)((APU_PutCycle ? 3 : 4));
            }
            else if (Address >= 0x4020)
            {
                // mapper chip specific stuff- but also open bus!
                Cart.MapperChip.StoreCPU(Address, Input);

                //MapperStore(Input, Address, Cart.MemoryMapper);

            }
            else
            {
                // open bus!
                // this doesn't write anywhere, but it still updates the databus!
            }

            dataBus = Input;
            internalBus = dataBus;
        }

void StorePPURegisters(ushort Addr, byte In)
{
            //EmulateNMasterClockCycles(1); // wait for PPUSEL to go high
            // Okay, I KNOW this shouldn't be commented out. TODO: figure out why the timing on this is off by one.

            ushort AddrT = (ushort)((Addr & 0x2007));
            switch (AddrT)
            {
                case 0x2000:
                    // writing here updates a large amount of PPU flags
                    PPUBus = In;
                    for (int i = 0; i < 8; i++) { PPUBusDecay[i] = PPUBusDecayConstant; }
                    if (PPU_RESET)
                    {
                        return;
                    }

                    
                    // now that PPUSEL is high, the value of the databus is written to the PPU register.
                    PPU_t = (ushort)((PPU_t & 0x73FF) | ((dataBus & 0x3) << 10)); // This early write to the t register is the cause of the scanline bug in SMB1.
                    PPU_EXT_Enable = (dataBus & 0x40) == 0x40;
                    // technically this changes PPUControl_NMIEnabled here too, but it's invisible as the NMI polling has already happened and it will be re-enabled before then.

                    EmulateNMasterClockCycles(2); // wait for the CPU databus to change. (that's right, it doesn't happen at the start of the write cycle!)
                    PPUControl_NMIEnabled = (In & 0x80) != 0;
                    PPUControlIncrementMode32 = (In & 0x4) != 0;
                    PPU_Spritex16 = (In & 0x20) != 0;
                    PPU_PatternSelect_Sprites = (In & 0x8) != 0;
                    PPU_PatternSelect_Background = (In & 0x10) != 0;
                    PPU_t = (ushort)((PPU_t & 0x73FF) | ((In & 0x3) << 10)); // change which nametable to render.
                    PPU_EXT_Enable = (In & 0x40) == 0x40;

                    break;

                case 0x2001:
                {
                    // writing here updates a large amount of PPU flags
                    // Is the background being drawn? Are sprites being drawn? Greyscale / color emphasis?
                    PPUBus = In;
                    for (int i = 0; i < 8; i++) { PPUBusDecay[i] = PPUBusDecayConstant; }
                    if (PPU_RESET)
                    {
                        return;
                    }
                    // Okay look, I *know* this hard-coded solution is jank and sloppy.
                    // It is temporary.
                    // I want to re-do the picture processing unit from the ground up, honestly.
                    // In the mean time, let's go back to the hard-coded delays. I got the correct results from the tests while doing this.
                    // And we can fix it later.
                    /*
                    EmulateNMasterClockCycles(1); // wait for PPUSEL to go high

                    PPU_Mask_EmphasizeBlue = (dataBus & 0x80) != 0;
                    PPU_Mask_Greyscale = (dataBus & 0x1) != 0;

                    EmulateNMasterClockCycles(2); // wait for the CPU databus to change. (that's right, it doesn't happen at the start of the write cycle!)

                    PPU_Mask_EmphasizeBlue = (In & 0x80) != 0;
                    PPU_Mask_EmphasizeGreen = (In & 0x40) != 0;
                    PPU_Mask_EmphasizeRed = (In & 0x20) != 0;
                    PPU_Mask_Greyscale = (In & 0x1) != 0;

                    EmulateNMasterClockCycles(4); // wait for PPUSEL to go low.

                    PPU_WasRenderingBefore2001Write = PPU_Mask_ShowBackground || PPU_Mask_ShowSprites;

                    PPU_Mask_8PxShowBackground = (In & 0x02) != 0;
                    PPU_Mask_8PxShowSprites = (In & 0x04) != 0;
                    PPU_Mask_ShowBackground = (In & 0x08) != 0;
                    PPU_Mask_ShowSprites = (In & 0x10) != 0;

                    PPU_Mask_ShowBackground_Instant = PPU_Mask_ShowBackground; // now that the PPU has updated, OAM evaluation will also recognize the change
                    PPU_Mask_ShowSprites_Instant = PPU_Mask_ShowSprites;
                    */


                    switch (PPUClock & 3) //depending on CPU/PPU alignment, the delay could be different.
                    {
                        case 0:
                            PPU_Update2001Delay = 2; PPU_Update2001EmphasisBitsDelay = 2; PPU_Update2001OAMCorruptionDelay = 2; break;
                        case 1:
                            PPU_Update2001Delay = 2; PPU_Update2001EmphasisBitsDelay = 1; PPU_Update2001OAMCorruptionDelay = 3; break; // PPU_Update2001EmphasisBitsDelay is actually 2, but different behavior than case 0 and 3.
                        case 2:
                            PPU_Update2001Delay = 3; PPU_Update2001EmphasisBitsDelay = 1; PPU_Update2001OAMCorruptionDelay = 3; break; // PPU_Update2001EmphasisBitsDelay is actually 2, but different behavior than case 0 and 3.
                        case 3:
                            PPU_Update2001Delay = 2; PPU_Update2001EmphasisBitsDelay = 2; PPU_Update2001OAMCorruptionDelay = 2; break;
                    }
                    PPU_WasRenderingBefore2001Write = PPU_Mask_ShowBackground || PPU_Mask_ShowSprites;
                    PPU_Mask_ShowBackground_Instant = PPU_Mask_ShowBackground; // now that the PPU has updated, OAM evaluation will also recognize the change
                    PPU_Mask_ShowSprites_Instant = PPU_Mask_ShowSprites;
                    // TODO: Remove this hard-coded junk:
                    bool temp_rendering = PPU_WasRenderingBefore2001Write;
                    bool temp_renderingFromInput = ((In & 0x08) != 0) || ((In & 0x10) != 0);
                    // disabling rendering can cause OAM corruption.
                    if (temp_rendering && !temp_renderingFromInput)
                    {
                        // we are disabling rendering inside vblank
                        if (PPU_Scanline < 241 || PPU_Scanline == 261)
                        {
                            PPU_OAMCorruptionRenderingDisabledOutOfVBlank_Instant = true; // used in the next cycle of sprite evaluation
                            if ((PPU_Dot & 7) < 2 && PPU_Dot <= 250)
                            {
                                // Palette corruption only occurs if rendering was disabled during the first 2 dots of a nametable fetch
                                // TODO: Fiskbit has enlightened me a bit on how this is actually working:
                                // The VRAM address muxer selects between the PAR, NT address, AT address, and v.
                                // v isn't an explicit input; it's actually the NT input when rendering is disabled.
                                // The AT input actually sources a lot of its bits from the NT input, and this leads to an unfortunate bug where turning rendering off during an AT fetch actually results in a brief period where you have an AT input that is sourcing from v instead of an NT address.
                                // And the address muxer ends up using this AT input briefly right after rendering is disabled.
                                // This is why you can get palette RAM corruption when turning rendering off during an AT fetch if v was pointing into $3C00-$3EFF, despite this clearly not being palette RAM. The AT input that is being used actually points into palette RAM because those 2 bits are forced to 1.
                                if ((PPU_v & 0x3FFF) >= 0x3C00) // palette corruption only appears to occur when disabling rendering if the VRAM address is currently greater than 3C00
                                {
                                    PPU_PaletteCorruptionRenderingDisabledOutOfVBlank = true; // used in the color calculation for the next dot being drawn
                                }
                            }
                        }
                    }
                    else if (!temp_rendering && temp_renderingFromInput)
                    {
                        if (PPU_Scanline < 241 || PPU_Scanline == 261)
                        {
                            // if re-enabling rendering outside vblank
                            if (PPU_PendingOAMCorruption)
                            {
                                // If OAM corruption is going to occur
                                if (PPUClock == 1 || PPUClock == 2)
                                {
                                    // if on clock alignment 1 or 2, it doesn't happen!
                                    PPU_OAMCorruptionRenderingEnabledOutOfVBlank = true;
                                }
                            }
                        }
                    }

                    // This is temp. I know it's wrong (we're not even waiting for PPUSEL here.) but I'll fix it after redoing the entire ppu or something.
                    if (PPU_Update2001EmphasisBitsDelay == 2)
                    {
                        PPU_Mask_Greyscale = (dataBus & 0x01) != 0;
                        PPU_Mask_EmphasizeBlue = (dataBus & 0x80) != 0;
                    }
                    else
                    {
                        PPU_Update2001EmphasisBitsDelay++; // it's always 2.
                    }
                    PPU_Mask_EmphasizeRed = (In & 0x20) != 0;
                    PPU_Mask_EmphasizeGreen = (In & 0x40) != 0;

                    PPU_Update2001Value = In;

                    break;
                }

                case 0x2002: // this value is Read only.
                    PPUBus = In;
                    for (int i = 0; i < 8; i++) { PPUBusDecay[i] = PPUBusDecayConstant; }
                    break;

                case 0x2003:
                    // writing here updates the OAM address
                    PPUBus = In;
                    for (int i = 0; i < 8; i++) { PPUBusDecay[i] = PPUBusDecayConstant; }
                    PPUOAMAddress = PPUBus;
                    break;

                case 0x2004:
                    // writing here updates the OAM byte at the current OAM address
                    PPUBus = In;
                    for (int i = 0; i < 8; i++) { PPUBusDecay[i] = PPUBusDecayConstant; }
                    if (((PPU_Scanline >= 240 && PPU_Scanline < 261) && (PPU_Mask_ShowBackground || PPU_Mask_ShowSprites)) || (!PPU_Mask_ShowBackground && !PPU_Mask_ShowSprites))
                    {
                        if ((PPUOAMAddress & 3) == 2)
                        {
                            In &= 0xE3;
                        }
                        OAM[PPUOAMAddress] = In;
                        PPUOAMAddress++;
                    }
                    else
                    {
                        PPUOAMAddress += 4;
                        PPUOAMAddress &= 0xFC;

                    }
                    break;

                case 0x2005:
                    // writing here updates the X and Y scroll
                    PPUBus = In;
                    for (int i = 0; i < 8; i++) { PPUBusDecay[i] = PPUBusDecayConstant; }
                    if (PPU_RESET)
                    {
                        return;
                    }
                    switch (PPUClock & 3) //depending on CPU/PPU alignment, the delay could be different.
                    {
                        case 0: PPU_Update2005Delay = 1; break;
                        case 1: PPU_Update2005Delay = 1; break;
                        case 2: PPU_Update2005Delay = 2; break;
                        case 3: PPU_Update2005Delay = 1; break;
                    }
                    PPU_Update2005Value = In;
                    // There's a slight delay before the PPU updates the scroll with the correct values.
                    // In the meantime, it uses the value from the databus.
                    if (!PPUAddrLatch)
                    {
                        PPU_FineXScroll = (byte)(dataBus & 7);
                        PPU_t = (ushort)((PPU_t & 0x7FE0) | (dataBus >> 3));
                    }
                    else
                    {
                        PPU_t = (ushort)((PPU_t & 0xC1F) | (((dataBus & 0xF8) << 2) | ((dataBus & 7) << 12)));
                    }
                    break;

                case 0x2006:
                    // writing here updates the PPU's read/write address.
                    PPUBus = In;
                    for (int i = 0; i < 8; i++) { PPUBusDecay[i] = PPUBusDecayConstant; }
                    if (PPU_RESET)
                    {
                        return;
                    }

                    if (!PPUAddrLatch)
                    {
                        PPU_t = (ushort)((PPU_t & 0xFF) | ((In & 0x3F) << 8));

                    }
                    else
                    {
                        PPU_t = (ushort)((PPU_t & 0x7F00) | (In));
                        PPU_Update2006Value = PPU_t;
                        PPU_Update2006Value_Temp = PPU_v;
                        switch (PPUClock & 3) //depending on CPU/PPU alignment, the delay could be different.
                        {
                            case 0: PPU_Update2006Delay = 4; break;
                            case 1: PPU_Update2006Delay = 4; break;
                            case 2: PPU_Update2006Delay = 5; break;
                            case 3: PPU_Update2006Delay = 4; break;
                        }
                    }
                    PPUAddrLatch = !PPUAddrLatch;

                    break;

                case 0x2007:
                    // writing here updates the byte at the current read/write address
                    PPUBus = In;
                    PPU_2007_WriteData = PPUBus;
                    for (int i = 0; i < 8; i++) { PPUBusDecay[i] = PPUBusDecayConstant; }
                    EmulateNMasterClockCycles(7); // wait for PPUSEL to go low
                    PPU_2007_Write = true;
                    PPU_2007_Write_SR = true; // set the SR latch at the end of the CPU write. Here's where the clock alignment differences begin. :)
                    break;
                // and that's it for the ppu registers!

                default: break; //should never happen
            }


        }

void StartDMCSample()
{
            // This runs when writing to $4015, or if a DPCM sample is looping and needs to restart.
            APU_DMC_AddressCounter = APU_DMC_SampleAddress;
            APU_DMC_BytesRemaining = APU_DMC_SampleLength;
        }

void GetImmediate()
{
            // Fetch the value at the program counter, store it in the DataLatch, and increment the Program Counter.
            dl = Fetch(programCounter);
            programCounter++;
            addressBus = programCounter;
        }

void GetAddressAbsolute()
{
            // Fetch the value at the PC, and write to either the High byte or Low byte of the 16 bit address bus. Also increment the Program Counter.
            if (operationCycle == 1)
            {
                // fetch address low
                dl = Fetch(programCounter);
            }
            else
            {
                // fetch address high
                addressBus = (ushort)(dl | (Fetch(programCounter) << 8));
            }
            programCounter++;
        }

void GetAddressZeroPage()
{
            // Fetch the value at the PC, and this 8 bit value replaces the contents of the 16 bit address bus.
            addressBus = Fetch(programCounter);
            programCounter++;
        }

void GetAddressIndOffX()
{
            // Fetch the value from the PC, then using that value as an 8-bit address on the zero page, add the X register, then set the High byte and Low byte of the Address Bus from there.
            switch (operationCycle)
            {
                case 1: // fetch pointer address
                    addressBus = Fetch(programCounter);
                    programCounter++;
                    break;
                case 2: // Add X
                    // dummy read
                    Fetch(addressBus);
                    addressBus = (byte)(addressBus + X);
                    break;
                case 3: // fetch address low
                    dl = Fetch((byte)(addressBus));
                    break;
                case 4: // fetch address high
                    addressBus = (ushort)(dl | (Fetch((byte)(addressBus + 1)) << 8));
                    break;
            }
        }

void GetAddressIndOffY(bool TakeExtraCycleOnlyIfPageBoundaryCrossed)
{
            // Some instructions will always take 4 cycles to determine the address, and others will normally take 3, but take the extra cycle if a page boundary was crossed.

            // either way, the general gist of this function is:
            // Fetch the value from the PC. use that 8 bit location on the zero page to fetch the High and Low byte of the new Address Bus location, then add Y to that.
            if (TakeExtraCycleOnlyIfPageBoundaryCrossed)
            {
                switch (operationCycle)
                {
                    case 1: // fetch pointer address
                        addressBus = Fetch(programCounter);
                        programCounter++;
                        break;
                    case 2: // fetch address low
                        dl = Fetch((byte)(addressBus));
                        break;
                    case 3: // fetch address high, add Y to low byte
                        addressBus = (ushort)(dl | (Fetch((byte)(addressBus + 1)) << 8));
                        temporaryAddress = addressBus;
                        H = (byte)(addressBus >> 8);
                        if (((temporaryAddress + Y) & 0xFF00) == (temporaryAddress & 0xFF00))
                        {
                            operationCycle++; //skip next cycle
                        }
                        addressBus = (ushort)((addressBus & 0xFF00) | ((addressBus + Y) & 0xFF));
                        break;
                    case 4: // increment high byte
                        dl = Fetch(addressBus); // dummy read
                        H = (byte)(addressBus >> 8);
                        H++; // This is incremented.
                        addressBus += 0x100;
                        break;
                }
            }
            else
            {
                switch (operationCycle)
                {
                    case 1: // fetch pointer address
                        addressBus = Fetch(programCounter);
                        programCounter++;
                        break;
                    case 2: // fetch address low
                        dl = Fetch((byte)(addressBus));
                        break;
                    case 3: // fetch address high, add Y to low byte
                        addressBus = (ushort)(dl | (Fetch((byte)(addressBus + 1)) << 8));
                        temporaryAddress = addressBus;
                        addressBus = (ushort)((addressBus & 0xFF00) | ((addressBus + Y) & 0xFF));
                        break;
                    case 4: // increment high byte
                        dl = Fetch(addressBus); // dummy read
                        H = (byte)(addressBus >> 8);
                        H++; // This is incremented.
                        if (((temporaryAddress + Y) & 0xFF00) != (temporaryAddress & 0xFF00))
                        {
                            addressBus += 0x100; // really, this would just replace the high byte with H, but this is less computationally expensive
                        }
                        break;
                }
            }

        }

void GetAddressZPOffX()
{
            // Fetch the value from the PC, then add X to that.
            if (operationCycle == 1)
            {
                // fetch address
                addressBus = Fetch(programCounter);
                programCounter++;
            }
            else
            {
                // dummy read, and add X
                dl = Fetch(addressBus);
                addressBus = (byte)(addressBus + X);
            }
        }

void GetAddressZPOffY()
{
            // Fetch the value from the PC, then add Y to that.
            if (operationCycle == 1)
            {
                // fetch address
                addressBus = Fetch(programCounter);
                programCounter++;
            }
            else
            {
                // dummy read, and add Y
                dl = Fetch(addressBus);
                addressBus = (byte)(addressBus + Y);
            }
        }

void GetAddressAbsOffX(bool TakeExtraCycleIfPageBoundaryCrossed)
{
            // Some instructions will always take 4 cycles to determine the address, and others will normally take 3, but take the extra cycle if a page boundary was crossed.

            // Fetch the High and Low byte values from the byte at the PC, then add X.
            if (TakeExtraCycleIfPageBoundaryCrossed)
            {
                switch (operationCycle)
                {
                    case 1: // fetch address low
                        dl = Fetch(programCounter);
                        programCounter++;

                        break;
                    case 2: // fetch address high, add Y to low byte
                        addressBus = (ushort)(dl | Fetch(programCounter) << 8);
                        temporaryAddress = addressBus;
                        H = (byte)(addressBus >> 8);

                        if (((temporaryAddress + X) & 0xFF00) == (temporaryAddress & 0xFF00))
                        {
                            operationCycle++; //skip next cycle
                            FixHighByte = false;
                        }
                        else
                        {
                            FixHighByte = true;
                        }

                        addressBus = (ushort)((addressBus & 0xFF00) | ((addressBus + X) & 0xFF));
                        programCounter++;

                        break;
                    case 3: // increment high byte
                        dl = Fetch(addressBus);
                        H = (byte)(addressBus >> 8);
                        H++;
                        if (FixHighByte)
                        {
                            addressBus += 0x100;
                        }
                        break;
                    case 4: // dummy read
                        dl = Fetch(addressBus); // read into pd
                        break;
                }
            }
            else
            {
                switch (operationCycle)
                {
                    case 1: // fetch address low
                        dl = Fetch(programCounter);
                        programCounter++;

                        break;
                    case 2: // fetch address high, add Y to low byte
                        addressBus = (ushort)(dl | Fetch(programCounter) << 8);
                        temporaryAddress = addressBus;
                        addressBus = (ushort)((addressBus & 0xFF00) | ((addressBus + X) & 0xFF));
                        programCounter++;

                        break;
                    case 3: // fix high byte if applicable
                        dl = Fetch(addressBus); // read into pd
                        H = (byte)(addressBus >> 8);
                        H++;
                        if (((temporaryAddress + X) & 0xFF00) != (temporaryAddress & 0xFF00))
                        {
                            addressBus += 0x100;
                        }
                        break;
                    case 4: // dummy read
                        dl = Fetch(addressBus); // read into pd
                        break;
                }
            }
        }

void GetAddressAbsOffY(bool TakeExtraCycleIfPageBoundaryCrossed)
{
            // Some instructions will always take 4 cycles to determine the address, and others will normally take 3, but take the extra cycle if a page boundary was crossed.

            // Fetch the High and Low byte values from the byte at the PC, then add Y.
            if (TakeExtraCycleIfPageBoundaryCrossed)
            {
                switch (operationCycle)
                {
                    case 1: // fetch address low
                        dl = Fetch(programCounter);
                        programCounter++;

                        break;
                    case 2: // fetch address high, add Y to low byte
                        addressBus = (ushort)(dl | Fetch(programCounter) << 8);
                        temporaryAddress = addressBus;
                        H = (byte)(addressBus >> 8);

                        if (((temporaryAddress + Y) & 0xFF00) == (temporaryAddress & 0xFF00))
                        {
                            operationCycle++; //skip next cycle
                            FixHighByte = false;
                        }
                        else
                        {
                            FixHighByte = true;
                        }

                        addressBus = (ushort)((addressBus & 0xFF00) | ((addressBus + Y) & 0xFF));
                        programCounter++;

                        break;
                    case 3: // increment high byte
                        dl = Fetch(addressBus);
                        H = (byte)(addressBus >> 8);
                        H++;
                        if (FixHighByte)
                        {
                            addressBus += 0x100;
                        }
                        break;
                    case 4: // dummy read
                        dl = Fetch(addressBus); // read into databus
                        break;
                }
            }
            else
            {
                switch (operationCycle)
                {
                    case 1: // fetch address low
                        dl = Fetch(programCounter);
                        programCounter++;

                        break;
                    case 2: // fetch address high, add Y to low byte
                        addressBus = (ushort)(dl | Fetch(programCounter) << 8);
                        temporaryAddress = addressBus;
                        addressBus = (ushort)((addressBus & 0xFF00) | ((addressBus + Y) & 0xFF));
                        programCounter++;

                        break;
                    case 3: // fix high byte if applicable
                        dl = Fetch(addressBus); // read into pd
                        H = (byte)(addressBus >> 8);
                        H++;
                        if (((temporaryAddress + Y) & 0xFF00) != (temporaryAddress & 0xFF00))
                        {
                            addressBus += 0x100;
                        }
                        break;
                    case 4: // dummy read
                        dl = Fetch(addressBus); // read into pd
                        break;
                }
            }
        }

void Op_ORA(byte Input)
{
            // Bitwise OR A with some value
            A |= Input;
            flag_Negative = A >= 0x80; // if bit 7 of the result is set
            flag_Zero = A == 0x00;     // if all bits are cleared
        }

void Op_ASL(byte Input, ushort Address)
{
            // Arithmetic shift left.
            flag_Carry = Input >= 0x80;    // If bit 7 was set before the shift
            Input <<= 1;
            Store(Input, Address);         // store the result at the target address
            flag_Negative = Input >= 0x80; // if bit 7 of the result is set
            flag_Zero = Input == 0x00;     // if all bits are cleared
        }

void Op_ASL_A()
{
            // Arithmetic shift left the Accumulator
            flag_Carry = A >= 0x80;    // If bit 7 was set before the shift
            A <<= 1;
            flag_Negative = A >= 0x80; // if bit 7 of the result is set
            flag_Zero = A == 0x00;     // if all bits are cleared
        }

void Op_SLO(byte Input, ushort Address)
{
            // Undocumented Opcode: equivalent to ASL + ORA
            Op_ASL(Input, Address);
            Op_ORA(dataBus);
        }

void Op_AND(byte Input)
{
            // Bitwise AND with A
            A &= Input;
            flag_Negative = A >= 0x80; // if bit 7 of the result is set
            flag_Zero = A == 0x00;     // if all bits are cleared
        }

void Op_ROL(byte Input, ushort Address)
{
            // Rotate Left
            bool Futureflag_Carry = Input >= 0x80;
            Input <<= 1;
            if (flag_Carry)
            {
                Input |= 1; // Put the old carry flag value into bit 0
            }
            Store(Input, Address);         // store the result at the target address
            flag_Carry = Futureflag_Carry; // if bit 7 of the initial value was set
            flag_Negative = Input >= 0x80; // if bit 7 of the result is set
            flag_Zero = Input == 0x00;     // if all bits are cleared
        }

void Op_ROL_A()
{
            // Rotate Left the Accumulator
            bool Futureflag_Carry = A >= 0x80;
            A <<= 1;
            if (flag_Carry)
            {
                A |= 1; // Put the old carry flag value into bit 0
            }
            flag_Carry = Futureflag_Carry; // if bit 7 of the initial value was set
            flag_Negative = A >= 0x80;     // if bit 7 of the result is set
            flag_Zero = A == 0x00;         // if all bits are cleared
        }

void Op_RLA(byte Input, ushort Address)
{
            // Undocumented Opcode: equivalent to ROL + AND
            Op_ROL(Input, Address);
            Op_AND(dataBus);
        }

void Op_EOR(byte Input)
{
            // Bitwise Exclusive OR A
            A ^= Input;
            flag_Negative = A >= 0x80; // if bit 7 of the result is set
            flag_Zero = A == 0x00;     // if all bits are cleared
        }

void Op_LSR(byte Input, ushort Address)
{
            // Logical Shift Right
            flag_Carry = (Input & 1) == 1; // If bit 0 of the initial value is set
            Input >>= 1;
            Store(Input, Address);         // store the result at the target address
            flag_Negative = Input >= 0x80; // if bit 7 of the result is set
            flag_Zero = Input == 0x00;     // if all bits are cleared
        }

void Op_LSR_A()
{
            // Logical Shift Right the Accumulator
            // NESRecomp: written with a local. MSVC 19.44 /O2 compiles the upstream form
            //   flag_Carry = (A & 1) == 1; A >>= 1; flag_Negative = A >= 0x80; flag_Zero = A == 0x00;
            // in an out-of-line function as "flag_Negative = 1" and drops the flag_Zero store.
            // Inlined copies inside _6502 happen to be correct, so only recompiled code (which
            // calls this function) saw it; found by cyc_host --hash-out differential tracing.
            byte Result = (byte)(A >> 1);
            flag_Carry = (A & 1) == 1; // If bit 0 of the initial value is set
            A = Result;
            flag_Negative = (Result & 0x80) != 0; // if bit 7 of the result is set
            flag_Zero = Result == 0x00;           // if all bits are cleared
        }

void Op_SRE(byte Input, ushort Address)
{
            // Undocumented Opcode: equivalent to LSR + EOR
            Op_LSR(Input, Address);
            Op_EOR(dataBus);
        }

void Op_ADC(byte Input)
{
            // Add with Carry
            int Intput = Input + A + (flag_Carry ? 1 : 0);
            flag_Overflow = (~(A ^ Input) & (A ^ Intput) & 0x80) != 0;
            flag_Carry = Intput > 0xFF;
            A = (byte)Intput;
            flag_Negative = A >= 0x80; // if bit 7 of the result is set
            flag_Zero = A == 0x00;     // if all bits are cleared
        }

void Op_ROR(byte Input, ushort Address)
{
            // Rotate Right
            bool FutureFlag_Carry = (Input & 1) == 1; // if bit 0 was set before the shift
            Input >>= 1;
            if (flag_Carry)
            {
                Input |= 0x80;  // put the old carry flag into bit 7
            }
            Store(Input, Address);
            flag_Carry = FutureFlag_Carry; // if bit 0 was set before the shift
            flag_Negative = Input >= 0x80; // if bit 7 of the result is set
            flag_Zero = Input == 0x00;     // if all bits are cleared
        }

void Op_ROR_A()
{
            bool FutureFlag_Carry = (A & 1) == 1;
            A >>= 1;
            if (flag_Carry)
            {
                A |= 0x80;  // put the old carry flag into bit 7
            }
            flag_Carry = FutureFlag_Carry; // if bit 0 was set before the shift
            flag_Negative = A >= 0x80;     // if bit 7 of the result is set
            flag_Zero = A == 0x00;         // if all bits are cleared
        }

void Op_RRA(byte Input, ushort Address)
{
            // Undocumented Opcode: equivalent to ROR + ADC
            Op_ROR(Input, Address);
            Op_ADC(dataBus);
        }

void Op_CMP(byte Input)
{
            // Compare A
            flag_Zero = A == Input; // if A is equal to the value being compared
            flag_Carry = A >= Input;// if A is greater than the value being compared
            flag_Negative = ((byte)(A - Input) >= 0x80); // if A - the value being compared would leave bit 7 set
        }

void Op_CPY(byte Input)
{
            // Compare Y
            flag_Zero = Y == Input; // if Y is equal to the value being compared
            flag_Carry = Y >= Input;// if Y is greater than the value being compared
            flag_Negative = ((byte)(Y - Input) >= 0x80); // if Y - the value being compared would leave bit 7 set
        }

void Op_CPX(byte Input)
{
            // Compare X
            flag_Zero = X == Input; // if X is equal to the value being compared
            flag_Carry = X >= Input;// if X is greater than the value being compared
            flag_Negative = ((byte)(X - Input) >= 0x80); // if X - the value being compared would leave bit 7 set
        }

void Op_SBC(byte Input)
{
            // Subtract with Carry
            int Intput = A - Input;
            if (!flag_Carry)
            {
                Intput -= 1;
            }
            flag_Overflow = ((A ^ Input) & (A ^ Intput) & 0x80) != 0;
            flag_Carry = Intput >= 0;
            A = (byte)Intput;
            flag_Negative = A >= 0x80; // if bit 7 of the result is set
            flag_Zero = A == 0x00;     // if all bits are cleared
        }

void Op_INC(ushort Address)
{
            // Increment
            dl++;   // The value read is currently stored in the PreDecode register
            flag_Zero = dl == 0;        // if all bits are cleared
            flag_Negative = dl >= 0x80; // if bit 7 of the result is set
            Store(dl, Address);

        }

void Op_DEC(ushort Address)
{
            // Decrement
            dl--;  // The value read is currently stored in the PreDecode register
            flag_Zero = dl == 0;        // if all bits are cleared
            flag_Negative = dl >= 0x80; // if bit 7 of the result is set
            Store(dl, Address);

        }


// ---------------------------------------------------------------------------
// NESRecomp host interface (not part of TriCNES)
// ---------------------------------------------------------------------------


// ---- Oracle: TriCNES's own CPU and per-tick loop ----

// Runs until the first instruction boundary at or after VBlank, the same
// frame rule as NESRecomp's scheduler (cyc_run.c).
void cyc_oracle_run_frame(void)
{
    static bool at_boundary;
    hw_frame_done = false;
    for (;;) {
        if (at_boundary && hw_frame_done) return;
        bool cpu_tick = CPUClock == 12;
        bool dma = cpu_tick && OracleDMATakesCycle();
        _EmulatorCore();
        at_boundary = cpu_tick && !dma && CPU_SYNC;
    }
}

void cyc_cpu_state(CycCpuState *out)
{
    out->pc = programCounter;
    out->a = A;
    out->x = X;
    out->y = Y;
    out->s = stackPointer;
    out->p = (byte)((flag_Negative ? 0x80 : 0) | (flag_Overflow ? 0x40 : 0) | (flag_Decimal ? 0x08 : 0) |
                    (flag_Interrupt ? 0x04 : 0) | (flag_Zero ? 0x02 : 0) | (flag_Carry ? 0x01 : 0));
    out->do_nmi = DoNMI;
    out->do_irq = DoIRQ;
}


// NESRecomp: the bank tables index with a power-of-two mask, so a ROM whose
// size is not a power of two is padded and the tail reads as zero, the same
// way hw_machine.c loads it.
static byte *tric_alloc_padded(const byte *src, size_t len, int *out_alloc) {
    uint32_t alloc = 1;
    while (alloc < len) alloc <<= 1;
    byte *p = (byte *)calloc(1, alloc);
    if (src) memcpy(p, src, len);
    *out_alloc = (int)alloc;
    return p;
}

// CYC_MMC3_TRACE=1: the same line hw_mapper.c prints, so the two counters can
// be diffed directly on the shared trace cycle (see cyc_trace.h). Needs a
// --hash-out or --trace-out run, which is what advances that clock.
void TricMmc3TraceClock(byte counter, bool out, ushort vbus) {
    static int on = -1;
    if (on < 0) on = getenv("CYC_MMC3_TRACE") != NULL;
    if (!on) return;
    fprintf(stderr, "MMC3 clk tc=%u sl=%u dot=%u ctr=%02X out=%u vbus=%04X\n",
            cyc_trace_cycle, PPU_Scanline, PPU_Dot, counter, out, vbus);
}

bool cyc_load_ines(const uint8_t *image, size_t size) {
    if (size < 16 || memcmp(image, "NES\x1A", 4) != 0) return false;
    int mapper = (image[6] >> 4) | (image[7] & 0xF0);
    if ((image[7] & 0x0C) == 0x08) {
        mapper |= (image[8] & 15) << 8;
        if (mapper != 0 && mapper != 1 && mapper != 2 && mapper != 3 &&
            mapper != 4 && mapper != 7 && mapper != 66) return false;
    }
    switch (mapper) {   // the set hw_mapper.c implements
    case 140: break;
    case 113: break;
    case 94: break;
    case 87: break;
    case 79: break;
    case 76: break;
    case 206: break;
    case 75: break;
    case 71: break;
    case 34: break;
    case 13: break;
    case 11: break;
    case 0: case 1: case 2: case 3: case 4: case 7: case 66: break;
    default: return false;
    }
    if (image[6] & 0x08) return false;   // four-screen boards carry their own RAM
    size_t prg_len = (size_t)image[4] * 0x4000;
    size_t chr_len = (size_t)image[5] * 0x2000;
    size_t offset = 16 + ((image[6] & 0x04) ? 512 : 0);
    if (prg_len == 0 || offset + prg_len + chr_len > size) return false;
    free(Cart.PRGROM);
    free(Cart.CHRROM);
    int prg_alloc, chr_alloc;
    Cart.PRGROM = tric_alloc_padded(image + offset, prg_len, &prg_alloc);
    Cart.PRGROM_Length = (int)prg_len;
    Cart.PRGSlots = prg_alloc / 0x2000 ? prg_alloc / 0x2000 : 1;
    Cart.UsingCHRRAM = chr_len == 0;
    Cart.CHRROM = tric_alloc_padded(chr_len ? image + offset + prg_len : NULL, chr_len ? chr_len : (mapper == 13 ? 0x4000 : 0x2000),
                                    &chr_alloc);
    Cart.CHRROM_Length = (int)(chr_len ? chr_len : (mapper == 13 ? 0x4000 : 0x2000));
    Cart.CHRPages = chr_alloc / 0x400 ? chr_alloc / 0x400 : 1;
    Cart.Mapper = mapper;
    Cart.NametableHorizontalMirroring = (image[6] & 1) == 0;
    Cart.AlternativeNametableArrangement = (image[6] & 8) != 0;
    Cart.MapperChip.Reset();
    return true;
}

void cyc_power_on(uint8_t ppu_alignment) {
    Emulator_Init();
    Cart.MapperChip.Reset();
    PPUClock = ppu_alignment & 3;
    CPUClock = 0;
}

uint32_t cyc_prg_hash(void) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < Cart.PRGROM_Length; i++) h = (h ^ Cart.PRGROM[i]) * 16777619u;
    return h;
}

// ---- state hash ----

#define mix cyc_trace_mix

// Hardware fields in cyc_hw_state_hash(), by name for cyc_hw_state_dump().
#define F(v) { #v, &(v), sizeof(v) }
static const struct { const char *name; const void *p; size_t n; } cyc_hw_fields[] = {
        F(PPUClock), F(CPUClock), F(CICClock), F(ResetMode), F(ResetModeCounter), F(APUAlignment),
        F(APU_PutCycle), F(OAM2), F(SecondaryOAMSize), F(OAM2Address), F(SpriteEvaluationTick),
        F(OAMAddressOverflowedDuringSpriteEvaluation), F(totalCycles), F(dataBus), F(internalBus), F(PPU_RESET),
        F(DoOAMDMA), F(FirstCycleOfOAMDMA), F(DoDMCDMA), F(DMCDMADelay), F(CannotRunDMCDMARightNow), F(DMAPage),
        F(DMAAddress), F(APU_ControllerPortsStrobing), F(APU_ControllerPortsStrobed),
        F(ControllerShiftRegister1), F(ControllerShiftRegister2), F(Controller1ShiftCounter),
        F(Controller2ShiftCounter), F(LagFrame), F(APU_Status_DMCInterrupt), F(APU_Status_FrameInterrupt),
        F(APU_Status_DMC), F(APU_Status_DelayedDMC), F(APU_Status_Noise), F(APU_Status_Triangle),
        F(APU_Status_Pulse2), F(APU_Status_Pulse1), F(Clearing_APU_FrameInterrupt), F(APU_DelayedDMC4015),
        F(APU_ImplicitAbortDMC4015), F(APU_SetImplicitAbortDMC4015), F(APU_Register), F(APU_FrameCounterMode),
        F(APU_FrameCounterInhibitIRQ), F(APU_FrameCounterReset), F(APU_Framecounter), F(APU_QuarterFrameClock),
        F(APU_HalfFrameClock), F(APU_Envelope_StartFlag), F(APU_Envelope_DividerClock),
        F(APU_Envelope_DecayLevel), F(APU_LengthCounter_Pulse1), F(APU_LengthCounter_Pulse2),
        F(APU_LengthCounter_Triangle), F(APU_LengthCounter_Noise), F(APU_LengthCounter_HaltPulse1),
        F(APU_LengthCounter_HaltPulse2), F(APU_LengthCounter_HaltTriangle), F(APU_LengthCounter_HaltNoise),
        F(APU_LengthCounter_ReloadPulse1), F(APU_LengthCounter_ReloadPulse2),
        F(APU_LengthCounter_ReloadTriangle), F(APU_LengthCounter_ReloadNoise),
        F(APU_LengthCounter_ReloadValuePulse1), F(APU_LengthCounter_ReloadValuePulse2),
        F(APU_LengthCounter_ReloadValueTriangle), F(APU_LengthCounter_ReloadValueNoise),
        F(APU_ChannelTimer_Pulse1), F(APU_ChannelTimer_Pulse2), F(APU_ChannelTimer_Triangle),
        F(APU_ChannelTimer_Noise), F(APU_ChannelTimer_DMC), F(APU_DMC_EnableIRQ), F(APU_DMC_Loop),
        F(APU_DMC_Rate), F(APU_DMC_Output), F(APU_DMC_SampleAddress), F(APU_DMC_SampleLength),
        F(APU_DMC_BytesRemaining), F(APU_DMC_Buffer), F(APU_DMC_AddressCounter), F(APU_DMC_Shifter),
        F(APU_DMC_ShifterBitsRemaining), F(DPCM_Up), F(APU_Silent), F(PPUBus), F(PPUBusDecay), F(PPUOAMAddress),
        F(PPUStatus_VBlank), F(PPUStatus_PendingSpriteZeroHit), F(PPUStatus_PendingSpriteZeroHit2),
        F(PPUStatus_SpriteZeroHit), F(PPUStatus_SpriteZeroHit_Delayed), F(PPUStatus_SpriteOverflow),
        F(PPUStatus_SpriteOverflow_Delayed), F(PPU_VSET), F(PPU_VSET_Latch1), F(PPU_VSET_Latch2),
        F(PPU_Read2002), F(PPU_Spritex16), F(PPU_Scanline), F(PPU_Dot), F(PPU_VRegisterChangedOutOfVBlank),
        F(PPU_OAMCorruptionRenderingDisabledOutOfVBlank), F(PPU_PendingOAMCorruption),
        F(PPU_OAMCorruptionIndex), F(PPU_OAMCorruptionRenderingDisabledOutOfVBlank_Instant),
        F(PPU_OAMCorruptionRenderingEnabledOutOfVBlank), F(PPU_OAMEvaluationCorruptionOddCycle),
        F(PPU_OAMEvaluationObjectInRange), F(PPU_OAMEvaluationObjectInXRange),
        F(PPU_PaletteCorruptionRenderingDisabledOutOfVBlank), F(PPU_AttributeLatchRegister),
        F(PPU_BackgroundAttributeShiftRegisterL), F(PPU_BackgroundAttributeShiftRegisterH),
        F(PPU_BackgroundPatternShiftRegisterL), F(PPU_BackgroundPatternShiftRegisterH), F(PPU_FineXScroll),
        F(PPU_SpriteShiftRegisterL), F(PPU_SpriteShiftRegisterH), F(PPU_SpriteAttribute), F(PPU_SpriteAttr),
        F(PPU_SpritePattern), F(PPU_SpriteXposition), F(PPU_SpriteShifterCounter),
        F(PPU_NextScanlineContainsSpriteZero), F(PPU_CurrentScanlineContainsSpriteZero), F(PPU_SpritePatternL),
        F(PPU_SpritePatternH), F(PPU_Mask_Greyscale), F(PPU_Mask_8PxShowBackground), F(PPU_Mask_8PxShowSprites),
        F(PPU_Mask_ShowBackground), F(PPU_Mask_ShowSprites), F(PPU_Mask_EmphasizeRed),
        F(PPU_Mask_EmphasizeGreen), F(PPU_Mask_EmphasizeBlue), F(PPU_Mask_ShowBackground_Delayed),
        F(PPU_Mask_ShowSprites_Delayed), F(PPU_Mask_ShowBackground_Instant), F(PPU_Mask_ShowSprites_Instant),
        F(PPU_RenderingCounter), F(PPU_LowBitPlane), F(PPU_HighBitPlane), F(PPU_Attribute),
        F(PPU_PatternAddressRegister_CHR), F(PPU_PatternAddressRegister_NT), F(PPU_PatternAddressRegister_AT),
        F(PPU_PAR_MUX), F(PPU_CanDetectSpriteZeroHit), F(PPU_OddFrame), F(DotColor), F(PrevDotColor),
        F(PrevPrevDotColor), F(PrevPrevPrevDotColor), F(PrevPrevPrevPrevDotColor), F(PaletteRAMAddress),
        F(ThisDotReadFromPaletteRAM), F(IRQ_LevelDetector), F(NMILine), F(CopyV), F(SkippedPreRenderDot341),
        F(PPUActiveForShiftRegisterUpdate), F(PPU_2007_Read), F(PPU_2007_Read_SR), F(PPU_2007_Read_Latches),
        F(PPU_2007_PD_RB), F(PPU_2007_ReadALE), F(PPU_2007_Read_H0_Latch), F(PPU_2007_Read_XRB), F(PPU_READ),
        F(PPU_2007_Write), F(PPU_2007_Write_SR), F(PPU_2007_Write_Latches), F(PPU_2007_DB_PAR),
        F(PPU_2007_WriteALE), F(PPU_2007_TStep_Latch), F(PPU_2007_TStep), F(PPU_2007_BLNK_Latch),
        F(PPU_2007_PaletteRAMEnable), F(PPU_2007_WriteData), F(PPU_WRITE), F(PPU_DecodeSignal),
        F(PPU_ShowScreenBorders), F(PPU_ShowRawNTSCSignal), F(OamCorruptedOnOddCycle), F(PPU_OAMBuffer_In),
        F(PPU_OAMBuffer), F(PPU_OAMLatch), F(PPU_OAM_VerticalOffset), F(NineObjectsOnThisScanline),
        F(OAM2Overflowed), F(OAM2ResetSignal), F(PPU_RenderTemp), F(PPU_Commit_NametableFetch),
        F(PPU_Commit_AttributeFetch), F(PPU_Commit_PatternLowFetch), F(PPU_Commit_PatternHighFetch),
        F(OAMDMA_Aligned), F(OAMDMA_Halt), F(DMCDMA_Halt), F(OAM_InternalBus), F(OAMAddressBus),
        F(PPU_VRAM_MysteryAddress), F(PPU_AddressBus), F(PPU_ALE), F(PPU_OctalLatch), F(PPU_v), F(PPU_t),
        F(PPU_Update2006Delay), F(PPU_Update2005Delay), F(PPU_Update2005Value), F(PPU_Update2001Value),
        F(PPU_Update2006Value), F(PPU_Update2006Value_Temp), F(PPU_Update2001Delay),
        F(PPU_Update2001OAMCorruptionDelay), F(PPU_Update2001EmphasisBitsDelay),
        F(PPU_WasRenderingBefore2001Write), F(PPU_ReadBuffer), F(PPUAddrLatch), F(PPUControlIncrementMode32),
        F(PPUControl_NMIEnabled), F(PPU_PatternSelect_Sprites), F(PPU_PatternSelect_Background),
        F(PPU_EXT_Enable), F(PPU_PendingVBlank), F(dataPinsAreNotFloating), F(SyncFM2),
        // NESRecomp: the mapper's internal state (tric_prelude.inc).
        F(Cart.MapperChip.PRG_Off), F(Cart.MapperChip.CHR_Off), F(Cart.MapperChip.Mirroring),
        F(Cart.MapperChip.WRAM_Readable), F(Cart.MapperChip.WRAM_Writable), F(Cart.MapperChip.Shift),
        F(Cart.MapperChip.ShiftCount), F(Cart.MapperChip.Ctrl), F(Cart.MapperChip.Chr0),
        F(Cart.MapperChip.Chr1), F(Cart.MapperChip.PrgReg), F(Cart.MapperChip.BankSelect),
        F(Cart.MapperChip.Reg), F(Cart.MapperChip.MirrorReg), F(Cart.MapperChip.RamProtect),
        F(Cart.MapperChip.IrqLatch), F(Cart.MapperChip.IrqCounter), F(Cart.MapperChip.IrqReload),
        F(Cart.MapperChip.IrqEnable), F(Cart.MapperChip.IrqOut), F(Cart.MapperChip.A12),
        F(Cart.MapperChip.Latch)
};
#undef F

// What any NES model can be compared on at a frame boundary (cyc_trace.c).
uint64_t cyc_mem_state_hash(void)
{
    return cyc_mem_hash(totalCycles, RAM, VRAM, OAM, PaletteRAM, Cart.UsingCHRRAM ? Cart.CHRROM : NULL,
                        (size_t)Cart.CHRROM_Length,
                        Cart.MapperChip.HasWRAM ? Cart.MapperChip.WRAM : NULL, sizeof(Cart.MapperChip.WRAM),
                        cyc_frame_index_buffer);
}

void cyc_mem_state_dump(void *file)
{
    cyc_mem_dump(file, totalCycles, RAM, VRAM, OAM, PaletteRAM, Cart.UsingCHRRAM ? Cart.CHRROM : NULL,
                 (size_t)Cart.CHRROM_Length,
                 Cart.MapperChip.HasWRAM ? Cart.MapperChip.WRAM : NULL, sizeof(Cart.MapperChip.WRAM),
                 cyc_frame_index_buffer);
}

const char *cyc_hw_name(void) { return "tricnes"; }
const uint8_t *cyc_cpu_ram(void) { return RAM; }
void cyc_set_controller(int port, uint8_t buttons) { (port ? ControllerPort2 : ControllerPort1) = buttons; }
uint64_t cyc_cycle_count(void) { return totalCycles; }
const uint32_t *cyc_frame_argb(void) { return cyc_framebuffer; }
const uint16_t *cyc_frame_index(void) { return cyc_frame_index_buffer; }
bool cyc_audio_enable(int) { return false; }
size_t cyc_audio_read(int16_t *, size_t) { return 0; }

// TriCNES's internal hardware state (PPU/APU internals, interrupt lines, DMA
// latches, clocks), so that a change to how the hardware is clocked is checked
// too, not only its effect on memory and the picture. Comparable only while
// both sides run TriCNES's hardware. CPU internals are not included: CPUs are
// compared on what they do (cyc_trace.h) and on their registers
// (CycCpuState). Host/debug-only fields are left out.
uint64_t cyc_hw_state_hash(void)
{
    uint64_t h = (uint64_t)PPU_Scanline | ((uint64_t)PPU_Dot << 16) | ((uint64_t)PPUClock << 32) |
                 ((uint64_t)CPUClock << 40);
    uint64_t acc = 0;
    for (size_t k = 0; k < sizeof(cyc_hw_fields) / sizeof(cyc_hw_fields[0]); k++)
        for (size_t i = 0; i < cyc_hw_fields[k].n; i++)
            acc = acc * 131 + ((const unsigned char *)cyc_hw_fields[k].p)[i];
    return mix(h, acc);
}
// Every hashed hardware field, one per line, for finding the field behind a
// cyc_hw_state_hash() difference.
void cyc_hw_state_dump(void *file)
{
    FILE *f = (FILE *)file;
    for (size_t k = 0; k < sizeof(cyc_hw_fields) / sizeof(cyc_hw_fields[0]); k++) {
        fprintf(f, "%s", cyc_hw_fields[k].name);
        const unsigned char *b = (const unsigned char *)cyc_hw_fields[k].p;
        for (size_t i = 0; i < cyc_hw_fields[k].n; i++) fprintf(f, "%s%02X", i % 32 ? "" : " ", b[i]);
        fputc('\n', f);
    }
}

} // extern "C"
