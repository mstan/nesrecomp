/* Opt-in host diagnostics. No files, handlers or threads until enabled. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void cyc_diagnostics_sync(void);
const void *cyc_diagnostics_provider(const void *provider);
bool cyc_diagnostics_enable(void);
void cyc_diagnostics_disable(void);
bool cyc_diagnostics_active(void);
void cyc_diagnostics_note(const char *format, ...);
void cyc_diagnostics_error(const char *format, ...);
void cyc_diagnostics_frame(uint64_t frame, uint64_t cycles, unsigned pc);
/* Records the return code, including ordinary startup failures. */
void cyc_diagnostics_exit(int code);
#ifdef __cplusplus
}
#endif
