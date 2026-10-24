// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

#include "wfg_status.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef enum wfg_media_state
{
    kWFG_MediaUninitialized = 0U,
    kWFG_MediaInitializing = 1U,
    kWFG_MediaHost = 2U,
    kWFG_MediaLocal = 3U,
    kWFG_MediaFault = 0xFFU,
} wfg_media_state_t;

typedef enum wfg_media_error
{
    kWFG_MediaErrorNone = 0U,
    kWFG_MediaErrorNoCard = 1U,
    kWFG_MediaErrorHostInit = 2U,
    kWFG_MediaErrorCardInit = 3U,
    kWFG_MediaErrorGeometry = 4U,
    kWFG_MediaErrorDiskInit = 5U,
    kWFG_MediaErrorTaskStart = 6U,
} wfg_media_error_t;

typedef enum wfg_media_host_request_result
{
    kWFG_MediaHostRequestReady = 0U,
    kWFG_MediaHostRequestInitializing = 1U,
    kWFG_MediaHostRequestFailed = 2U,
} wfg_media_host_request_result_t;

/* The answer to MEDIA LOCAL. The board takes the card only after the host has
   let go of its volume; a refusal changes nothing. A tag without a typedef:
   C writes enum wfg_media_local_request_result, C++ the bare name. C11 has no
   fixed underlying type, so its size is the target's (one byte on
   arm-none-eabi, short enums; an int on the host), the same for C and C++
   (wfg_status.h). */
enum wfg_media_local_request_result
{
    kWFG_MediaLocalGranted = 0U,     /* the board owns the card (now or already) */
    kWFG_MediaLocalHostWriting = 1U, /* a host WRITE(10) is between its blocks */
    kWFG_MediaLocalHostLocked = 2U,  /* the host prevents medium removal */
    kWFG_MediaLocalFailed = 3U,      /* the card is in neither Host nor Local ownership */
};

/* Bits of wfg_media_snapshot_t.msc_host_hold (STATUS msc_host_hold): what the
   host still holds of its volume. MEDIA LOCAL is granted only while it is 0. */
enum wfg_media_host_hold
{
    kWFG_MediaHostHoldNone = 0U,
    /* PREVENT ALLOW MEDIUM REMOVAL with PREVENT set: the host still uses the
       volume (Windows FAT is expected to send it while the volume is dirty,
       Linux while the device is open; Windows is a bench check). */
    kWFG_MediaHostHoldRemovalPrevented = 1U,
    /* A WRITE(10) has written some of its blocks and its status is not sent. */
    kWFG_MediaHostHoldWriting = 2U,
};

typedef struct wfg_media_snapshot
{
    uint32_t state;
    uint32_t error;
    uint32_t card_ready;
    uint32_t msc_ready;
    uint32_t msc_read_only;
    uint32_t block_count;
    uint32_t block_size;
    uint32_t init_attempts;
    uint32_t host_init_status;
    uint32_t host_detect_status;
    uint32_t card_init_status;
    uint32_t disk_init_status;
    uint32_t test_ready_calls;
    uint32_t not_ready_responses;
    uint32_t read_calls;
    uint32_t read_blocks;
    uint32_t write_calls;
    uint32_t write_blocks;
    uint32_t read_errors;
    uint32_t write_errors;
    uint32_t invalid_requests;
    uint32_t eject_requests;
    uint32_t sync_cache_calls;
    uint32_t ownership_handoffs;
    uint32_t cd_gpio3_level;
    uint32_t cd_cm7_gpio3_level;
    uint32_t cd_inserted;
    /* 1 after the host ejected the medium (START STOP UNIT, LoEj=1 Start=0):
       the LUN reports NOT READY / MEDIUM NOT PRESENT until the next accepted
       MEDIA HOST, or a board reset. A USB bus reset does not clear it. */
    uint32_t msc_ejected;
    /* enum wfg_media_host_hold bits; 0 when the host holds nothing. */
    uint32_t msc_host_hold;
    /* The host's transfers since boot (USB? only; STATUS has no room). TinyUSB
       hands a READ(10) or WRITE(10) over in chunks of its MSC buffer
       (tusb_config.h, 16 KB: 32 blocks), and each chunk is one SD command:
       read_calls and write_calls count chunks, read_blocks and write_blocks
       the blocks of the chunks that passed. The longest SD read and write of
       one chunk, passed or failed, in microseconds (0 until the first; the
       read-only LUN never writes), and the most blocks one chunk moved. */
    uint32_t msc_max_read_us;
    uint32_t msc_max_write_us;
    uint32_t msc_max_blocks_per_call;
    /* The SD bus clock the card's transfers run at, in Hz (USB? only), as
       SD_CardInit left it: its last step, the CMD6 switch to High Speed, sets
       49.5 MHz from the 198 MHz root (waveform_sd_clock.h). A card that does
       not support High Speed stays in Default Speed, 24.75 MHz, and still
       initialises with status 0; a WFG_SD_CLOCK_24MHZ image runs at 24 MHz.
       0 while no card is ready: before and during an initialisation, and
       after one failed. */
    uint32_t sd_bus_clock_hz;
} wfg_media_snapshot_t;

/* Start media initialization in a worker task so USB CDC stays responsive.
   kWFG_StatusOk: an attempt started from uninitialised, or the card is
   already ready, owned or initialising. kWFG_StatusMediaFault: the task could
   not be created (a task_start fault is latched). kWFG_StatusMediaRefused: a
   fault is latched, which only MEDIA HOST retries. */
__attribute__((warn_unused_result)) enum wfg_status WFG_MediaStartInitialization(void);

/* Initialize the EVK microSD once, then publish it in host/MSC mode (the
   initialisation task's attempt). kWFG_StatusOk: the host owns the card.
   kWFG_StatusMediaFault: a step failed and latched a fault (the snapshot's
   error says which). kWFG_StatusMediaRefused: the board owns the card or a
   fault is latched; nothing changes. */
__attribute__((warn_unused_result)) enum wfg_status WFG_MediaInitialize(void);

/* Ownership transitions only happen through CDC commands in the USB task.
   An accepted host request (ready or initializing) also clears the host-eject
   latch, so the host sees the medium again. */
wfg_media_host_request_result_t WFG_MediaRequestHost(void);
/* The host takes a ready card from Host or Local ownership: kWFG_StatusOk,
   else kWFG_StatusMediaRefused, changing nothing. */
__attribute__((warn_unused_result)) enum wfg_status WFG_MediaSetHost(void);

/* MEDIA LOCAL, in the USB task (the task that runs the MSC callbacks, so no
   SCSI command can start in between). From Host ownership the card moves to
   the board only while msc_host_hold is 0: never between two blocks of a host
   WRITE(10), and never while the host prevents medium removal. The host
   releases its hold by finishing the write and by dismounting or ejecting the
   volume (ALLOW MEDIUM REMOVAL); a new USB configuration or an unplug ends it
   too (WFG_MediaEndHostSession), and a Bulk-Only Mass Storage Reset ends a
   write the host gave up on (WFG_MediaControlTransferDone). Already Local:
   granted, nothing changes. A refusal changes nothing. */
enum wfg_media_local_request_result WFG_MediaRequestLocal(void);

/* A new USB configuration (SET_CONFIGURATION) or an unplug, in the USB task:
   the host's SCSI session is over, and with it its removal lock and any
   WRITE(10) it had started (SPC: a reset ends the prevention of removal). */
void WFG_MediaEndHostSession(void);

/* The status stage of a control transfer completed, in the USB task: its
   SETUP's bmRequestType, bRequest and the low byte of wIndex (the interface
   of an interface request). A Bulk-Only Mass Storage Reset of the MSC
   interface (usb/usb_composite.h) is the host's reset recovery after it gave
   up on a command: TinyUSB's MSC driver answers it without an MSC callback,
   so a WRITE(10) cancelled between its chunks, whose status is never sent,
   ends here and no longer holds the volume. The removal lock stays: the
   reset readies the interface for the next command, and the host still uses
   the volume. Any other request changes nothing. */
void WFG_MediaControlTransferDone(uint8_t request_type, uint8_t request, uint8_t interface_number);

bool WFG_MediaIsLocal(void);
wfg_media_snapshot_t WFG_MediaGetSnapshot(void);

#ifdef __cplusplus
}
#endif
