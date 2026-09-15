// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#pragma once

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
} wfg_media_snapshot_t;

/* Start media initialization in a worker task so USB CDC stays responsive. */
bool WFG_MediaStartInitialization(void);

/* Initialize the EVK microSD once, then publish it in host/MSC mode. */
bool WFG_MediaInitialize(void);

/* Ownership transitions only happen through CDC commands in the USB task. */
wfg_media_host_request_result_t WFG_MediaRequestHost(void);
bool WFG_MediaSetHost(void);
bool WFG_MediaSetLocal(void);
bool WFG_MediaIsLocal(void);
wfg_media_snapshot_t WFG_MediaGetSnapshot(void);

#ifdef __cplusplus
}
#endif
