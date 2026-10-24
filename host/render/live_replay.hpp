// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

#include "common/status.hpp"
#include "signal_lab/live_control.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace waveform_generator
{

struct LiveReplay
{
    std::uint64_t seed{};
    double reference_rms{};
    bool has_frames{};
    std::uint64_t frames{};
    // Stable output-frame order; controls sharing a frame retain recorded order.
    std::vector<signal_lab::ControlEvent> events;
};

// Read an exported wfg-live.replay/1 plan into `replay`. Metadata and controls are
// checked before rendering; uint64 seed/frame tokens are parsed without a double
// roundtrip. Success, or why the plan is refused (a static message): io_error for a
// file that cannot be read or is over 1 MiB, invalid_argument for its content.
// A refused plan leaves `replay` as it was.
[[nodiscard]] m110::Status read_live_replay(const std::string& path, LiveReplay& replay);

} // namespace waveform_generator
