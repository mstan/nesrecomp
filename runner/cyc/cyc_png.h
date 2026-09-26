/* cyc_png.h - minimal PNG writer (uncompressed zlib stream). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Write width x height ARGB8888 pixels as an RGB PNG. */
bool cyc_write_png(const char *path, const uint32_t *argb, int width, int height);

#ifdef __cplusplus
}
#endif
