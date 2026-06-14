/*
 * debug_server_stub.c — production no-op stand-in for debug_server.c.
 *
 * Compiled instead of debug_server.c when NESRECOMP_ENABLE_TRACE is OFF (the
 * default / shipping build). It satisfies every public debug_server_* symbol
 * that the always-compiled runner + per-game extras.c reference, so the game
 * links cleanly while carrying NO TCP server, NO 36000-frame ring buffer, and
 * NO observability state. debug_server_init() opening nothing is the whole
 * point: a prod build never listens on a port.
 *
 * To get the real developer tooling back, configure with
 *   -DNESRECOMP_ENABLE_TRACE=ON
 * which compiles debug_server.c (the real implementation) in this file's place.
 * Keep the signatures here byte-identical to debug_server.c's public API.
 */
#include "debug_server.h"   /* NESFrameRecord, FrameDiffEntry */
#include "nes_runtime.h"    /* debug_server_request_pause prototype */
#include <stddef.h>         /* NULL */
#include <stdarg.h>
#include <stdint.h>

void debug_server_send_line(const char *json) { (void)json; }
void debug_server_send_fmt(const char *fmt, ...) { (void)fmt; }
void debug_server_init(int port) { (void)port; }
void debug_server_poll(void) {}
void debug_server_record_frame(void) {}
const NESFrameRecord *debug_server_get_frame_record(uint64_t frame) { (void)frame; return NULL; }
void debug_server_request_pause(const char *reason) { (void)reason; }
void debug_server_wait_if_paused(void) {}
void debug_server_shutdown(void) {}
int debug_server_is_connected(void) { return 0; }
int debug_server_get_input_override(void) { return -1; }
void debug_server_set_verify_result(int passed, int diff_count,
                                    const FrameDiffEntry *diffs, int n_diffs) {
    (void)passed; (void)diff_count; (void)diffs; (void)n_diffs;
}
