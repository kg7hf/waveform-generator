// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Host stand-in for the MCUXpresso common header, which
// rt1170/platform/waveform_msc.c includes for status_t. On the host the
// stand-in fsl_sd.h defines status_t, so nothing is left for this header. The
// bounce buffer's placement (a GNU section attribute) needs no stand-in.
// Only C units include this header.

#pragma once
