// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// The Watterson stage's scenario rules, kept in watterson.cpp so the scenario
// dispatchers reach them through the one fallback branch each keeps for the stages
// whose rules live in their own files (QA, issue #20; the complexity ratchet of #38).
// Guide: docs/modules/signal-lab-watterson.md, "Scenario schema".

#include "common/status.hpp"
#include "signal_lab/json.hpp"
#include "signal_lab/scenario.hpp"
#include "signal_lab/watterson_params.hpp"

#include <cstdint>

namespace signal_lab
{

// Reads one {"type": "watterson", "paths": [...]} item into params. Unknown members
// of the stage or of a path are errors; every range is checked; then
// validate_watterson() runs. Messages are static (design 9.3).
[[nodiscard]] m110::Status parse_watterson_stage(const json::Document& document, const json::Value& object, WattersonParams& params) noexcept;

// The placement rules of stage `index`, a Watterson stage, and its parameters: at
// most one Watterson stage, and it must be the first (noise and interference follow
// the channel, E.4.1). validate_scenario() calls it for parsed and programmatic scenarios.
[[nodiscard]] m110::Status validate_watterson_stage(const Scenario& scenario, std::uint32_t index) noexcept;

} // namespace signal_lab
