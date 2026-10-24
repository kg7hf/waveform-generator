// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

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
// TinyUSB hands a READ(10) or WRITE(10) to the MSC callbacks one endpoint
// buffer at a time, so the buffer is the chunk platform/waveform_msc.c moves
// with one SD command: 16 KB is 32 blocks (CMD18 / CMD25) instead of one. It is
// the largest the ChipIdea controller carries in its single transfer
// descriptor from any buffer address (a 15-bit byte count and five 4 KB
// pages); waveform_msc.c checks it and sizes its SD bounce buffer from it.
// A literal, like every TinyUSB setting here: TinyUSB may test a setting in
// #if, where a named constant would silently read as 0.
#define CFG_TUD_CDC 1
#define CFG_TUD_NCM 0
#define CFG_TUD_ECM_RNDIS 0
#if defined(WFG_PLAYER_IMAGE)
#define CFG_TUD_MSC 1
#define CFG_TUD_MSC_EP_BUFSIZE 16384
#else
#define CFG_TUD_MSC 0
#endif

// TinyUSB places its device buffers with one attribute, CFG_TUD_MEM_SECTION,
// which each TinyUSB source expands for its own objects. In msc_device.c that
// is the MSC endpoint buffer (_mscd_epbuf, the 16 KB chunk) alone, and
// rt1170/CMakeLists.txt defines WFG_TUSB_MSC_DEVICE_UNIT for that source only,
// so the buffer moves out of DTCM into the non-cacheable OCRAM window
// (.ncache_ocram, MPU region 10; cmake/MIMXRT1176xxxxx_cm7_flexspi_nor_wfg.ld)
// and every other TinyUSB buffer stays where it was (.bss, in DTCM). Uncached,
// the buffer needs no cache maintenance. The ChipIdea driver (dcd_ci_hs.c
// qtd_init) cleans and invalidates a buffer only before each transfer: in
// cached memory a line the CPU filled speculatively while the controller wrote
// a CBW or WRITE(10) data would go stale. Here its maintenance finds no line.
#if defined(WFG_TUSB_MSC_DEVICE_UNIT)
#define CFG_TUD_MEM_SECTION __attribute__((section(".ncache_ocram.tusb_msc_epbuf")))
#elif defined(CPU_MIMXRT1064DVL6A)
// RT1064 ordinary state is cached OCRAM. Keep controller and other endpoint
// buffers in DTCM so speculative CPU reads cannot stale DMA-written packets.
#define CFG_TUD_MEM_SECTION __attribute__((section("NonCacheable")))
#endif
#define CFG_TUD_HID 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_MIDI2 0
#define CFG_TUD_VENDOR 0
#if defined(WFG_PLAYER_IMAGE)
// The player's composite device always carries one UAC2 function beside CDC
// and MSC: a single capture stream (EP IN only) at 48 kHz, whose alternate
// settings choose the format: stereo 24-bit samples in 4-byte subslots (384-byte
// packets), stereo 16-bit (192) or, by build option, mono 16-bit (96). Every
// packet is one USB millisecond, 48 frames, so flow control is off: the stream
// is synchronous to SOF, and TinyUSB sends at most the selected alternate's
// wMaxPacketSize per packet. The endpoint buffer is the largest packet; the
// software FIFO holds four of them, and the producer keeps two packets of the
// selected format queued. usb/usb_composite.h carries the same numbers for C
// and C++, and usb_audio_tinyusb.c checks that both agree.
#define CFG_TUD_AUDIO 1
#define CFG_TUD_AUDIO_ENABLE_EP_IN 1
#define CFG_TUD_AUDIO_ENABLE_EP_OUT 0
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP 0
#define CFG_TUD_AUDIO_ENABLE_INTERRUPT_EP 0
#define CFG_TUD_AUDIO_EP_IN_FLOW_CONTROL 0
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX 384
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ (4 * 384)
// TinyUSB's TU_ASSERT, and its direct TU_BREAKPOINT() calls, halt at a BKPT
// when a debugger is attached. Count the failure instead, and record where it
// happened (usb_audio.cpp; STATUS's usb_stack_asserts and the usb_*assert*
// fields of USB?), so an attached probe never stops a running stream. The
// failing function calls the hook itself; rt1170/CMakeLists.txt keeps each
// such call a call of its own (no sibling calls, tail merging or cross-jumping
// in TinyUSB).
#define CFG_TUSB_DEBUG_BREAKPOINT wfg_usb_count_stack_assert
#else
#define CFG_TUD_AUDIO 0
#endif
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
