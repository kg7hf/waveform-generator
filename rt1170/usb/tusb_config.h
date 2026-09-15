// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be supplied by the selected board build
#endif

#if defined(M110_TINYUSB_SINGLE_OWNER_OS_NONE)
#define CFG_TUSB_OS OPT_OS_NONE
#else
#define CFG_TUSB_OS OPT_OS_FREERTOS
#endif
#define CFG_TUSB_DEBUG 0

#if defined(M110_USB_HIGH_SPEED)
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_HIGH_SPEED)
#define CFG_TUD_MAX_SPEED OPT_MODE_HIGH_SPEED
#define M110_USB_CDC_ENDPOINT_SIZE 512
#else
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUD_MAX_SPEED OPT_MODE_FULL_SPEED
#define M110_USB_CDC_ENDPOINT_SIZE 64
#endif
#define CFG_TUD_ENABLED 1
#define CFG_TUD_ENDPOINT0_SIZE 64
#define CFG_TUD_VBUS_DETECT_HW 0

#ifndef M110_USB_DEVICE_EVENT_QUEUE_SIZE
#define M110_USB_DEVICE_EVENT_QUEUE_SIZE 16
#endif

#define CFG_TUD_TASK_QUEUE_SZ M110_USB_DEVICE_EVENT_QUEUE_SIZE

#ifndef M110_USB_CDC_RX_BUFFER_SIZE
#define M110_USB_CDC_RX_BUFFER_SIZE 512
#endif

#ifndef M110_USB_CDC_TX_BUFFER_SIZE
#define M110_USB_CDC_TX_BUFFER_SIZE 512
#endif

// One CDC-ACM function for the independent waveform fixture. The player
// image also exposes exactly one microSD-backed MSC LUN while the host owns it.
#define CFG_TUD_CDC 1
#define CFG_TUD_NCM 0
#define CFG_TUD_ECM_RNDIS 0
#if defined(WFG_PLAYER_IMAGE)
#define CFG_TUD_MSC 1
#define CFG_TUD_MSC_EP_BUFSIZE 512
#else
#define CFG_TUD_MSC 0
#endif
#define CFG_TUD_HID 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_MIDI2 0
#define CFG_TUD_VENDOR 0
#define CFG_TUD_AUDIO 0
#define CFG_TUD_VIDEO 0
#define CFG_TUD_DFU 0
#define CFG_TUD_DFU_RUNTIME 0
#define CFG_TUD_MTP 0
#define CFG_TUD_PRINTER 0
#define CFG_TUD_USBTMC 0
#define CFG_TUD_BTH 0

#define CFG_TUD_CDC_NOTIFY 1
#define CFG_TUD_CDC_RX_BUFSIZE M110_USB_CDC_RX_BUFFER_SIZE
#define CFG_TUD_CDC_TX_BUFSIZE M110_USB_CDC_TX_BUFFER_SIZE
#define CFG_TUD_CDC_RX_EPSIZE M110_USB_CDC_ENDPOINT_SIZE
#define CFG_TUD_CDC_TX_EPSIZE M110_USB_CDC_ENDPOINT_SIZE

#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(32)))

// TinyUSB 0.21 uses this FreeRTOS helper; the project's pinned FreeRTOS 10.6.2
// predates it. Keep the compatibility definition at the integration boundary.
#ifndef pdTICKS_TO_MS
#define pdTICKS_TO_MS(ticks) \
    ((uint32_t)(((uint64_t)(ticks) * 1000ULL) / (uint64_t)configTICK_RATE_HZ))
#endif

#ifdef __cplusplus
}
#endif
