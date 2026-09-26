/* NES_RB_PROBE -- rollback determinism probe. See src/rollback/nes_rb_probe.c. */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void nes_rb_probe_init(void);
int  nes_rb_probe_active(void);
/* 1 while the probe's second (replayed) pass is running. */
int  nes_rb_probe_replaying(void);
void nes_rb_probe_top(void);
void nes_rb_probe_pre_tick(void);
#ifdef __cplusplus
}
#endif
