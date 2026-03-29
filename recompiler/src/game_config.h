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
#define GAME_CFG_MAX_EXTRA_FUNCS      4096
#define GAME_CFG_MAX_INLINE_DISPATCHES 8
#define GAME_CFG_MAX_INLINE_POINTERS   8
#define GAME_CFG_MAX_NOP_JSRS          8
#define GAME_CFG_MAX_RAM_READ_HOOKS   16
#define GAME_CFG_MAX_BANK_SWITCHES     8
#define GAME_CFG_MAX_SRAM_MAPS         4
#define GAME_CFG_MAX_EXTRA_LABELS   1024
#define GAME_CFG_MAX_DATA_REGIONS    64
#define GAME_CFG_MAX_MERGE_FUNCS     16

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
    int      addr_adjust;   /* added to inline addr (1=RTS convention, 0=JMP indirect) */
    char     bank_reg;      /* register holding bank number: 'A' or 'X' (default 'X') */
    uint16_t bank_save_addr;/* address to read current bank from before switch */
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
    int      call;   /* 1 = also call the function after loading ZP */
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

/*
 * Data region: a range of PRG ROM that contains data, not code.
 * The pointer scanner will not scan these bytes for function pointers,
 * and the function walker will not try to walk them as code.
 *
 * Usage in game.cfg:  data_region <bank> <hex_start> <hex_end>
 * Example:            data_region 0 A200 A800
 */
typedef struct {
    int      bank;
    uint16_t start;
    uint16_t end;     /* exclusive */
} DataRegion;

/*
 * Merge functions: two function entry points that should be generated as
 * a single function body with multiple labels. JMPs between them become
 * goto statements instead of function calls. The lower address is the
 * canonical entry; the higher is a secondary label within the same body.
 *
 * Usage in game.cfg:  merge_func <bank> <addr1_hex> <addr2_hex>
 * Example:            merge_func 1 A046 A047
 */
typedef struct {
    int      bank;
    uint16_t addr_lo;   /* lower address (canonical function entry) */
    uint16_t addr_hi;   /* higher address (secondary entry label) */
} MergeFunc;

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

    uint16_t        nop_jsrs[GAME_CFG_MAX_NOP_JSRS]; /* JSR targets to skip entirely */
    int             nop_jsr_count;

    uint16_t        push_jsrs[GAME_CFG_MAX_NOP_JSRS]; /* JSR targets that need 6502 return addr pushed */
    int             push_jsr_count;

    ExtraFunc        extra_labels[GAME_CFG_MAX_EXTRA_LABELS];
    int              extra_label_count;

    DataRegion       data_regions[GAME_CFG_MAX_DATA_REGIONS];
    int              data_region_count;

    MergeFunc        merge_funcs[GAME_CFG_MAX_MERGE_FUNCS];
    int              merge_func_count;

    ExtraFunc        replace_funcs[GAME_CFG_MAX_EXTRA_FUNCS];  /* body provided by extras.c */
    int              replace_func_count;

    bool             push_all_jsr;  /* emit 6502 stack push/pop on every JSR/RTS */
} GameConfig;

/* Initialize to empty (no dispatch tables, prefix derived from ROM name) */
void game_config_init_empty(GameConfig *cfg);

/*
 * Load config from a game.cfg file.
 * Returns true on success; cfg remains empty on failure.
 * game_dir is set to the directory containing the file (for annotations lookup).
 */
bool game_config_load(GameConfig *cfg, const char *path);
