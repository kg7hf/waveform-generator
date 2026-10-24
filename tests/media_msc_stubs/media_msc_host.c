// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Host stand-ins for what rt1170/platform/waveform_msc.c calls: the EVK SD
// socket (waveform_sd.h), the NXP SD card API, the FatFs disk glue, TinyUSB's
// sense setter, the FreeRTOS task calls and the CPU cycle counter. The card has
// card_blocks blocks of 512 bytes held in memory; card initialisation leaves
// the bus clock the test set, a transfer moves its bytes, takes the time the
// test set (cycles and ticks), and fails only when the test says so.
// tests/media_msc_tests.cpp steers and reads them through media_msc_host.h.

#include "media_msc_host.h"

#include "cycle_counter.h"
#include "fsl_sd_disk.h"
#include "task.h"
#include "tusb.h"
#include "waveform_sd.h"

#include <string.h>

enum
{
    card_blocks = 2048,
    block_bytes = 512,
    sense_key_shift = 16,
    sense_code_shift = 8,
    // The board's CPU clock (SystemCoreClock after BOARD_BootClockRUN).
    cpu_clock_hz = 996000000,
};

typedef struct
{
    bool host_init_fails;
    bool detect_read_fails;
    bool card_removed;
    bool card_init_fails;
    bool geometry_set;
    bool disk_init_fails;
    bool task_create_fails;
    bool transfers_fail;
    uint32_t init_block_count;
    uint32_t init_block_size;
    uint32_t sd_host_inits;
    uint32_t sd_host_deinits;
    uint32_t sd_card_deinits;
    uint32_t task_notifications;
    uint32_t sd_reads;
    uint32_t sd_writes;
    uint32_t last_lba;
    uint32_t last_blocks;
    uint32_t sense;
    uint32_t cycles;
    TickType_t ticks;
    uint32_t transfer_cycles;
    TickType_t transfer_ticks;
    uint32_t init_bus_clock_hz;
    uint8_t card[(size_t)card_blocks * block_bytes];
} host_model_t;

static host_model_t* model(void)
{
    static host_model_t instance;
    return &instance;
}

// The SD card descriptor that the FatFs glue owns in the firmware.
sd_card_t g_sd;

void wfg_test_fail_sd_host_init(bool fail)
{
    model()->host_init_fails = fail;
}

void wfg_test_fail_sd_detect_read(bool fail)
{
    model()->detect_read_fails = fail;
}

void wfg_test_remove_card(bool removed)
{
    model()->card_removed = removed;
}

void wfg_test_fail_sd_card_init(bool fail)
{
    model()->card_init_fails = fail;
}

void wfg_test_set_card_geometry(uint32_t block_count, uint32_t block_size)
{
    model()->geometry_set = true;
    model()->init_block_count = block_count;
    model()->init_block_size = block_size;
}

void wfg_test_fail_disk_init(bool fail)
{
    model()->disk_init_fails = fail;
}

void wfg_test_fail_task_create(bool fail)
{
    model()->task_create_fails = fail;
}

uint32_t wfg_test_sd_host_inits(void)
{
    return model()->sd_host_inits;
}

uint32_t wfg_test_sd_host_deinits(void)
{
    return model()->sd_host_deinits;
}

uint32_t wfg_test_sd_card_deinits(void)
{
    return model()->sd_card_deinits;
}

uint32_t wfg_test_task_notifications(void)
{
    return model()->task_notifications;
}

void wfg_test_fail_sd_transfers(bool fail)
{
    model()->transfers_fail = fail;
}

void wfg_test_set_sd_transfer_time(uint32_t cycles, uint32_t ticks)
{
    model()->transfer_cycles = cycles;
    model()->transfer_ticks = ticks;
}

void wfg_test_set_sd_init_bus_clock(uint32_t hz)
{
    model()->init_bus_clock_hz = hz;
}

void wfg_test_set_sd_driver_bus_clock(uint32_t hz)
{
    g_sd.busClock_Hz = hz;
}

uint32_t wfg_test_sd_reads(void)
{
    return model()->sd_reads;
}

uint32_t wfg_test_sd_writes(void)
{
    return model()->sd_writes;
}

uint32_t wfg_test_last_sd_lba(void)
{
    return model()->last_lba;
}

uint32_t wfg_test_last_sd_blocks(void)
{
    return model()->last_blocks;
}

// Whether count bytes from block lba lie on the card.
static bool on_card(uint32_t lba, uint32_t count)
{
    const uint32_t capacity = (uint32_t)card_blocks * block_bytes;
    return lba < card_blocks && count <= capacity - lba * block_bytes;
}

bool wfg_test_put_card_bytes(uint32_t lba, const uint8_t* bytes, uint32_t count)
{
    if (!on_card(lba, count))
    {
        return false;
    }

    memcpy(&model()->card[(size_t)lba * block_bytes], bytes, count);
    return true;
}

bool wfg_test_get_card_bytes(uint32_t lba, uint8_t* bytes, uint32_t count)
{
    if (!on_card(lba, count))
    {
        return false;
    }

    memcpy(bytes, &model()->card[(size_t)lba * block_bytes], count);
    return true;
}

uint32_t wfg_test_last_sense(void)
{
    return model()->sense;
}

int32_t wfg_test_scsi_command(uint8_t lun, uint8_t operation_code)
{
    uint8_t command[scsi_command_bytes] = {0};
    command[0] = operation_code;
    return tud_msc_scsi_cb(lun, command, NULL, 0U);
}

int32_t wfg_test_scsi_without_command(uint8_t lun)
{
    return tud_msc_scsi_cb(lun, NULL, NULL, 0U);
}

void wfg_test_clear_sense(void)
{
    model()->sense = 0U;
}

uint32_t m110_platform_cycle_counter(void)
{
    return model()->cycles;
}

uint32_t m110_platform_cycle_counter_frequency_hz(void)
{
    return cpu_clock_hz;
}

TickType_t xTaskGetTickCount(void)
{
    return model()->ticks;
}

void WFG_SD_Config(sd_card_t* card)
{
    (void)card;
}

enum wfg_status WFG_SD_ReadHostDetectStatus(sd_card_t* card, uint32_t* detect_status)
{
    (void)card;

    if (model()->detect_read_fails)
    {
        return kWFG_StatusSdHostNotReady;
    }

    *detect_status = model()->card_removed ? kSD_Removed : kSD_Inserted;
    return kWFG_StatusOk;
}

void WFG_SD_PrepareDataTransfer(sd_card_t* card)
{
    (void)card;
}

wfg_sd_detect_sample_t WFG_SD_ReadDetectSample(void)
{
    const wfg_sd_detect_sample_t sample = {0U, 0U, 0U};
    return sample;
}

status_t SD_HostInit(sd_card_t* card)
{
    model()->sd_host_inits++;
    card->isHostReady = !model()->host_init_fails;
    return model()->host_init_fails ? kStatus_Fail : kStatus_Success;
}

void SD_HostDeinit(sd_card_t* card)
{
    model()->sd_host_deinits++;
    card->isHostReady = false;
}

void SD_Deinit(sd_card_t* card)
{
    model()->sd_card_deinits++;
    card->isHostReady = false;
}

void SD_SetCardPower(sd_card_t* card, bool enable)
{
    (void)card;
    (void)enable;
}

status_t SD_CardInit(sd_card_t* card)
{
    const host_model_t* const host = model();
    card->busClock_Hz = host->init_bus_clock_hz;
    card->blockCount = host->geometry_set ? host->init_block_count : (uint32_t)card_blocks;
    card->blockSize = host->geometry_set ? host->init_block_size : (uint32_t)block_bytes;
    return host->card_init_fails ? kStatus_Fail : kStatus_Success;
}

// One SD command: it is recorded and takes the transfer time, then fails as
// the test asked or when it would leave the card.
static bool sd_transfer(uint32_t startBlock, uint32_t blockCount)
{
    host_model_t* const host = model();
    host->last_lba = startBlock;
    host->last_blocks = blockCount;
    host->cycles += host->transfer_cycles;
    host->ticks += host->transfer_ticks;
    return !host->transfers_fail && blockCount != 0U && blockCount <= card_blocks && on_card(startBlock, blockCount * block_bytes);
}

status_t SD_ReadBlocks(sd_card_t* card, uint8_t* buffer, uint32_t startBlock, uint32_t blockCount)
{
    (void)card;
    model()->sd_reads++;

    if (!sd_transfer(startBlock, blockCount))
    {
        return kStatus_Fail;
    }

    memcpy(buffer, &model()->card[(size_t)startBlock * block_bytes], (size_t)blockCount * block_bytes);
    return kStatus_Success;
}

status_t SD_WriteBlocks(sd_card_t* card, const uint8_t* buffer, uint32_t startBlock, uint32_t blockCount)
{
    (void)card;
    model()->sd_writes++;

    if (!sd_transfer(startBlock, blockCount))
    {
        return kStatus_Fail;
    }

    memcpy(&model()->card[(size_t)startBlock * block_bytes], buffer, (size_t)blockCount * block_bytes);
    return kStatus_Success;
}

DSTATUS sd_disk_initialize(BYTE pdrv)
{
    // FatFs's STA_NOINIT for a disk that did not initialise.
    enum
    {
        disk_not_initialised = 0x01,
    };
    (void)pdrv;
    return model()->disk_init_fails ? (DSTATUS)disk_not_initialised : 0U;
}

bool tud_msc_set_sense(uint8_t lun, uint8_t sense_key, uint8_t add_sense_code, uint8_t add_sense_qualifier)
{
    (void)lun;
    model()->sense = ((uint32_t)sense_key << sense_key_shift) | ((uint32_t)add_sense_code << sense_code_shift) | add_sense_qualifier;
    return true;
}

// The media initialization task never runs here; the test calls
// WFG_MediaInitialize() in its place.
TaskHandle_t xTaskCreateStatic(void (*entry)(void*), const char* name, uint32_t stack_words, void* argument, UBaseType_t priority,
                               StackType_t* stack, StaticTask_t* control)
{
    (void)entry;
    (void)name;
    (void)stack_words;
    (void)argument;
    (void)priority;
    (void)stack;
    return model()->task_create_fails ? NULL : control;
}

void xTaskNotifyGive(TaskHandle_t task)
{
    (void)task;
    model()->task_notifications++;
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{
    (void)clear;
    (void)ticks;
    return 0U;
}
