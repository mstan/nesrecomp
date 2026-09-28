/*
 * hw_fds.h - the Famicom Disk System RAM Adapter and disk drive (hw_fds.c),
 * as the cartridge-side board hw_mapper.c dispatches to for mapper 20.
 * Hosts use cyc_core.h (cyc_load_fds and the cyc_fds_* drive controls).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cyc_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Power-on: registers cleared as Mesen's constructor leaves them
 * (FDS.h:22-66), the memory map set up by hw_mapper.c. */
void     fds_power_on(void);
/* CPU writes to $4020-$FFFF and reads of $4020-$7FFF. */
void     fds_cpu_write(uint16_t addr, uint8_t value);
bool     fds_cpu_read(uint16_t addr, uint8_t *value);
/* One CPU cycle of the timer and the drive, before the cycle's access. */
void     fds_cpu_clock(void);
bool     fds_irq(void);
/* Build the side streams from a disk image (cyc_load_fds, hw_machine.c). */
bool     cyc_fds_load_media(const uint8_t *image, size_t size, const CycFdsOptions *options);
uint64_t fds_state_hash(uint64_t acc);
/* The sound unit (hw_fds_audio.c): $4040-$408A writes and $4040-$4092
 * reads while $4023.1 enables them, one clock per CPU cycle (inside
 * fds_cpu_clock), its output level for the mixer, and the per-frame ring
 * summary. */
void     fds_audio_set_profile(CycFdsProfile profile);
void     fds_audio_power_on(void);
void     fds_audio_write(uint16_t addr, uint8_t value);
bool     fds_audio_read(uint16_t addr, uint8_t *value);
void     fds_audio_clock(void);
double   fds_audio_level(void);
void     fds_audio_frame_end(void);
void     fds_audio_state_dump(void *file);
uint64_t fds_media_hash(uint64_t h);
void     fds_state_dump(void *file);
/* The drive's eject/insert with the fds.side event's source (hw_fds.c). */
bool     fds_drive_eject(unsigned source);
bool     fds_drive_insert(unsigned side, unsigned source);
/* The HLE tier (hw_fds_hle.c). The bus and the drive report to it; with no
 * plan axis on it only observes (ring events) and changes nothing. */
void     fds_hle_power_on(void);
void     fds_hle_snoop(uint16_t addr);       /* every CPU read of $8000-$FFFF */
void     fds_hle_status_read(void);          /* $4032 read */
void     fds_hle_data(void);                 /* $4031 read or $4024 write */
void     fds_hle_transfer(void);             /* a byte clocked with $4025.1 (transfer reset) clear */
void     fds_hle_rewind(void);
void     fds_hle_ready(void);
void     fds_hle_host_disk_change(void);     /* a host eject or insert */
void     fds_hle_frame_end(void);
uint64_t fds_hle_state_hash(uint64_t acc);
/* The boot (hw_fds_boot.c): the BIOS's jump into the game, the boot skip's
 * stops, auto insert. */
void     fds_boot_power_on(void);
void     fds_boot_snoop(uint16_t addr);       /* every CPU read of $8000-$FFFF */
void     fds_boot_status_read(void);          /* $4032 read */
void     fds_boot_host_disk_change(void);     /* a host eject or insert */
void     fds_boot_frame_end(void);
uint64_t fds_boot_state_hash(uint64_t acc);
void     fds_boot_skipped(void);              /* the boot skip started this boot */
/* Stop the scheduler after the next execution of the instruction at pc
 * (hw_entry_hit); fds_boot_stop_taken() says (once) that it happened. */
void     fds_boot_stop_after(uint16_t pc);
bool     fds_boot_stop_taken(void);
/* The drive reports CRC mismatches ($4030.4) with these options (hw_fds.c). */
bool     fds_crc_reported(void);
/* The boot skip's access to the drive (hw_fds.c): put the drive in the state
 * the BIOS's boot load leaves it in once the head has run to the end of the
 * side: stopped at the end, $4025 as last written, the last byte transferred,
 * $4024 as last written, the last CRC check's result. */
void     fds_drive_boot_end(uint8_t ctrl, uint8_t read_data, uint8_t write_data, bool bad_crc);

#ifdef __cplusplus
}
#endif
