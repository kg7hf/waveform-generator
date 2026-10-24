// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Host stand-in for the MCUXpresso register header, which TinyUSB's tusb_mcu.h
// includes for OPT_MCU_MIMXRT1XXX, as rt1170/usb/usb_tinyusb_port.c uses it:
// the USB PHY's and the USB OTG1 controller's registers it reads and writes,
// as plain memory in usb_port_host.c, with the RT1176 device header's bit
// positions; the USB interrupt's number; and NVIC_SetPriority(), which
// usb_port_host.c records.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct
{
    volatile uint32_t CTRL;
    volatile uint32_t PWD;
    volatile uint32_t TX;
} USBPHY_Type;

typedef struct
{
    volatile uint32_t FRINDEX;
    volatile uint32_t USBSTS;
    volatile uint32_t USBINTR;
    volatile uint32_t GPTIMER0LD;
    volatile uint32_t GPTIMER0CTRL;
    volatile uint32_t ENDPTSTAT;
    volatile uint32_t ENDPTCOMPLETE;
} USB_Type;

extern USBPHY_Type* const USBPHY1;
extern USB_Type* const USB_OTG1;

typedef enum
{
    USB_OTG1_IRQn = 136,
} IRQn_Type;

void NVIC_SetPriority(IRQn_Type irq, uint32_t priority);

// The fields the port reads and writes; the SDK spells a field's value as a
// function-like macro, which the stand-in keeps.
static const uint32_t USBPHY_CTRL_SET_ENUTMILEVEL2_MASK = 0x4000U;
static const uint32_t USBPHY_CTRL_SET_ENUTMILEVEL3_MASK = 0x8000U;
static const uint32_t USBPHY_TX_D_CAL_MASK = 0xFU;
static const uint32_t USBPHY_TX_TXCAL45DM_MASK = 0xF00U;
static const uint32_t USBPHY_TX_TXCAL45DP_MASK = 0xF0000U;
static const uint32_t USB_FRINDEX_FRINDEX_MASK = 0x3FFFU;
static const uint32_t USB_USBSTS_TI0_MASK = 0x1000000U;
static const uint32_t USB_USBINTR_TIE0_MASK = 0x1000000U;
static const uint32_t USB_GPTIMER0CTRL_GPTCNT_MASK = 0xFFFFFFU;
static const uint32_t USB_GPTIMER0CTRL_GPTRST_MASK = 0x40000000U;
static const uint32_t USB_GPTIMER0CTRL_GPTRUN_MASK = 0x80000000U;

#define USBPHY_TX_D_CAL(x) (((uint32_t)(x) << 0U) & USBPHY_TX_D_CAL_MASK)
#define USBPHY_TX_TXCAL45DM(x) (((uint32_t)(x) << 8U) & USBPHY_TX_TXCAL45DM_MASK)
#define USBPHY_TX_TXCAL45DP(x) (((uint32_t)(x) << 16U) & USBPHY_TX_TXCAL45DP_MASK)
#define USB_GPTIMER0LD_GPTLD(x) (((uint32_t)(x) << 0U) & 0xFFFFFFU)
#define USB_ENDPTSTAT_ETBR(x) (((uint32_t)(x) << 16U) & 0xFF0000U)
#define USB_ENDPTCOMPLETE_ERCE(x) (((uint32_t)(x) << 0U) & 0xFFU)
#define USB_ENDPTCOMPLETE_ETCE(x) (((uint32_t)(x) << 16U) & 0xFF0000U)

#ifdef __cplusplus
}
#endif
