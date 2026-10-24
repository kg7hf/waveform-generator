// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// CW pulse schedule parsing (`pulse_period_seconds` plus `pulse_duty_cycle`)
// and typed-descriptor validation for the scenario boundary.

#include "signal_lab/json.hpp"
#include "signal_lab/scenario.hpp"

namespace signal_lab
{

[[nodiscard]] m110::Status parse_cw_pulse_schedule(const json::Document& document, const json::Value& object, CwParams& params) noexcept;
[[nodiscard]] m110::Status validate_cw_pulse_timing(const CwParams& params) noexcept;

} // namespace signal_lab
