// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#pragma once
#include <stdint.h>
#define __DMB() do {} while (0)
typedef struct { uint32_t FUSE; } TestFuse;
typedef struct { TestFuse FUSEN[3]; } TestOcotp;
extern TestOcotp* OCOTP;
