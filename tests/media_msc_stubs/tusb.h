// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Host stand-in for the part of TinyUSB's MSC device API that
// rt1170/platform/waveform_msc.c uses: the sense setter, the SCSI codes and the
// callback declarations (class/msc/msc.h and msc_device.h, TinyUSB 0.21).
// media_msc_host.c defines the sense setter and records the sense;
// tests/media_msc_tests.cpp drives the callbacks. As TinyUSB's own tusb.h does,
// it reads the firmware's configuration (rt1170/usb/tusb_config.h), so the
// test runs with the firmware's MSC chunk size (CFG_TUD_MSC_EP_BUFSIZE).

#pragma once

#include "tusb_config.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

// SPC sense keys, as TinyUSB names them.
enum
{
    SCSI_SENSE_NONE = 0x00,
    SCSI_SENSE_NOT_READY = 0x02,
    SCSI_SENSE_ILLEGAL_REQUEST = 0x05,
    SCSI_SENSE_DATA_PROTECT = 0x07,
};

// tud_msc_read10_cb / tud_msc_write10_cb failure.
enum
{
    TUD_MSC_RET_ERROR = -1,
};

// INQUIRY data: 8 bytes of flags, then the identification strings (36 bytes).
enum
{
    scsi_inquiry_flag_bytes = 8,
    scsi_vendor_id_bytes = 8,
    scsi_product_id_bytes = 16,
    scsi_product_rev_bytes = 4,
    scsi_command_bytes = 16,
};

typedef struct
{
    uint8_t flags[scsi_inquiry_flag_bytes];
    uint8_t vendor_id[scsi_vendor_id_bytes];
    uint8_t product_id[scsi_product_id_bytes];
    uint8_t product_rev[scsi_product_rev_bytes];
} scsi_inquiry_resp_t;

bool tud_msc_set_sense(uint8_t lun, uint8_t sense_key, uint8_t add_sense_code, uint8_t add_sense_qualifier);

uint32_t tud_msc_inquiry2_cb(uint8_t lun, scsi_inquiry_resp_t* response, uint32_t buffer_size);
bool tud_msc_test_unit_ready_cb(uint8_t lun);
void tud_msc_capacity_cb(uint8_t lun, uint32_t* block_count, uint16_t* block_size);
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject);
bool tud_msc_prevent_allow_medium_removal_cb(uint8_t lun, uint8_t prohibit_removal, uint8_t control);
bool tud_msc_is_writable_cb(uint8_t lun);
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void* buffer, uint32_t buffer_size);
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t buffer_size);
void tud_msc_write10_complete_cb(uint8_t lun);
uint8_t tud_msc_get_maxlun_cb(void);
int32_t tud_msc_scsi_cb(uint8_t lun, const uint8_t command[scsi_command_bytes], void* buffer, uint16_t buffer_size);

#ifdef __cplusplus
}
#endif
