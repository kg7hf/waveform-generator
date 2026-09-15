// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#include "waveform_msc.h"

#include "FreeRTOS.h"
#include "ff.h"
#include "diskio.h"
#include "fsl_common.h"
#include "fsl_sd.h"
#include "fsl_sd_disk.h"
#include "task.h"
#include "tusb.h"
#include "waveform_sd.h"

#include <stddef.h>
#include <string.h>

enum
{
    wfg_msc_lun = 0U,
    wfg_msc_block_size = 512U,
    wfg_scsi_synchronize_cache_10 = 0x35U,
    wfg_media_init_task_stack_words = 2048U,
};

#define WFG_MEDIA_UNKNOWN_STATUS UINT32_MAX

typedef struct wfg_media_runtime
{
    volatile wfg_media_state_t state;
    volatile wfg_media_error_t error;
    volatile uint32_t card_ready;
    volatile uint32_t msc_ready;
    volatile uint32_t block_count;
    volatile uint32_t block_size;
    volatile uint32_t init_attempts;
    volatile uint32_t host_init_status;
    volatile uint32_t host_detect_status;
    volatile uint32_t card_init_status;
    volatile uint32_t disk_init_status;
    volatile uint32_t test_ready_calls;
    volatile uint32_t not_ready_responses;
    volatile uint32_t read_calls;
    volatile uint32_t read_blocks;
    volatile uint32_t write_calls;
    volatile uint32_t write_blocks;
    volatile uint32_t read_errors;
    volatile uint32_t write_errors;
    volatile uint32_t invalid_requests;
    volatile uint32_t eject_requests;
    volatile uint32_t sync_cache_calls;
    volatile uint32_t ownership_handoffs;
} wfg_media_runtime_t;

static wfg_media_runtime_t s_media;
static StaticTask_t s_media_init_task_control;
static StackType_t s_media_init_task_stack[wfg_media_init_task_stack_words];
static TaskHandle_t s_media_init_task_handle;
static bool s_media_sd_host_initialized;
static bool s_media_sd_card_init_started;
AT_NONCACHEABLE_SECTION_ALIGN(static uint8_t s_msc_bounce[wfg_msc_block_size], 32U);

static void WFG_MediaPrepareInitializationAttemptLocked(void)
{
    s_media.state = kWFG_MediaInitializing;
    s_media.error = kWFG_MediaErrorNone;
    s_media.card_ready = 0U;
    s_media.msc_ready = 0U;
    s_media.block_count = 0U;
    s_media.block_size = 0U;
    s_media.init_attempts++;
    s_media.host_init_status = WFG_MEDIA_UNKNOWN_STATUS;
    s_media.host_detect_status = WFG_MEDIA_UNKNOWN_STATUS;
    s_media.card_init_status = WFG_MEDIA_UNKNOWN_STATUS;
    s_media.disk_init_status = WFG_MEDIA_UNKNOWN_STATUS;
}

static void WFG_MediaDeinitializeSdHost(void)
{
    if (s_media_sd_card_init_started)
    {
        SD_Deinit(&g_sd);
    }
    else if (s_media_sd_host_initialized || g_sd.isHostReady)
    {
        SD_HostDeinit(&g_sd);
    }
    memset(&g_sd, 0, sizeof(g_sd));
    s_media_sd_host_initialized = false;
    s_media_sd_card_init_started = false;
}

static void WFG_MediaLatchInitializationFault(wfg_media_error_t error)
{
    taskENTER_CRITICAL();
    s_media.error = error;
    s_media.state = kWFG_MediaFault;
    s_media.card_ready = 0U;
    s_media.msc_ready = 0U;
    taskEXIT_CRITICAL();
}

static bool WFG_MediaFailInitialization(wfg_media_error_t error)
{
    WFG_MediaDeinitializeSdHost();
    WFG_MediaLatchInitializationFault(error);
    return false;
}

static bool WFG_MediaMscReady(void)
{
    bool ready;
    taskENTER_CRITICAL();
    ready = (s_media.state == kWFG_MediaHost) &&
            (s_media.card_ready != 0U) && (s_media.msc_ready != 0U);
    taskEXIT_CRITICAL();
    return ready;
}

static void WFG_MediaRecordStatus(volatile uint32_t *target, uint32_t value)
{
    taskENTER_CRITICAL();
    *target = value;
    taskEXIT_CRITICAL();
}

static void WFG_MediaRecordInvalidRequest(bool write_request)
{
    taskENTER_CRITICAL();
    s_media.invalid_requests++;
    if (write_request)
    {
        s_media.write_errors++;
    }
    else
    {
        s_media.read_errors++;
    }
    taskEXIT_CRITICAL();
}

static bool WFG_MediaValidateIo(uint8_t lun, uint32_t lba, uint32_t offset,
                                const void *buffer, uint32_t bytes,
                                uint32_t *blocks, bool write_request)
{
    const uint32_t block_count = s_media.block_count;
    const uint32_t block_size = s_media.block_size;
    if (lun != wfg_msc_lun || buffer == NULL || block_size != wfg_msc_block_size ||
        offset != 0U || bytes == 0U || bytes > sizeof(s_msc_bounce) ||
        (bytes % block_size) != 0U)
    {
        WFG_MediaRecordInvalidRequest(write_request);
        return false;
    }

    *blocks = bytes / block_size;
    if (lba >= block_count || *blocks > block_count - lba)
    {
        WFG_MediaRecordInvalidRequest(write_request);
        return false;
    }
    return true;
}

bool WFG_MediaInitialize(void)
{
    taskENTER_CRITICAL();
    if (s_media.state == kWFG_MediaHost)
    {
        taskEXIT_CRITICAL();
        return true;
    }
    if (s_media.state == kWFG_MediaUninitialized)
    {
        WFG_MediaPrepareInitializationAttemptLocked();
    }
    else if (s_media.state != kWFG_MediaInitializing)
    {
        taskEXIT_CRITICAL();
        return false;
    }
    taskEXIT_CRITICAL();

    WFG_MediaDeinitializeSdHost();
    WFG_SD_Config(&g_sd);
    const status_t host_status = SD_HostInit(&g_sd);
    WFG_MediaRecordStatus(&s_media.host_init_status, (uint32_t)host_status);
    if (host_status != kStatus_Success)
    {
        return WFG_MediaFailInitialization(kWFG_MediaErrorHostInit);
    }
    s_media_sd_host_initialized = true;

    SD_SetCardPower(&g_sd, false);
    SD_SetCardPower(&g_sd, true);

    uint32_t host_detect_status = WFG_MEDIA_UNKNOWN_STATUS;
    if (!WFG_SD_ReadHostDetectStatus(&g_sd, &host_detect_status))
    {
        return WFG_MediaFailInitialization(kWFG_MediaErrorHostInit);
    }
    WFG_MediaRecordStatus(&s_media.host_detect_status, host_detect_status);
    WFG_SD_PrepareDataTransfer(&g_sd);
    if (host_detect_status != (uint32_t)kSD_Inserted)
    {
        return WFG_MediaFailInitialization(kWFG_MediaErrorNoCard);
    }

    s_media_sd_card_init_started = true;
    const status_t card_status = SD_CardInit(&g_sd);
    WFG_MediaRecordStatus(&s_media.card_init_status, (uint32_t)card_status);
    if (card_status != kStatus_Success)
    {
        return WFG_MediaFailInitialization(kWFG_MediaErrorCardInit);
    }

    if (g_sd.blockCount == 0U || g_sd.blockSize != wfg_msc_block_size)
    {
        return WFG_MediaFailInitialization(kWFG_MediaErrorGeometry);
    }

    const DSTATUS disk_status = sd_disk_initialize(SDDISK);
    WFG_MediaRecordStatus(&s_media.disk_init_status, (uint32_t)disk_status);
    if (disk_status != 0U)
    {
        return WFG_MediaFailInitialization(kWFG_MediaErrorDiskInit);
    }

    taskENTER_CRITICAL();
    s_media.error = kWFG_MediaErrorNone;
    s_media.block_count = g_sd.blockCount;
    s_media.block_size = g_sd.blockSize;
    s_media.card_ready = 1U;
    s_media.msc_ready = 1U;
    s_media.state = kWFG_MediaHost;
    taskEXIT_CRITICAL();
    return true;
}

static void WFG_MediaInitTask(void *parameter)
{
    (void)parameter;
    for (;;)
    {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        (void)WFG_MediaInitialize();
    }
}

static bool WFG_MediaEnsureInitializationTask(void)
{
    if (s_media_init_task_handle != NULL)
    {
        return true;
    }

    s_media_init_task_handle = xTaskCreateStatic(&WFG_MediaInitTask, "wfg_media",
                                                 wfg_media_init_task_stack_words,
                                                 NULL, 1U,
                                                 s_media_init_task_stack,
                                                 &s_media_init_task_control);
    if (s_media_init_task_handle == NULL)
    {
        WFG_MediaLatchInitializationFault(kWFG_MediaErrorTaskStart);
        return false;
    }

    return true;
}

static bool WFG_MediaStartInitializationAttempt(bool allow_fault_retry,
                                                bool *started)
{
    bool accepted = false;

    if (started != NULL)
    {
        *started = false;
    }

    taskENTER_CRITICAL();
    if (s_media.state == kWFG_MediaHost || s_media.state == kWFG_MediaLocal ||
        s_media.state == kWFG_MediaInitializing)
    {
        accepted = true;
    }
    taskEXIT_CRITICAL();

    if (accepted)
    {
        return true;
    }

    if (!WFG_MediaEnsureInitializationTask())
    {
        return false;
    }

    taskENTER_CRITICAL();
    if (s_media.state == kWFG_MediaUninitialized ||
        (allow_fault_retry && s_media.state == kWFG_MediaFault))
    {
        WFG_MediaPrepareInitializationAttemptLocked();
        accepted = true;
        if (started != NULL)
        {
            *started = true;
        }
    }
    taskEXIT_CRITICAL();

    if (started != NULL && *started)
    {
        xTaskNotifyGive(s_media_init_task_handle);
    }
    return accepted;
}

bool WFG_MediaStartInitialization(void)
{
    bool started = false;
    return WFG_MediaStartInitializationAttempt(false, &started);
}

wfg_media_host_request_result_t WFG_MediaRequestHost(void)
{
    if (WFG_MediaSetHost())
    {
        return kWFG_MediaHostRequestReady;
    }

    bool started = false;
    if (WFG_MediaStartInitializationAttempt(true, &started))
    {
        return kWFG_MediaHostRequestInitializing;
    }

    return kWFG_MediaHostRequestFailed;
}

bool WFG_MediaSetHost(void)
{
    bool accepted;
    taskENTER_CRITICAL();
    accepted = (s_media.state == kWFG_MediaHost ||
                s_media.state == kWFG_MediaLocal) &&
               s_media.card_ready != 0U;
    if (accepted)
    {
        if (s_media.state != kWFG_MediaHost)
        {
            s_media.ownership_handoffs++;
        }
        s_media.msc_ready = 1U;
        s_media.state = kWFG_MediaHost;
    }
    taskEXIT_CRITICAL();
    return accepted;
}

bool WFG_MediaSetLocal(void)
{
    bool accepted = false;
    taskENTER_CRITICAL();
    if (s_media.state == kWFG_MediaHost && s_media.card_ready != 0U)
    {
        /* All TinyUSB callbacks and CDC parsing run in the single USB task. */
        s_media.msc_ready = 0U;
        s_media.state = kWFG_MediaLocal;
        s_media.ownership_handoffs++;
        accepted = true;
    }
    else if (s_media.state == kWFG_MediaLocal)
    {
        accepted = true;
    }
    taskEXIT_CRITICAL();
    return accepted;
}

bool WFG_MediaIsLocal(void)
{
    bool local;
    taskENTER_CRITICAL();
    local = s_media.state == kWFG_MediaLocal && s_media.card_ready != 0U &&
            s_media.msc_ready == 0U;
    taskEXIT_CRITICAL();
    return local;
}

wfg_media_snapshot_t WFG_MediaGetSnapshot(void)
{
    wfg_media_snapshot_t snapshot;
    const wfg_sd_detect_sample_t cd_sample = WFG_SD_ReadDetectSample();
    taskENTER_CRITICAL();
    snapshot.state = (uint32_t)s_media.state;
    snapshot.error = (uint32_t)s_media.error;
    snapshot.card_ready = s_media.card_ready;
    snapshot.msc_ready = s_media.msc_ready;
#if defined(WFG_MSC_READ_ONLY)
    snapshot.msc_read_only = 1U;
#else
    snapshot.msc_read_only = 0U;
#endif
    snapshot.block_count = s_media.block_count;
    snapshot.block_size = s_media.block_size;
    snapshot.init_attempts = s_media.init_attempts;
    snapshot.host_init_status = s_media.host_init_status;
    snapshot.host_detect_status = s_media.host_detect_status;
    snapshot.card_init_status = s_media.card_init_status;
    snapshot.disk_init_status = s_media.disk_init_status;
    snapshot.test_ready_calls = s_media.test_ready_calls;
    snapshot.not_ready_responses = s_media.not_ready_responses;
    snapshot.read_calls = s_media.read_calls;
    snapshot.read_blocks = s_media.read_blocks;
    snapshot.write_calls = s_media.write_calls;
    snapshot.write_blocks = s_media.write_blocks;
    snapshot.read_errors = s_media.read_errors;
    snapshot.write_errors = s_media.write_errors;
    snapshot.invalid_requests = s_media.invalid_requests;
    snapshot.eject_requests = s_media.eject_requests;
    snapshot.sync_cache_calls = s_media.sync_cache_calls;
    snapshot.ownership_handoffs = s_media.ownership_handoffs;
    taskEXIT_CRITICAL();
    snapshot.cd_gpio3_level = cd_sample.gpio3_level;
    snapshot.cd_cm7_gpio3_level = cd_sample.cm7_gpio3_level;
    snapshot.cd_inserted = cd_sample.inserted;
    return snapshot;
}

uint32_t tud_msc_inquiry2_cb(uint8_t lun, scsi_inquiry_resp_t *response,
                             uint32_t buffer_size)
{
    static const char vendor[8] = {'W', 'F', 'G', ' ', ' ', ' ', ' ', ' '};
    static const char product[16] = {
#if defined(WFG_MSC_READ_ONLY)
        'R', 'T', '1', '1', '7', '0', ' ', 'S', 'D', ' ', 'R', 'E', 'A', 'D', ' ', ' '};
#else
        'R', 'T', '1', '1', '7', '0', ' ', 'S', 'D', ' ', 'L', 'O', 'A', 'D', 'E', 'R'};
#endif
    static const char revision[4] = {'0', '0', '0', '1'};
    (void)lun;
    if (response == NULL || buffer_size < sizeof(*response))
    {
        return 0U;
    }
    memcpy(response->vendor_id, vendor, sizeof(vendor));
    memcpy(response->product_id, product, sizeof(product));
    memcpy(response->product_rev, revision, sizeof(revision));
    return sizeof(*response);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    bool ready = false;
    taskENTER_CRITICAL();
    s_media.test_ready_calls++;
    ready = lun == wfg_msc_lun && s_media.state == kWFG_MediaHost &&
            s_media.card_ready != 0U && s_media.msc_ready != 0U;
    if (!ready)
    {
        s_media.not_ready_responses++;
    }
    taskEXIT_CRITICAL();
    if (!ready)
    {
        (void)tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3AU, 0x00U);
    }
    return ready;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    if (block_count == NULL || block_size == NULL)
    {
        return;
    }
    if (lun == wfg_msc_lun && WFG_MediaMscReady())
    {
        *block_count = s_media.block_count;
        *block_size = (uint16_t)s_media.block_size;
    }
    else
    {
        *block_count = 0U;
        *block_size = 0U;
    }
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start,
                           bool load_eject)
{
    (void)power_condition;
    if (lun != wfg_msc_lun)
    {
        return false;
    }
    if (load_eject && !start)
    {
        taskENTER_CRITICAL();
        s_media.eject_requests++;
        taskEXIT_CRITICAL();
    }
    /* Safe-eject is evidence only; MEDIA LOCAL performs ownership transfer. */
    return true;
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
#if defined(WFG_MSC_READ_ONLY)
    (void)lun;
    return false;
#else
    return lun == wfg_msc_lun && WFG_MediaMscReady();
#endif
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t buffer_size)
{
    uint32_t blocks = 0U;
    if (!WFG_MediaMscReady() ||
        !WFG_MediaValidateIo(lun, lba, offset, buffer, buffer_size, &blocks, false))
    {
        (void)tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3AU, 0x00U);
        return TUD_MSC_RET_ERROR;
    }

    const status_t status = SD_ReadBlocks(&g_sd, s_msc_bounce, lba, blocks);
    taskENTER_CRITICAL();
    s_media.read_calls++;
    if (status == kStatus_Success)
    {
        s_media.read_blocks += blocks;
    }
    else
    {
        s_media.read_errors++;
    }
    taskEXIT_CRITICAL();
    if (status != kStatus_Success)
    {
        return TUD_MSC_RET_ERROR;
    }
    memcpy(buffer, s_msc_bounce, buffer_size);
    return (int32_t)buffer_size;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t buffer_size)
{
#if defined(WFG_MSC_READ_ONLY)
    (void)lba;
    (void)offset;
    (void)buffer;
    (void)buffer_size;
    taskENTER_CRITICAL();
    s_media.write_calls++;
    s_media.write_errors++;
    taskEXIT_CRITICAL();
    (void)tud_msc_set_sense(lun, SCSI_SENSE_DATA_PROTECT, 0x27U, 0x00U);
    return TUD_MSC_RET_ERROR;
#else
    uint32_t blocks = 0U;
    if (!WFG_MediaMscReady() ||
        !WFG_MediaValidateIo(lun, lba, offset, buffer, buffer_size, &blocks, true))
    {
        (void)tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3AU, 0x00U);
        return TUD_MSC_RET_ERROR;
    }

    memcpy(s_msc_bounce, buffer, buffer_size);
    const status_t status = SD_WriteBlocks(&g_sd, s_msc_bounce, lba, blocks);
    taskENTER_CRITICAL();
    s_media.write_calls++;
    if (status == kStatus_Success)
    {
        s_media.write_blocks += blocks;
    }
    else
    {
        s_media.write_errors++;
    }
    taskEXIT_CRITICAL();
    return status == kStatus_Success ? (int32_t)buffer_size : TUD_MSC_RET_ERROR;
#endif
}

uint8_t tud_msc_get_maxlun_cb(void)
{
    return 0U;
}

int32_t tud_msc_scsi_cb(uint8_t lun, const uint8_t command[16], void *buffer,
                        uint16_t buffer_size)
{
    (void)buffer;
    (void)buffer_size;
    if (lun == wfg_msc_lun && command != NULL &&
        command[0] == wfg_scsi_synchronize_cache_10)
    {
        taskENTER_CRITICAL();
        s_media.sync_cache_calls++;
        taskEXIT_CRITICAL();
        return WFG_MediaMscReady() ? 0 : TUD_MSC_RET_ERROR;
    }

    taskENTER_CRITICAL();
    s_media.invalid_requests++;
    taskEXIT_CRITICAL();
    (void)tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20U, 0x00U);
    return TUD_MSC_RET_ERROR;
}
