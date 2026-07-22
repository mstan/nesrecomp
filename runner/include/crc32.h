#pragma once
#include <stdint.h>
#include <stddef.h>

/* Compute CRC32 (IEEE 802.3) over a byte buffer. */
uint32_t crc32_compute(const uint8_t *data, size_t len);
typedef struct Crc32Context { uint32_t value; } Crc32Context;
void crc32_begin(Crc32Context *ctx);
void crc32_update(Crc32Context *ctx, const void *data, size_t len);
uint32_t crc32_end(const Crc32Context *ctx);
