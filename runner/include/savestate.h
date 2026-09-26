#pragma once
#include <stddef.h>
#include <stdint.h>

/* Save current emulator state to a binary file.
 * Returns 1 on success, 0 on failure. */
int savestate_save(const char *path);

/* Load emulator state from a binary file.
 * Returns 1 on success, 0 on failure. */
int savestate_load(const char *path);

/*
 * In-memory form of the same image. The V7 file IS this byte stream, so the
 * file path and the rollback snapshot ring (nes_rb_state.c) share one
 * serializer and cannot cover different state.
 *
 * savestate_serialize returns the bytes written, or 0 when the image does not
 * fit in `cap` or a subsystem refused to serialize.
 * savestate_deserialize validates every section before touching the machine
 * (a rejected image leaves the running game unchanged), then applies it and
 * requests the guest-resume continuation exactly like savestate_load.
 */
#define SAVESTATE_LOAD_QUIET 1
size_t savestate_serialize(uint8_t *buf, size_t cap);
/* As above; *need receives the exact image size (also when it did not fit,
 * in which case 0 is returned and nothing past `cap` was written). */
size_t savestate_serialize_ex(uint8_t *buf, size_t cap, size_t *need);
int    savestate_deserialize(const uint8_t *buf, size_t len, int flags);
/* sizeof the fixed base struct (for partitioned digests and size guards). */
size_t savestate_base_size(void);
/* Rollback digest partitions within the image; see savestate.c. */
void savestate_partitions(size_t *cpu_end, size_t *ppu_end);
/* "ram+0x01a3", "mod[smb.coop]+0x0010", ... for a byte offset of an image. */
void savestate_describe_offset(const uint8_t *image, size_t len, size_t off,
                               char *out, size_t cap);
