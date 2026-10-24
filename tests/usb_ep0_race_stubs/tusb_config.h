// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// TinyUSB's configuration for the EP0 race test (tests/usb_ep0_order_tests.cpp):
// the pinned TinyUSB's device stack (usbd.c) and CDC driver, built for the
// host, on the ChipIdea controller's numbers (OPT_MCU_MIMXRT1XXX), without an
// RTOS. usb_ep0_race.c stands in for the ChipIdea driver.

#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

#define CFG_TUSB_MCU OPT_MCU_MIMXRT1XXX
#define CFG_TUSB_OS OPT_OS_NONE
#define CFG_TUSB_DEBUG 0
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_HIGH_SPEED)
#define CFG_TUD_ENABLED 1
#define CFG_TUD_MAX_SPEED OPT_MODE_HIGH_SPEED
#define CFG_TUD_ENDPOINT0_SIZE 64
#define CFG_TUD_TASK_QUEUE_SZ 16
#define CFG_TUD_CDC 1
#define CFG_TUD_CDC_RX_BUFSIZE 512
#define CFG_TUD_CDC_TX_BUFSIZE 512
#define CFG_TUD_CDC_EP_BUFSIZE 512
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(32)))

// TinyUSB's assertions count here (usb_ep0_race.c), as the firmware's hook
// counts them for USB? (usb/tusb_config.h).
#define CFG_TUSB_DEBUG_BREAKPOINT wfg_race_count_assert

#ifdef __cplusplus
}
#endif
