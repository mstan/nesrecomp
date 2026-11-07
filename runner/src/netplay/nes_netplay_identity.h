#ifndef NES_NETPLAY_IDENTITY_H
#define NES_NETPLAY_IDENTITY_H
/*
 * Netplay identity (recomp-ai-rules/NETPLAY.md §4: version identity must be
 * exact and machine-checked).
 *
 *   game_version        "<NESRECOMP_GAME_VERSION>+<16 hex of the exe hash>":
 *                       two builds from different trees never share it, so
 *                       the lobby keeps them apart before a match exists.
 *   content_fingerprint lower-case SHA-256 of the ROM image the runner loaded.
 *   build_fp/content_fp the same two, folded to 32 bits for the rollback
 *                       driver's in-band IDENT handshake.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Computed once; the ROM is registered by the runner when it loads it. */
const char *nes_netplay_identity_game_version(void);
void        nes_netplay_identity_set_rom(const uint8_t *rom, size_t len);
/* Same, from a file (the launcher knows the path before the runner loads it). */
int         nes_netplay_identity_set_rom_file(const char *path);
const char *nes_netplay_identity_rom_sha256(void);   /* "" before set_rom */
uint32_t    nes_netplay_identity_build_fp(void);
uint32_t    nes_netplay_identity_content_fp(void);
const uint8_t *nes_netplay_identity_build_sha256(void);
/* Full canonical image referenced in the lobby's 64-byte field (50 + NUL). */
void nes_netplay_session_reference(const char *text,char out[64]);

/* Distinct fixed simulation contracts; these never select/switch a backend. */
enum {NES_NET_PROFILE_CYCLE_LLE=1, NES_NET_PROFILE_LEGACY_C=2, NES_NET_PROFILE_CYCLE_HLE=4};
uint32_t nes_netplay_cycle_profile(const char *implementation);
const char *nes_netplay_cycle_contract(const char *implementation);
int nes_netplay_profile_compatible(uint32_t selected,uint32_t local,uint32_t remote);

/* SHA-256 (FIPS 180-4). out: 32 bytes. */
void nes_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
#endif
