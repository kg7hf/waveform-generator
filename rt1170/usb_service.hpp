// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#pragma once

#include "common/status.hpp"

namespace waveform_generator
{

// Initialize the USB owner task for CDC commands and player media service.
[[nodiscard]] m110::Status start_usb_service() noexcept;

} // namespace waveform_generator
