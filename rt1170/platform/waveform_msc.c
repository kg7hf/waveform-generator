// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#include "waveform_msc.h"

#include "FreeRTOS.h"
#include "cycle_counter.h"
#include "ff.h"
#include "diskio.h"
#include "fsl_common.h"
#include "fsl_sd.h"
#include "fsl_sd_disk.h"
#include "task.h"
#include "tusb.h"
#include "usb/usb_composite.h"
#include "waveform_sd.h"

#include <stddef.h>
#include <string.h>

enum
{
    wfg_msc_lun = 0U,
    wfg_msc_block_size = 512U,
    /* One chunk of a READ(10) or WRITE(10), at most. TinyUSB hands a command's
       data to tud_msc_read10_cb / tud_msc_write10_cb one MSC endpoint buffer at
       a time (CFG_TUD_MSC_EP_BUFSIZE, tusb_config.h): every chunk of a command
       but its last is full, and the next chunk's LBA follows on
       (msc_device.c). One chunk is one SD command through the bounce buffer:
       CMD18 / CMD25 for more than one block. */
    wfg_msc_transfer_bytes = CFG_TUD_MSC_EP_BUFSIZE,
    /* The ChipIdea device controller moves a transfer with one descriptor
       (dcd_ci_hs.c qtd_init): a 15-bit byte count and five 4 KB page pointers,
       the first at the buffer's own place in its page. 16 KB therefore fits
       from any buffer address; 32 KB fits neither field. */
    wfg_msc_transfer_max_bytes = 16384U,
    /* The bounce buffer's alignment: the Cortex-M7 cache line, which the SD
       host driver's cache maintenance by address works in, and a multiple of
       the 4 bytes the SD controller's ADMA2 needs for a data address. */
    wfg_msc_dma_alignment = 32U,
    wfg_scsi_synchronize_cache_10 = 0x35U,
    wfg_scsi_prevent_field_mask = 0x03U,
    wfg_media_init_task_stack_words = 2048U,
    wfg_microseconds_per_second = 1000000U,
    wfg_microseconds_per_tick = wfg_microseconds_per_second / configTICK_RATE_HZ,
};

_Static_assert(wfg_msc_transfer_bytes % wfg_msc_block_size == 0U,
               "a chunk is whole blocks, so every chunk of a command starts at a block boundary (offset 0)");
_Static_assert(wfg_msc_transfer_bytes <= wfg_msc_transfer_max_bytes,
               "the controller moves a chunk with one transfer descriptor from any buffer address");
_Static_assert(wfg_microseconds_per_second % configTICK_RATE_HZ == 0U, "a tick is a whole number of microseconds");
_Static_assert(sizeof(TickType_t) == sizeof(uint32_t), "tick differences wrap as uint32_t");

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
    /* Set by a host eject (START STOP UNIT, LoEj=1 Start=0), cleared by an
       accepted MEDIA HOST. While set the LUN reports NOT READY / MEDIUM NOT
       PRESENT, so the host cannot remount the volume it just ejected. */
    volatile uint32_t host_ejected;
    /* The host's hold on its volume, which keeps MEDIA LOCAL waiting. Set by
       PREVENT MEDIUM REMOVAL, cleared by ALLOW; set when a WRITE(10) chunk
       reaches the card, cleared when its status is sent or it fails. A new
       USB configuration or an unplug clears both. */
    volatile uint32_t host_prevents_removal;
    volatile uint32_t host_writing;
    /* Since boot: the longest SD read and write of one chunk, passed or
       failed, in microseconds, and the most blocks one chunk moved. */
    volatile uint32_t max_read_us;
    volatile uint32_t max_write_us;
    volatile uint32_t max_blocks_per_call;
} wfg_media_runtime_t;

/* A moment, for timing an SD transfer: the CPU cycle counter, exact but
   wrapping every 2^32 cycles (4.3 s at 996 MHz), and the RTOS tick, which no
   transfer wraps. */
typedef struct wfg_media_clock
{
    uint32_t cycles;
    TickType_t ticks;
} wfg_media_clock_t;

static wfg_media_runtime_t s_media;
static StaticTask_t s_media_init_task_control;
static StackType_t s_media_init_task_stack[wfg_media_init_task_stack_words];
static TaskHandle_t s_media_init_task_handle;
static bool s_media_sd_host_initialized;
static bool s_media_sd_card_init_started;

/* The SD side of every chunk, wfg_msc_transfer_bytes long: a READ(10) chunk is
   read from the card into it and copied to TinyUSB's buffer, a WRITE(10) chunk
   is copied from TinyUSB's buffer and written to the card from it. It holds a
   whole chunk, so a chunk is one SD command. The SD DMA uses only this buffer,
   never TinyUSB's, which the USB controller's DMA uses. Both lie in the
   non-cacheable OCRAM window (.ncache_ocram, MPU region 10; TinyUSB's through
   tusb_config.h), outside DTCM, so neither DMA needs cache maintenance. Only
   the USB task uses it, one chunk at a time. */
static uint8_t *WFG_MediaBounceBuffer(void)
{
    static _Alignas(wfg_msc_dma_alignment) uint8_t bounce[wfg_msc_transfer_bytes]
        __attribute__((section(".ncache_ocram.wfg_msc_bounce")));
    return bounce;
}

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

/* The initialisation's failure path: the SD host goes, the fault is latched. */
static __attribute__((warn_unused_result)) enum wfg_status WFG_MediaFailInitialization(wfg_media_error_t error)
{
    WFG_MediaDeinitializeSdHost();
    WFG_MediaLatchInitializationFault(error);
    return kWFG_StatusMediaFault;
}

static bool WFG_MediaMscReady(void)
{
    bool ready = false;
    taskENTER_CRITICAL();
    ready = (s_media.state == kWFG_MediaHost) &&
            (s_media.card_ready != 0U) && (s_media.msc_ready != 0U) &&
            (s_media.host_ejected == 0U);
    taskEXIT_CRITICAL();
    return ready;
}

#if !defined(WFG_MSC_READ_ONLY)
static bool WFG_MediaHostEjected(void)
{
    bool ejected = false;
    taskENTER_CRITICAL();
    ejected = s_media.host_ejected != 0U;
    taskEXIT_CRITICAL();
    return ejected;
}
#endif

static void WFG_MediaClearHostEject(void)
{
    taskENTER_CRITICAL();
    s_media.host_ejected = 0U;
    taskEXIT_CRITICAL();
}

/* The enum wfg_media_host_hold bits; the caller holds the critical section. */
static uint32_t WFG_MediaHostHoldLocked(void)
{
    uint32_t hold = kWFG_MediaHostHoldNone;

    if (s_media.host_prevents_removal != 0U)
    {
        hold |= kWFG_MediaHostHoldRemovalPrevented;
    }

    if (s_media.host_writing != 0U)
    {
        hold |= kWFG_MediaHostHoldWriting;
    }

    return hold;
}

static void WFG_MediaRecordHostWriting(bool writing)
{
    taskENTER_CRITICAL();
    s_media.host_writing = writing ? 1U : 0U;
    taskEXIT_CRITICAL();
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

/* One chunk's request: kWFG_StatusOk with its blocks, or
   kWFG_StatusInvalidArgument, counted as an invalid request and an error. */
static __attribute__((warn_unused_result)) enum wfg_status WFG_MediaValidateIo(uint8_t lun, uint32_t lba, uint32_t offset,
                                                                                 const void *buffer, uint32_t bytes,
                                                                                 uint32_t *blocks, bool write_request)
{
    const uint32_t block_count = s_media.block_count;
    const uint32_t block_size = s_media.block_size;
    if (lun != wfg_msc_lun || buffer == NULL || block_size != wfg_msc_block_size ||
        offset != 0U || bytes == 0U || bytes > wfg_msc_transfer_bytes ||
        (bytes % block_size) != 0U)
    {
        WFG_MediaRecordInvalidRequest(write_request);
        return kWFG_StatusInvalidArgument;
    }

    *blocks = bytes / block_size;
    if (lba >= block_count || *blocks > block_count - lba)
    {
        WFG_MediaRecordInvalidRequest(write_request);
        return kWFG_StatusInvalidArgument;
    }
    return kWFG_StatusOk;
}

static wfg_media_clock_t WFG_MediaClockNow(void)
{
    wfg_media_clock_t now;
    now.cycles = m110_platform_cycle_counter();
    now.ticks = xTaskGetTickCount();
    return now;
}

/* Saturates at UINT32_MAX microseconds (71 minutes). */
static uint32_t WFG_MediaTicksToMicroseconds(uint32_t ticks)
{
    return ticks <= UINT32_MAX / wfg_microseconds_per_tick ? ticks * wfg_microseconds_per_tick : UINT32_MAX;
}

/* Microseconds from start to now. The cycle counter measures them while it
   cannot have wrapped: n ticks between the two readings bound the time to less
   than n + 1 ticks, so n below the counter's period in whole ticks (4312 at
   996 MHz) is enough. A longer transfer, which only a failing card takes, is
   measured in ticks. */
static uint32_t WFG_MediaMicrosecondsSince(wfg_media_clock_t start)
{
    const wfg_media_clock_t now = WFG_MediaClockNow();
    const uint32_t ticks = now.ticks - start.ticks;
    const uint32_t cycles_per_us = m110_platform_cycle_counter_frequency_hz() / wfg_microseconds_per_second;

    if (cycles_per_us == 0U)
    {
        return WFG_MediaTicksToMicroseconds(ticks);
    }

    const uint32_t period_ticks = UINT32_MAX / cycles_per_us / wfg_microseconds_per_tick;
    if (ticks < period_ticks)
    {
        return (now.cycles - start.cycles) / cycles_per_us;
    }

    return WFG_MediaTicksToMicroseconds(ticks);
}

/* The SD bus clock of a ready card, 0 while none is; the caller holds the
   critical section. The SD driver keeps the clock in g_sd, and only an
   initialisation attempt writes it (SD_CardInit, or the reset of g_sd when
   an attempt starts or fails): each attempt clears card_ready in a critical
   section before it touches g_sd, and sets it in a critical section only
   after SD_CardInit has returned. While card_ready is 1 the clock therefore
   does not change, so the read needs no copy of its own in DTCM. */
static uint32_t WFG_MediaBusClockHzLocked(void)
{
    return s_media.card_ready != 0U ? g_sd.busClock_Hz : 0U;
}

/* One chunk's SD transfer, into the maxima; the caller holds the critical
   section. */
static void WFG_MediaRecordTransferLocked(volatile uint32_t *max_us, uint32_t elapsed_us,
                                          uint32_t blocks)
{
    if (elapsed_us > *max_us)
    {
        *max_us = elapsed_us;
    }

    if (blocks > s_media.max_blocks_per_call)
    {
        s_media.max_blocks_per_call = blocks;
    }
}

enum wfg_status WFG_MediaInitialize(void)
{
    taskENTER_CRITICAL();
    if (s_media.state == kWFG_MediaHost)
    {
        taskEXIT_CRITICAL();
        return kWFG_StatusOk;
    }
    if (s_media.state == kWFG_MediaUninitialized)
    {
        WFG_MediaPrepareInitializationAttemptLocked();
    }
    else if (s_media.state != kWFG_MediaInitializing)
    {
        taskEXIT_CRITICAL();
        return kWFG_StatusMediaRefused;
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
    if (WFG_SD_ReadHostDetectStatus(&g_sd, &host_detect_status) != kWFG_StatusOk)
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
    return kWFG_StatusOk;
}

static void WFG_MediaInitTask(void *parameter)
{
    (void)parameter;
    for (;;)
    {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        /* The outcome is the media state and error, which STATUS reports;
           the task has no caller to give it to. */
        const enum wfg_status outcome = WFG_MediaInitialize();
        (void)outcome;
    }
}

static __attribute__((warn_unused_result)) enum wfg_status WFG_MediaEnsureInitializationTask(void)
{
    if (s_media_init_task_handle != NULL)
    {
        return kWFG_StatusOk;
    }

    s_media_init_task_handle = xTaskCreateStatic(&WFG_MediaInitTask, "wfg_media",
                                                 wfg_media_init_task_stack_words,
                                                 NULL, 1U,
                                                 s_media_init_task_stack,
                                                 &s_media_init_task_control);
    if (s_media_init_task_handle == NULL)
    {
        WFG_MediaLatchInitializationFault(kWFG_MediaErrorTaskStart);
        return kWFG_StatusMediaFault;
    }

    return kWFG_StatusOk;
}

/* kWFG_StatusOk: the card is ready, owned or initialising, or an attempt
   started (*started). kWFG_StatusMediaFault: the task could not be created.
   kWFG_StatusMediaRefused: a fault is latched and allow_fault_retry is not
   set. */
static __attribute__((warn_unused_result)) enum wfg_status WFG_MediaStartInitializationAttempt(bool allow_fault_retry,
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
        return kWFG_StatusOk;
    }

    const enum wfg_status task = WFG_MediaEnsureInitializationTask();
    if (task != kWFG_StatusOk)
    {
        return task;
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
    return accepted ? kWFG_StatusOk : kWFG_StatusMediaRefused;
}

enum wfg_status WFG_MediaStartInitialization(void)
{
    bool started = false;
    return WFG_MediaStartInitializationAttempt(false, &started);
}

wfg_media_host_request_result_t WFG_MediaRequestHost(void)
{
    if (WFG_MediaSetHost() == kWFG_StatusOk)
    {
        return kWFG_MediaHostRequestReady;
    }

    bool started = false;
    if (WFG_MediaStartInitializationAttempt(true, &started) == kWFG_StatusOk)
    {
        WFG_MediaClearHostEject();
        return kWFG_MediaHostRequestInitializing;
    }

    return kWFG_MediaHostRequestFailed;
}

enum wfg_status WFG_MediaSetHost(void)
{
    bool accepted = false;
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
        s_media.host_ejected = 0U;
        s_media.state = kWFG_MediaHost;
    }
    taskEXIT_CRITICAL();
    return accepted ? kWFG_StatusOk : kWFG_StatusMediaRefused;
}

/* The answer to MEDIA LOCAL from Host ownership; the caller holds the critical
   section. The host's hold is checked before anything changes. */
static enum wfg_media_local_request_result WFG_MediaTakeFromHostLocked(void)
{
    if (s_media.host_writing != 0U)
    {
        return kWFG_MediaLocalHostWriting;
    }

    if (s_media.host_prevents_removal != 0U)
    {
        return kWFG_MediaLocalHostLocked;
    }

    /* All TinyUSB callbacks and CDC parsing run in the single USB task, so no
       SCSI command starts between this check and the handoff. */
    s_media.msc_ready = 0U;
    s_media.state = kWFG_MediaLocal;
    s_media.ownership_handoffs++;
    return kWFG_MediaLocalGranted;
}

enum wfg_media_local_request_result WFG_MediaRequestLocal(void)
{
    enum wfg_media_local_request_result result = kWFG_MediaLocalFailed;
    taskENTER_CRITICAL();

    if (s_media.state == kWFG_MediaLocal)
    {
        result = kWFG_MediaLocalGranted;
    }
    else if (s_media.state == kWFG_MediaHost && s_media.card_ready != 0U)
    {
        result = WFG_MediaTakeFromHostLocked();
    }

    taskEXIT_CRITICAL();
    return result;
}

void WFG_MediaEndHostSession(void)
{
    taskENTER_CRITICAL();
    s_media.host_prevents_removal = 0U;
    s_media.host_writing = 0U;
    taskEXIT_CRITICAL();
}

void WFG_MediaControlTransferDone(uint8_t request_type, uint8_t request, uint8_t interface_number)
{
    if (request_type == wfg_usb_request_type_class_interface_out && request == wfg_msc_request_bot_reset &&
        interface_number == wfg_usb_interface_msc)
    {
        WFG_MediaRecordHostWriting(false);
    }
}

bool WFG_MediaIsLocal(void)
{
    bool local = false;
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
    snapshot.msc_ejected = s_media.host_ejected;
    snapshot.msc_host_hold = WFG_MediaHostHoldLocked();
    snapshot.msc_max_read_us = s_media.max_read_us;
    snapshot.msc_max_write_us = s_media.max_write_us;
    snapshot.msc_max_blocks_per_call = s_media.max_blocks_per_call;
    snapshot.sd_bus_clock_hz = WFG_MediaBusClockHzLocked();
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
            s_media.card_ready != 0U && s_media.msc_ready != 0U &&
            s_media.host_ejected == 0U;
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
        /* The medium stays absent until MEDIA HOST, so Windows does not
           remount the volume it just ejected. */
        taskENTER_CRITICAL();
        s_media.eject_requests++;
        s_media.host_ejected = 1U;
        taskEXIT_CRITICAL();
    }
    /* Ownership still moves only through MEDIA LOCAL and MEDIA HOST. */
    return true;
}

/* PREVENT ALLOW MEDIUM REMOVAL (SBC-3 5.11): the PREVENT field is the low two
   bits of CDB byte 4; 00b allows removal, 01b prevents it, 1xb are obsolete
   and count as prevent. While the host prevents removal it still uses its
   volume, so MEDIA LOCAL waits (WFG_MediaRequestLocal). */
bool tud_msc_prevent_allow_medium_removal_cb(uint8_t lun, uint8_t prohibit_removal,
                                             uint8_t control)
{
    (void)control;

    if (lun != wfg_msc_lun)
    {
        return false;
    }

    const uint32_t prevent = prohibit_removal & wfg_scsi_prevent_field_mask;
    taskENTER_CRITICAL();
    s_media.host_prevents_removal = prevent != 0U ? 1U : 0U;
    taskEXIT_CRITICAL();
    return true;
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
#if defined(WFG_MSC_READ_ONLY)
    (void)lun;
    return false;
#else
    /* An ejected medium is absent, not write-protected: WRITE(10) then fails
       in tud_msc_write10_cb with NOT READY / MEDIUM NOT PRESENT, as TEST UNIT
       READY and READ(10) do. */
    return lun == wfg_msc_lun && (WFG_MediaMscReady() || WFG_MediaHostEjected());
#endif
}

/* One chunk of a READ(10) (wfg_msc_transfer_bytes at most, in this task): one
   SD read of all its blocks. A failed chunk fails the command. */
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t buffer_size)
{
    uint32_t blocks = 0U;
    if (!WFG_MediaMscReady() ||
        WFG_MediaValidateIo(lun, lba, offset, buffer, buffer_size, &blocks, false) != kWFG_StatusOk)
    {
        (void)tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3AU, 0x00U);
        return TUD_MSC_RET_ERROR;
    }

    uint8_t *const bounce = WFG_MediaBounceBuffer();
    const wfg_media_clock_t start = WFG_MediaClockNow();
    const status_t status = SD_ReadBlocks(&g_sd, bounce, lba, blocks);
    const uint32_t elapsed_us = WFG_MediaMicrosecondsSince(start);
    taskENTER_CRITICAL();
    s_media.read_calls++;
    WFG_MediaRecordTransferLocked(&s_media.max_read_us, elapsed_us, blocks);
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
    memcpy(buffer, bounce, buffer_size);
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
    /* TinyUSB delivers a WRITE(10) one chunk (one endpoint buffer,
       wfg_msc_transfer_bytes at most) per call, in this task, and each chunk
       is one SD write of all its blocks. Once a chunk is on the card the
       command holds the volume until tud_msc_write10_complete_cb; a failed
       chunk ends the command. */
    uint32_t blocks = 0U;

    if (!WFG_MediaMscReady() ||
        WFG_MediaValidateIo(lun, lba, offset, buffer, buffer_size, &blocks, true) != kWFG_StatusOk)
    {
        WFG_MediaRecordHostWriting(false);
        (void)tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3AU, 0x00U);
        return TUD_MSC_RET_ERROR;
    }

    uint8_t *const bounce = WFG_MediaBounceBuffer();
    memcpy(bounce, buffer, buffer_size);
    const wfg_media_clock_t start = WFG_MediaClockNow();
    const status_t status = SD_WriteBlocks(&g_sd, bounce, lba, blocks);
    const uint32_t elapsed_us = WFG_MediaMicrosecondsSince(start);
    taskENTER_CRITICAL();
    s_media.write_calls++;
    WFG_MediaRecordTransferLocked(&s_media.max_write_us, elapsed_us, blocks);
    if (status == kStatus_Success)
    {
        s_media.write_blocks += blocks;
        s_media.host_writing = 1U;
    }
    else
    {
        s_media.write_errors++;
        s_media.host_writing = 0U;
    }
    taskEXIT_CRITICAL();
    return status == kStatus_Success ? (int32_t)buffer_size : TUD_MSC_RET_ERROR;
#endif
}

/* After the status of every WRITE(10), passed or failed: the command no longer
   holds the volume. */
void tud_msc_write10_complete_cb(uint8_t lun)
{
    (void)lun;
    WFG_MediaRecordHostWriting(false);
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
