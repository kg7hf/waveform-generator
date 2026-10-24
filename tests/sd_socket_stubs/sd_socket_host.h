// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// The C++ test's view of rt1170/platform/waveform_sd.c and of the stand-ins in
// sd_socket_host.c: WFG_SD_ReadHostDetectStatus() on a card that
// WFG_SD_Config() set up, each way the test needs it, and the SD host's own
// card detect, which the test steers. Functions only, so the C++ test never
// compiles the C stand-in headers (NXP's SD types have C function pointer
// types).

#pragma once

#include "platform/wfg_status.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

// Whether WFG_SD_Config() gives the card the GPIO card detect.
bool wfg_test_socket_detects_by_gpio(void);

// WFG_SD_ReadHostDetectStatus()'s status on a card WFG_SD_Config() set up and
// whose host SD_HostInit() readied, writing to *status: through the socket's own
// card detect; a GPIO detect whose callback finds no card; a GPIO detect
// without its callback; the SD host's DATA3 detect; no card detect at all.
__attribute__((warn_unused_result)) enum wfg_status wfg_test_read_socket_detect(uint32_t* status);
__attribute__((warn_unused_result)) enum wfg_status wfg_test_read_gpio_detect_without_card(uint32_t* status);
__attribute__((warn_unused_result)) enum wfg_status wfg_test_read_gpio_detect_without_callback(uint32_t* status);
__attribute__((warn_unused_result)) enum wfg_status wfg_test_read_host_data3_detect(uint32_t* status);
__attribute__((warn_unused_result)) enum wfg_status wfg_test_read_without_detect(uint32_t* status);

// The same read with something missing: no card, no status to write, a host
// SD_HostInit() has not readied, a card with no host.
__attribute__((warn_unused_result)) enum wfg_status wfg_test_read_without_card(uint32_t* status);
__attribute__((warn_unused_result)) enum wfg_status wfg_test_read_without_status(void);
__attribute__((warn_unused_result)) enum wfg_status wfg_test_read_host_not_ready(uint32_t* status);
__attribute__((warn_unused_result)) enum wfg_status wfg_test_read_without_host(uint32_t* status);

// What SDMMCHOST_CardDetectStatus() answers from now on (kSD_Removed, 0, at
// start), and how often it was asked so far.
void wfg_test_set_host_card_detect(uint32_t status);
uint32_t wfg_test_host_card_detect_reads(void);

#ifdef __cplusplus
}
#endif
