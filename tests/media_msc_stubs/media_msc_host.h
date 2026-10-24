// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The C++ test's view of rt1170/platform/waveform_msc.c and of the stand-ins in
// media_msc_host.c: the TinyUSB MSC callbacks it drives, and plain functions to
// steer the SD card and read what the stand-ins recorded. Functions only, so the
// C++ test never compiles the C stand-in headers.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

// The TinyUSB MSC callbacks of waveform_msc.c (TinyUSB 0.21 class/msc/msc_device.h).
bool tud_msc_test_unit_ready_cb(uint8_t lun);
void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count, uint16_t* block_size);
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject);
bool tud_msc_prevent_allow_medium_removal_cb(uint8_t lun, uint8_t prohibit_removal, uint8_t control);
bool tud_msc_is_writable_cb(uint8_t lun);
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t buffer_size);
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t buffer_size);
void tud_msc_write10_complete_cb(uint8_t lun);

// tud_msc_scsi_cb, the SCSI commands TinyUSB leaves to waveform_msc.c, as
// TinyUSB calls it: a CBW's command block whose first byte is
// `operation_code` (the rest zero), and no data stage; or no command block.
int32_t wfg_test_scsi_command(uint8_t lun, uint8_t operation_code);
int32_t wfg_test_scsi_without_command(uint8_t lun);

// Whether SD_HostInit() fails from now on (it succeeds at start).
void wfg_test_fail_sd_host_init(bool fail);

// The rest of an initialisation, each step as the firmware takes it, from
// now on: whether the socket's detect status cannot be read
// (WFG_SD_ReadHostDetectStatus() fails), whether the socket reports no card,
// whether SD_CardInit() fails, the geometry it leaves (2048 blocks of 512
// bytes at start), and whether sd_disk_initialize() fails. Each succeeds at
// start.
void wfg_test_fail_sd_detect_read(bool fail);
void wfg_test_remove_card(bool removed);
void wfg_test_fail_sd_card_init(bool fail);
void wfg_test_set_card_geometry(uint32_t block_count, uint32_t block_size);
void wfg_test_fail_disk_init(bool fail);

// Whether xTaskCreateStatic() fails from now on (it succeeds at start).
void wfg_test_fail_task_create(bool fail);

// SD_HostInit() calls, SD_HostDeinit() and SD_Deinit() calls, and
// xTaskNotifyGive() calls (an initialisation the task is told to run), so far.
uint32_t wfg_test_sd_host_inits(void);
uint32_t wfg_test_sd_host_deinits(void);
uint32_t wfg_test_sd_card_deinits(void);
uint32_t wfg_test_task_notifications(void);

// Whether SD_ReadBlocks() and SD_WriteBlocks() fail from now on (they succeed
// at start). A failed transfer still counts, records and takes its time. The
// board's SD_WriteBlocks() fails a write the card refuses only with
// patches/sdmmc-wfg.patch: upstream it sends a write that ACMD22 reports as
// unwritten again without end.
void wfg_test_fail_sd_transfers(bool fail);

// How long every SD transfer takes from now on: CPU cycles (996 MHz,
// m110_platform_cycle_counter_frequency_hz()) and RTOS ticks (1 ms). Both 0 at
// start. Each clock advances by its own amount, so a test can make the cycle
// counter wrap within a transfer.
void wfg_test_set_sd_transfer_time(uint32_t cycles, uint32_t ticks);

// The SD bus clock, in Hz, that SD_CardInit() leaves in the card descriptor
// (g_sd.busClock_Hz) from now on, as NXP's leaves the clock its last step
// chose; 0 at start.
void wfg_test_set_sd_init_bus_clock(uint32_t hz);

// Writes g_sd.busClock_Hz now, as an SD driver call in progress may while no
// card is ready.
void wfg_test_set_sd_driver_bus_clock(uint32_t hz);

// SD_ReadBlocks() and SD_WriteBlocks() calls so far, and the first block and
// block count of the latest one.
uint32_t wfg_test_sd_reads(void);
uint32_t wfg_test_sd_writes(void);
uint32_t wfg_test_last_sd_lba(void);
uint32_t wfg_test_last_sd_blocks(void);

// Copy count bytes onto or off the card from block lba, bypassing the SD API;
// false, copying nothing, when they would leave the card.
bool wfg_test_put_card_bytes(uint32_t lba, const uint8_t* bytes, uint32_t count);
bool wfg_test_get_card_bytes(uint32_t lba, uint8_t* bytes, uint32_t count);

// The sense the device last set through tud_msc_set_sense(), packed as
// key << 16 | additional code << 8 | qualifier; 0 after wfg_test_clear_sense().
uint32_t wfg_test_last_sense(void);
void wfg_test_clear_sense(void);

#ifdef __cplusplus
}
#endif
