// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#pragma once
#include <cstdint>

namespace waveform_generator
{
// USB task only. Route one byte at a time and stop while blocked; this leaves
// unread host bytes in the USB endpoint so its normal backpressure applies.
[[nodiscard]] bool tx_usb_active() noexcept;
[[nodiscard]] bool tx_usb_blocked() noexcept;
void tx_usb_feed(std::uint8_t byte) noexcept;
void tx_usb_service() noexcept;
void tx_usb_disconnect() noexcept;
}
