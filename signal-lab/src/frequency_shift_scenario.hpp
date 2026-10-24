// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Scenario grammar and placement rules for the whole-signal Doppler stage.
// Guide: docs/modules/signal-lab-engine.md.

#include "signal_lab/json.hpp"
#include "signal_lab/scenario.hpp"

namespace signal_lab
{

// Unknown, duplicate and mode-inapplicable members are rejected. Numeric ranges
// are checked by the same validator used by programmatically built scenarios.
[[nodiscard]] m110::Status parse_frequency_shift_stage(const json::Document& document, const json::Value& object, FrequencyShiftParams& params) noexcept;
[[nodiscard]] m110::Status validate_frequency_shift_stage(const Scenario& scenario, std::uint32_t index) noexcept;

} // namespace signal_lab
