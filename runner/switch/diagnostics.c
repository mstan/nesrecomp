/* Switch has no /proc, desktop crash reporter or process-launching service. */
#include "cyc_diagnostics.h"
#include <stdarg.h>
#include <stdio.h>
void cyc_diagnostics_sync(void) {}
void cyc_diagnostics_startup(void) {}
const void *cyc_diagnostics_provider(const void *provider) { return provider; }
bool cyc_diagnostics_enable(void) { return false; }
void cyc_diagnostics_disable(void) {}
bool cyc_diagnostics_active(void) { return false; }
void cyc_diagnostics_note(const char *format, ...) {
    va_list ap; va_start(ap, format); vfprintf(stderr, format, ap);
    va_end(ap); fputc('\n', stderr);
}
void cyc_diagnostics_error(const char *format, ...) {
    va_list ap; va_start(ap, format); vfprintf(stderr, format, ap); va_end(ap);
}
void cyc_diagnostics_frame(uint64_t frame, uint64_t cycles, unsigned pc) {
    (void)frame; (void)cycles; (void)pc;
}
void cyc_diagnostics_exit(int code) { fprintf(stderr, "exit=%d\n", code); }
