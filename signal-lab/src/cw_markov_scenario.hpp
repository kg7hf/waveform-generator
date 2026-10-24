// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

// Strict parser for the optional CW Markov ON/OFF dwell distribution.

#include "signal_lab/json.hpp"
#include "signal_lab/scenario.hpp"

namespace signal_lab
{

[[nodiscard]] m110::Status parse_cw_markov_gate(const json::Document& document, const json::Value& object, CwParams& params) noexcept;

} // namespace signal_lab
