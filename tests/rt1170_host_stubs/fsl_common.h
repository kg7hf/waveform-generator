// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once
#include <stdint.h>
// One thread on the host: no barrier to issue.
#define __DMB() ((void)0)
typedef struct { uint32_t FUSE; } TestFuse;
typedef struct { TestFuse FUSEN[3]; } TestOcotp;
extern TestOcotp* OCOTP;
