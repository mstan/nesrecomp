#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hdpack_jobs.h"
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;
typedef struct CycHdGpu CycHdGpu;

/* Optional presentation consumer. The native framebuffer and guest state are
 * never read back from this renderer. Destroy before the owning SDL renderer. */
CycHdGpu *cyc_hd_gpu_create(SDL_Renderer *renderer, SDL_Texture *activation);
void cyc_hd_gpu_destroy(CycHdGpu *gpu, bool context_lost);
SDL_Texture *cyc_hd_gpu_render(CycHdGpu *gpu, const HdJobFrame *frame,
                              unsigned requested_scale, uint64_t prepare_ns);
void cyc_hd_gpu_present_begin(CycHdGpu *gpu);
void cyc_hd_gpu_present_end(CycHdGpu *gpu);
/* Explicit QA readback only; ordinary presentation never allocates a shadow. */
bool cyc_hd_gpu_readback(CycHdGpu *gpu, uint32_t *pixels, size_t pitch_bytes);
unsigned cyc_hd_gpu_width(const CycHdGpu *gpu);
unsigned cyc_hd_gpu_height(const CycHdGpu *gpu);
