// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#pragma once

#include "common/status.hpp"

namespace waveform_generator
{

// Nominal 1 kHz deterministic PCM24 diagnostic. This is not the finite WAV
// player or a qualified source: must verify physical format and timing.
[[nodiscard]] m110::Status start_tone() noexcept;

} // namespace waveform_generator
