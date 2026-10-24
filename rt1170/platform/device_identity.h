// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// One board identity and fuse mapping for CDC, DD008 and USB descriptors.
// Callers using the fuse macros supply their device register declarations.
// The RT1170 default also serves the existing host register mocks.
#if defined(CPU_MIMXRT1064DVL6A)
#define WFG_BOARD_NAME "MIMXRT1064-EVK"
#define WFG_BOARD_LABEL "RT1064"
#define WFG_SERIAL_WORD0 (OCOTP->CFG0)
#define WFG_SERIAL_WORD1 (OCOTP->CFG1)
#else
#define WFG_BOARD_NAME "MIMXRT1170-EVK"
#define WFG_BOARD_LABEL "RT1170"
#define WFG_SERIAL_WORD0 (OCOTP->FUSEN[1].FUSE)
#define WFG_SERIAL_WORD1 (OCOTP->FUSEN[2].FUSE)
#endif
