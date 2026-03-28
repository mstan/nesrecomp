/*
 * game_config.h — Per-game recompiler configuration
 *
 * Loaded from games/<name>/game.cfg at recompile time.
 * Encapsulates all game-specific addresses that the generic recompiler
 * needs to handle correctly (dispatch tables, bank-switch trampolines, etc.).
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define GAME_CFG_MAX_TRAMPOLINES       8
#define GAME_CFG_MAX_KNOWN_TABLES     32
#define GAME_CFG_MAX_SPLIT_TABLES     16
#define GAME_CFG_MAX_EXTRA_FUNCS      2048
#define GAME_CFG_MAX_INLINE_DISPATCHES 8
#define GAME_CFG_MAX_INLINE_POINTERS   8
#define GAME_CFG_MAX_RAM_READ_HOOKS   16
#define GAME_CFG_MAX_BANK_SWITCHES     8
#define GAME_CFG_MAX_SRAM_MAPS         4
#define GAME_CFG_MAX_EXTRA_LABELS   1024

/*
 * Trampoline: a JSR whose operand address is a known bank-switch dispatch
 * routine. The bytes immediately after the JSR are inline data (not code).
 * inline_bytes: how many bytes after the JSR to consume as data (typ. 3).
 * bs_fn_addr:   the recompiled bank-switch function to call (e.g. 0xCC1A).
 */
typedef struct {
    uint16_t addr;          /* JSR target address (the trampoline entry) */
    int      inline_bytes;  /* extra data bytes following the JSR opcode  */
    uint16_t bs_fn_addr;    /* bank-switch function address in fixed bank  */
} TrampolineEntry;

/*
 * Known 2-byte little-endian dispatch table.
 * Each pair of bytes is (target-1) stored as a LE16.
 * start..end is the half-open byte range within the given bank.
 */
typedef struct {
    int      bank;
    uint16_t start;
    uint16_t end;
} KnownTable;

/*
 * Split dispatch table: lo-bytes and hi-bytes in separate arrays.
 * target = (hi[i] << 8 | lo[i]) + 1.
 * stride=1: packed; stride=2: interleaved pairs.
 */
typedef struct {
    int      bank;
    uint16_t lo_start;
    uint16_t hi_start;
    int      count;
    int      stride;
} KnownSplitTable;

/*
 * Extra function seed: a function that is dispatched dynamically and cannot
 * be discovered via static pointer/table scans.
 */
typedef struct {
    uint16_t addr;
    int      bank;   /* -1 for fixed bank */
} ExtraFunc;

/*
 * Inline indexed dispatch: JSR to a routine that pops the return address to
 * find an inline address table immediately after the JSR instruction.
 * The called routine dispatches via A register: entry = table[A].
 * Table entries are 2-byte little-endian absolute addresses.
 * Table ends when a hi byte < 0x80 is found (no valid ROM address).
 *
 * Usage in game.cfg:  inline_dispatch <hex_addr>
 * Example (SMB):      inline_dispatch 8E04
 */
typedef struct {
    uint16_t addr;  /* JSR target (the dispatch routine) */
} InlineDispatch;

typedef struct {
    uint16_t addr;
    uint8_t  zp_lo;
    uint8_t  zp_hi;
} InlinePointer;

/*
 * RAM read hook: when the generated code reads from this address via
 * absolute addressing, wrap the value through game_ram_read_hook(pc, addr, val).
 * This lets game-specific extras.c adjust the returned value per-call-site.
 *
 * Usage in game.cfg:  ram_read_hook <hex_addr>
 * Example (SMB):      ram_read_hook 071D
 */
typedef struct {
    uint16_t addr;
} RamReadHook;

/*
 * Bank-switch routine: a JSR target that switches the mapper's switchable
 * bank to the value currently in the A register.
 *
 * Usage in game.cfg:  bank_switch <hex_addr>
 * Example (Zelda):    bank_switch FFAC
 */
typedef struct {
    uint16_t addr;  /* JSR target address of bank-switch routine */
} BankSwitchRoutine;

/*
 * SRAM-to-ROM mapping: a region of SRAM that is a copy of ROM code.
 * When the walker encounters a JSR/JMP to an address in the SRAM range,
 * it translates to the corresponding ROM address and adds it as a function.
 * The ROM entry point is also seeded so internal calls get walked.
 *
 * Usage in game.cfg:  sram_map <sram_start> <rom_start> <bank> <size>
 * Example (Zelda):    sram_map 6C90 A500 1 1370
 */
typedef struct {
    uint16_t sram_start;  /* Start of SRAM region (e.g., $6C90) */
    uint16_t rom_start;   /* Corresponding ROM address (e.g., $A500) */
    int      bank;        /* ROM bank containing the source code */
    uint16_t size;        /* Size of the mapped region in bytes */
} SramMap;

typedef struct {
    char            output_prefix[64];  /* e.g. "faxanadu" → generated/faxanadu_full.c */
    char            annotations_path[512]; /* override for annotations.csv */

    TrampolineEntry trampolines[GAME_CFG_MAX_TRAMPOLINES];
    int             trampoline_count;

    KnownTable      known_tables[GAME_CFG_MAX_KNOWN_TABLES];
    int             known_table_count;

    KnownSplitTable known_split_tables[GAME_CFG_MAX_SPLIT_TABLES];
    int             known_split_table_count;

    ExtraFunc       extra_funcs[GAME_CFG_MAX_EXTRA_FUNCS];
    int             extra_func_count;

    InlineDispatch  inline_dispatches[GAME_CFG_MAX_INLINE_DISPATCHES];
    int             inline_dispatch_count;

    RamReadHook     ram_read_hooks[GAME_CFG_MAX_RAM_READ_HOOKS];
    int             ram_read_hook_count;

    BankSwitchRoutine bank_switches[GAME_CFG_MAX_BANK_SWITCHES];
    int              bank_switch_count;

    SramMap          sram_maps[GAME_CFG_MAX_SRAM_MAPS];
    int              sram_map_count;

    InlinePointer   inline_pointers[GAME_CFG_MAX_INLINE_POINTERS];
    int             inline_pointer_count;

    ExtraFunc        extra_labels[GAME_CFG_MAX_EXTRA_LABELS];
    int              extra_label_count;
} GameConfig;

/* Initialize to empty (no dispatch tables, prefix derived from ROM name) */
void game_config_init_empty(GameConfig *cfg);

/*
 * Load config from a game.cfg file.
 * Returns true on success; cfg remains empty on failure.
 * game_dir is set to the directory containing the file (for annotations lookup).
 */
bool game_config_load(GameConfig *cfg, const char *path);
