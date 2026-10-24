// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Scenario grammar and placement rules for the bounded second-source overlay.

#include "signal_lab/json.hpp"
#include "signal_lab/scenario.hpp"

namespace signal_lab
{

[[nodiscard]] m110::Status parse_second_source_stage(const json::Document& document, const json::Value& object, SecondSourceParams& params, char (&source_path)[second_source_path_capacity]) noexcept;
[[nodiscard]] m110::Status validate_second_source_stage(const Scenario& scenario, std::uint32_t index) noexcept;

} // namespace signal_lab
