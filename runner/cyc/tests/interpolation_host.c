/* A snapshot-only presenter over a real cycle host. Link wrappers observe
 * ownership; an unsafe read is counted and suppressed instead of racing. */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "cyc_core.h"
#include "cyc_host_extras.h"
#include "cyc_tcp.h"
#include <stdio.h>
#include <string.h>

static SDL_threadID main_thread;
static SDL_atomic_t emulating, worker_frames, unsafe_reads;
static uint32_t pixels[256 * 240];
static unsigned frames, phases;

void __real_cyc_run_frame(void);
void __wrap_cyc_run_frame(void)
{
    const bool worker = SDL_ThreadID() != main_thread;
    if (worker) {
        SDL_AtomicAdd(&worker_frames, 1);
        SDL_AtomicSet(&emulating, 1);
        SDL_Delay(10);
    }
    __real_cyc_run_frame();
    if (worker) SDL_AtomicSet(&emulating, 0);
}

void __real_cyc_zapper_state(CycZapperState *out);
void __wrap_cyc_zapper_state(CycZapperState *out)
{
    if (SDL_AtomicGet(&emulating)) {
        SDL_AtomicAdd(&unsafe_reads, 1);
        memset(out, 0, sizeof *out);
        return;
    }
    __real_cyc_zapper_state(out);
}

static void begin(void *ctx) { (void)ctx; main_thread = SDL_ThreadID(); }
static void end(void *ctx)
{
    (void)ctx;
    ++frames;
    for (unsigned i = 0; i < 256 * 240; ++i)
        pixels[i] = 0xFF001020u | ((frames & 127) << 16);
    SDL_Delay(6); /* Leaves real presentation work across a frame boundary. */
}
static const uint32_t *present(void *ctx, int *w, int *h)
{
    (void)ctx;
    *w = 256; *h = 240;
    return pixels; /* No machine access, including in-between pictures. */
}
static void phase(void *ctx, double value) { (void)ctx; (void)value; ++phases; }
static void stats(int id, const char *line)
{
    (void)line;
    char fields[256];
    snprintf(fields, sizeof fields,
             "\"worker_frames\":%d,\"unsafe_machine_reads\":%d,\"phases\":%u,\"frames\":%u",
             SDL_AtomicGet(&worker_frames), SDL_AtomicGet(&unsafe_reads), phases, frames);
    cyc_tcp_ok(id, fields);
}
static void setup(void *ctx) { (void)ctx; cyc_tcp_register("interpolation_test", "ownership counters", stats); }
static const CycHostExtras extras = {
    .present = present, .frame_begin = begin, .frame_end = end,
    .tcp_setup = setup, .present_rate = 240,
    .present_phase = phase, .present_from_snapshot = true,
};
const CycHostExtras *cyc_host_extras(void) { return &extras; }
